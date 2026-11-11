#include "muse_wake.h"

#if CONFIG_MUSE_WAKE_WORD_HI_MUSE

#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "model_path.h"

#include "muse_audio.h"
#include "muse_mem.h"

static const char *TAG = "muse_wake";

#define MODEL_PARTITION "model"
#define MODEL_KEY "himuse"
#define RING_BYTES (MUSE_AUDIO_RATE * 2 / 2)   /* 0.5 s of mono 16-bit */
/* Idle reads come every 20 ms; a longer gap means the voice task was busy
 * with something else (and may have been speaking). */
#define IDLE_GAP_US 150000
#define COOLDOWN_US 2500000
#define WAKE_STACK_PSRAM 12288
#define WAKE_STACK_INTERNAL 8192

static const esp_wn_iface_t *s_wn;
static model_iface_data_t *s_data;
static StreamBufferHandle_t s_ring;
static atomic_bool s_hit;
static atomic_uint s_gen;            /* bumped at every gap: the detector then drops its partial chunk and state */
static atomic_llong s_hold_until;    /* feed ignored (and the detector's output discarded) until then */
static int64_t s_last_feed_us;       /* voice task only */

/* Liveness stats (INFO log every STATS_US). Feed side is written by the voice
 * task, detector side by the wake task; both are only read for the log. */
#define STATS_US 10000000
static atomic_uint s_fed;            /* chunks passed to muse_wake_feed() */
static atomic_uint s_sent;           /* chunks that went into the ring (not cooling down) */
static atomic_uint s_dropped;        /* chunks the ring had no room for */
static atomic_uint s_gaps;           /* idle gaps seen (each one = a cooldown) */
static atomic_uint s_max_gap_ms;     /* longest feed-to-feed gap this period */
static atomic_int s_last_db10;       /* dBFS x10 of the latest chunk */
static atomic_uint s_detects;        /* detector calls this period */
static atomic_uint s_max_detect_us;  /* slowest detect() this period */

