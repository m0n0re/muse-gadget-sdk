#include "muse_wake.h"

#if CONFIG_MUSE_WAKE_WORD_HI_MUSE

#include <stdatomic.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
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

static const esp_wn_iface_t *s_wn;
static model_iface_data_t *s_data;
static StreamBufferHandle_t s_ring;
static atomic_bool s_hit;
static atomic_uint s_gen;            /* bumped at every gap: the detector then drops its partial chunk and state */
static atomic_llong s_hold_until;    /* feed ignored (and the detector's output discarded) until then */
static int64_t s_last_feed_us;       /* voice task only */

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
    for (;;) {
        /* Blocks until the voice task feeds idle audio: no polling. */
        size_t got = xStreamBufferReceive(s_ring, (uint8_t *)buf + have * 2, (chunk - have) * 2, portMAX_DELAY);
        if (gen != atomic_load(&s_gen)) {
            gen = atomic_load(&s_gen);
            have = 0;
            s_wn->clean(s_data);
            continue;
        }
        have += got / 2;
        if (have < (size_t)chunk) continue;
        have = 0;
        if (s_wn->detect(s_data, buf) > 0 && esp_timer_get_time() >= atomic_load(&s_hold_until)) {
            ESP_LOGI(TAG, "wake word detected (\"%s\")", s_wn->get_word_name(s_data, 1));
            atomic_store(&s_hit, true);
            /* Nothing more until the voice task has started the turn. */
            atomic_store(&s_hold_until, esp_timer_get_time() + COOLDOWN_US);
        }
    }
}

bool muse_wake_start(void)
{
    srmodel_list_t *models = esp_srmodel_init(MODEL_PARTITION);
    char *name = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, MODEL_KEY) : NULL;
    if (!name) {
        ESP_LOGE(TAG, "wn10_himuse not found in the \"%s\" partition: wake word off", MODEL_PARTITION);
        return false;
    }
    s_wn = esp_wn_handle_from_name(name);
    s_data = s_wn ? s_wn->create(name, DET_MODE_90) : NULL;
    if (!s_data) {
        ESP_LOGE(TAG, "could not create %s: wake word off", name);
        return false;
    }
    if (s_wn->get_samp_rate(s_data) != MUSE_AUDIO_RATE || s_wn->get_channel_num(s_data) != 1) {
        ESP_LOGE(TAG, "%s wants %d Hz x %d ch: wake word off", name, s_wn->get_samp_rate(s_data),
                 s_wn->get_channel_num(s_data));
        s_wn->destroy(s_data);
        return false;
    }
    s_ring = xStreamBufferCreateWithCaps(RING_BYTES, 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_ring) return false;
    atomic_store(&s_hold_until, esp_timer_get_time() + COOLDOWN_US);
    if (xTaskCreatePinnedToCoreWithCaps(wake_task, "muse_wake", 8192, NULL, 4, NULL, MUSE_AUDIO_CORE,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "no wake task");
        return false;
    }
    ESP_LOGI(TAG, "listening for \"%s\" (%s, chunk %d samples)", s_wn->get_word_name(s_data, 1), name,
             s_wn->get_samp_chunksize(s_data));
    return true;
}

void muse_wake_feed(const int16_t *pcm, size_t frames)
{
    if (!s_ring) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_feed_us > IDLE_GAP_US) {
        /* Back from a turn (or first idle): audio from before is stale and may
         * hold Muse's own voice. Drop it and stay deaf for the cooldown. */
        atomic_store(&s_hold_until, now + COOLDOWN_US);
        atomic_store(&s_hit, false);
        atomic_fetch_add(&s_gen, 1);
    }
    s_last_feed_us = now;
    if (now < atomic_load(&s_hold_until)) return;   /* cooling down: not even fed */
    xStreamBufferSend(s_ring, pcm, frames * sizeof(int16_t), 0);
}

bool muse_wake_take(void)
{
    return atomic_exchange(&s_hit, false);
}

#endif