static void wake_task(void *arg)
{
    int chunk = s_wn->get_samp_chunksize(s_data);
    int16_t *buf = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the detector buffer");
        vTaskDelete(NULL);
    }
    size_t have = 0;
    unsigned gen = atomic_load(&s_gen);
    int64_t next_stats = esp_timer_get_time() + STATS_US;
    for (;;) {
        /* Blocks until the voice task feeds idle audio (the timeout only
         * lets the liveness log run while nothing is fed). */
        size_t got = xStreamBufferReceive(s_ring, (uint8_t *)buf + have * 2, (chunk - have) * 2,
                                          pdMS_TO_TICKS(1000));
        if (esp_timer_get_time() >= next_stats) {
            next_stats += STATS_US;
            ESP_LOGI(TAG, "alive: fed=%u sent=%u dropped=%u gaps=%u maxgap=%ums level=%.1f dBFS detects=%u maxdetect=%uus stackfree=%uB",
                     atomic_load(&s_fed), atomic_load(&s_sent), atomic_load(&s_dropped), atomic_load(&s_gaps),
                     atomic_exchange(&s_max_gap_ms, 0), atomic_load(&s_last_db10) / 10.0f,
                     atomic_exchange(&s_detects, 0), atomic_exchange(&s_max_detect_us, 0),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        if (got == 0) continue;
        if (gen != atomic_load(&s_gen)) {
            gen = atomic_load(&s_gen);
            have = 0;
            s_wn->clean(s_data);
            continue;
        }
        have += got / 2;
        if (have < (size_t)chunk) continue;
        have = 0;
        int64_t t0 = esp_timer_get_time();
        int r = s_wn->detect(s_data, buf);
        unsigned dt = (unsigned)(esp_timer_get_time() - t0);
        atomic_fetch_add(&s_detects, 1);
        if (dt > atomic_load(&s_max_detect_us)) atomic_store(&s_max_detect_us, dt);
        if (r > 0 && esp_timer_get_time() < atomic_load(&s_hold_until)) {
            ESP_LOGI(TAG, "wake word heard during cooldown: ignored");
        }
        if (r > 0 && esp_timer_get_time() >= atomic_load(&s_hold_until)) {
            ESP_LOGI(TAG, "wake word detected (\"%s\")", s_wn->get_word_name(s_data, 1));
            atomic_store(&s_hit, true);
            /* Nothing more until the voice task has started the turn. */
            atomic_store(&s_hold_until, esp_timer_get_time() + COOLDOWN_US);
        }
    }
}

bool muse_wake_start(void)
{
    ESP_LOGI(TAG, "start: scheduler state %d (1=not started, 2=running), task \"%s\", core %d, in ISR %d",
             (int)xTaskGetSchedulerState(), pcTaskGetName(NULL), (int)xPortGetCoreID(), (int)xPortInIsrContext());
    muse_wake_warmup();   /* no-op if main already did it */
    ESP_LOGI(TAG, "start: free internal %u, psram %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "start: largest internal block %u, heap integrity %s",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             heap_caps_check_integrity_all(false) ? "ok" : "CORRUPT");
    /* The first C++ function-local static after the scheduler starts makes
     * the C++ runtime (__cxa_guard_acquire) create a mutex and a counting
     * semaphore, and it abort()s if either fails. esp-dl's
     * ModuleCreator::get_instance() is such a static. Try the same
     * allocations here first so a failure is a logged "wake word off", not a
     * reboot loop. */
    {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        SemaphoreHandle_t c = xSemaphoreCreateCounting(INT_MAX, 0);
        bool ok = m && c;
        if (m) vSemaphoreDelete(m);
        if (c) vSemaphoreDelete(c);
        if (!ok) {
            ESP_LOGE(TAG, "C++ static-init semaphores cannot be created (free internal %u, largest %u): wake word off",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            return false;
        }
        ESP_LOGI(TAG, "C++ static-init probe ok");
    }
    srmodel_list_t *models = esp_srmodel_init(MODEL_PARTITION);
    char *name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, MODEL_KEY) : NULL;
    if (!name) {
        ESP_LOGE(TAG, "wn10_himuse not found in the \"%s\" partition: wake word off", MODEL_PARTITION);
        return false;
    }
    s_wn = esp_wn_handle_from_name(name);
    ESP_LOGI(TAG, "create %s: begin (free internal %u)", name, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s_data = s_wn ? s_wn->create(name, DET_MODE_90) : NULL;
    ESP_LOGI(TAG, "create %s: %s (free internal %u)", name, s_data ? "returned" : "FAILED",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (!s_data) {
        ESP_LOGE(TAG, "could not create %s: wake word off", name);
        return false;
    }
    if (s_wn->get_samp_rate(s_data) != MUSE_AUDIO_RATE || s_wn->get_channel_num(s_data) != 1) {
        ESP_LOGE(TAG, "%s wants %d Hz x %d ch: wake word off", name, s_wn->get_samp_rate(s_data),
                 s_wn->get_channel_num(s_data));
        s_wn->destroy(s_data);
        s_data = NULL;
        return false;
    }
    s_ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_ring) s_ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MUSE_BIG_CAPS);   /* internal RAM is tight */
    if (!s_ring) {
        ESP_LOGE(TAG, "no memory for the audio ring (%d bytes; free internal %u): wake word off", RING_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        s_wn->destroy(s_data);
        s_data = NULL;
        return false;
    }
    atomic_store(&s_hold_until, esp_timer_get_time() + COOLDOWN_US);
    /* The stack goes in PSRAM: after create() the largest internal block is
     * only a few KB, too small for an internal stack. The detector runs
     * esp-dl on heap buffers only (no DMA, no ISR, no flash/cache-off work on
     * this task), so an external stack is safe; internal RAM is the fallback. */
    const char *where = "psram";
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(wake_task, "muse_wake", WAKE_STACK_PSRAM, NULL, 4, NULL,
                                                         MUSE_AUDIO_CORE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "wake task stack in PSRAM failed; trying internal RAM (largest block %u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        where = "internal";
        created = xTaskCreatePinnedToCoreWithCaps(wake_task, "muse_wake", WAKE_STACK_INTERNAL, NULL, 4, NULL,
                                                  MUSE_AUDIO_CORE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (created != pdPASS) {
        ESP_LOGE(TAG, "no wake task (free internal %u, largest block %u): wake word off",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vStreamBufferDeleteWithCaps(s_ring);
        s_ring = NULL;
        s_wn->destroy(s_data);
        s_data = NULL;
        return false;
    }
    ESP_LOGI(TAG, "wake task started (stack in %s, free internal %u)", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "ready model=%s", name);
    ESP_LOGI(TAG, "listening for \"%s\" (%s, chunk %d samples)", s_wn->get_word_name(s_data, 1), name,
             s_wn->get_samp_chunksize(s_data));
    return true;
}

void muse_wake_feed(const int16_t *pcm, size_t frames)
{
    if (!s_ring) return;
    int64_t now = esp_timer_get_time();
    atomic_fetch_add(&s_fed, 1);
    if (s_last_feed_us) {
        unsigned gap_ms = (unsigned)((now - s_last_feed_us) / 1000);
        if (gap_ms > atomic_load(&s_max_gap_ms)) atomic_store(&s_max_gap_ms, gap_ms);
    }
    {
        double acc = 0;
        for (size_t i = 0; i < frames; i++) acc += (double)pcm[i] * pcm[i];
        float db = acc > 0 ? 10.0f * log10f((float)(acc / frames) / (32768.0f * 32768.0f)) : -100.0f;
        atomic_store(&s_last_db10, (int)(db * 10));
    }
    if (now - s_last_feed_us > IDLE_GAP_US) {
        atomic_fetch_add(&s_gaps, 1);
        /* Back from a turn (or first idle): audio from before is stale and may
         * hold Muse's own voice. Drop it and stay deaf for the cooldown. */
        atomic_store(&s_hold_until, now + COOLDOWN_US);
        atomic_store(&s_hit, false);
        atomic_fetch_add(&s_gen, 1);
    }
    s_last_feed_us = now;
    if (now < atomic_load(&s_hold_until)) return;   /* cooling down: not even fed */
    if (xStreamBufferSend(s_ring, pcm, frames * sizeof(int16_t), 0) == frames * sizeof(int16_t)) {
        atomic_fetch_add(&s_sent, 1);
    } else {
        atomic_fetch_add(&s_dropped, 1);   /* detector too slow: the stream now has a hole */
    }
}

bool muse_wake_take(void)
{
    return atomic_exchange(&s_hit, false);
}

#endif
