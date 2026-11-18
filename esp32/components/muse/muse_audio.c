/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_MUSE_WAKE_WORD_HI_MUSE
#include "esp_ns.h"   /* esp-sr stand-alone noise suppression (no AFE pipeline) */
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_mem.h"
#include "muse_settings.h"

static const char *TAG = "muse_audio";

#define CHANNELS 2
#define HPF_HZ 80.0f

static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
static bool s_open;
/* Opening, closing and the volume and gain setters, which other tasks call:
 * with a board-switched codec supply, none may reach a codec mid-switch. */
static SemaphoreHandle_t s_lock;
static int16_t s_in_stereo[MUSE_AUDIO_CHUNK * CHANNELS];
static int16_t s_in_mono[MUSE_AUDIO_CHUNK];
static int16_t s_out_stereo[MUSE_AUDIO_CHUNK * CHANNELS];

/* One-pole high-pass on the mixed mic signal. */
static float s_hpf_a;
static float s_mic_scale = 1.0f;
/* Digital gain above the codec PGA ceiling, applied to every captured sample
 * in muse_audio_read (the one path wake word, notes and calls all read). */
static float s_dig_scale = 1.0f;
static float s_hpf_x1, s_hpf_y1;
/* The last read chunk before the digital gain (codec PGA only), kept so the
 * voice-note path can normalise from it instead of the saturated signal. */
static int16_t s_pre_dig[MUSE_AUDIO_CHUNK];
/* Digital gain above 100 % volume, applied to every sample written to the
 * speaker (muse_audio_write and the loopback tone). */
static float s_out_scale = 1.0f;

static inline int16_t out_sample(int16_t x)
{
    if (s_out_scale == 1.0f) return x;
    float y = (float)x * s_out_scale;
    return (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
}

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void codec_power(bool on)
{
    if (muse_board->codec_power) {
        muse_board->codec_power(on);
    }
}

/* A closed codec on a board-switched supply has no power. */
static bool codecs_reachable(void)
{
    return s_open || !muse_board->codec_power;
}

static void apply_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > MUSE_VOLUME_MAX) volume = MUSE_VOLUME_MAX;
    int codec = volume > 100 ? 100 : volume;
    float dig_db = volume > 100 ? (float)(volume - 100) * MUSE_VOLUME_DIG_DB_PER_PCT : 0.0f;
    s_out_scale = powf(10.0f, dig_db / 20.0f);
    ESP_LOGI(TAG, "volume %d%% (codec %d + digital %.1f dB)", volume, codec, (double)dig_db);
    volume = codec;
    if (s_spk && codecs_reachable()) {
        esp_codec_dev_set_out_vol(s_spk, volume);
    }
}

static void apply_mic_gain(int db)
{
    if (db < 0) db = 0;
    if (db > MUSE_MIC_GAIN_MAX) db = MUSE_MIC_GAIN_MAX;
    if (muse_board->mic_init || (!s_mic && muse_board->mic_read)) {
        s_mic_scale = powf(10.0f, (float)db / 20.0f);   /* no codec gain: scale samples */
        s_dig_scale = 1.0f;
        ESP_LOGI(TAG, "mic gain %d dB (digital %d)", db, db);
        return;
    }
    /* PGA up to its ceiling; the rest is digital, applied in muse_audio_read. */
    int pga = db > MUSE_MIC_GAIN_PGA_MAX ? MUSE_MIC_GAIN_PGA_MAX : db;
    int dig = db - pga;
    s_dig_scale = powf(10.0f, (float)dig / 20.0f);
    ESP_LOGI(TAG, "mic gain %d dB (pga %d + digital %d)", db, pga, dig);
    db = pga;
    if (!s_mic || !codecs_reachable()) {
        return;
    }
    if (muse_board->set_mic_gain) {
        muse_board->set_mic_gain(s_mic, db);
    } else {
        esp_codec_dev_set_in_gain(s_mic, (float)db);
    }
}

static esp_err_t open_codecs(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = MUSE_AUDIO_RATE,
        .channel = CHANNELS,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open speaker");
    if (s_mic) {
        if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
            esp_codec_dev_close(s_spk);
            ESP_LOGE(TAG, "open mic");
            return ESP_FAIL;
        }
    }
    s_open = true;
    return ESP_OK;
}

static esp_err_t read_input(size_t frames)
{
    if (s_mic) {
        return esp_codec_dev_read(s_mic, s_in_stereo, frames * CHANNELS * sizeof(int16_t)) == ESP_CODEC_DEV_OK
            ? ESP_OK : ESP_FAIL;
    }
    /* A mic without a codec beside a speaker codec, for the self-test and
     * loopback test (muse_audio_read reads it directly): mono, its DC
     * dropped before the gain so an offset doesn't saturate the samples. */
    if (!muse_board->mic_read || muse_board->mic_read(s_in_mono, frames) != ESP_OK) {
        return ESP_FAIL;
    }
    float dc = 0;
    for (size_t i = 0; i < frames; i++) {
        dc += s_in_mono[i];
    }
    dc /= (float)frames;
    for (size_t i = 0; i < frames; i++) {
        float x = ((float)s_in_mono[i] - dc) * s_mic_scale;
        int16_t v = (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
        s_in_stereo[2 * i] = v;
        s_in_stereo[2 * i + 1] = v;
    }
    return ESP_OK;
}

esp_err_t muse_audio_init(int volume, int mic_gain_db)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "audio lock");
    esp_err_t err;
    if (muse_board->mic_init) {
        err = muse_board->mic_init();
        if (err == ESP_OK) err = muse_board->mic_power(true);
        if (err != ESP_OK) return err;
        s_open = true;
    } else {
        /* A board-switched supply goes off again on any failure: nothing will
         * open the codecs, and the setters skip them while closed. */
        codec_power(true);
        err = muse_board->audio_init(&s_spk, &s_mic);
        if (err == ESP_OK && !(s_spk && (s_mic || muse_board->mic_read))) {
            err = ESP_FAIL;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "codec init failed");
        } else {
            err = open_codecs();   /* logs its own failure */
        }
        if (err != ESP_OK) {
            codec_power(false);
            return err;
        }
    }
    muse_audio_set_volume(volume);
    muse_audio_set_mic_gain(mic_gain_db);

    float rc = 1.0f / (2.0f * (float)M_PI * HPF_HZ);
    float dt = 1.0f / MUSE_AUDIO_RATE;
    s_hpf_a = rc / (rc + dt);

    ESP_LOGI(TAG, "audio ready: %d Hz, %d ch, vol %d, mic gain %d dB", MUSE_AUDIO_RATE, CHANNELS, volume, mic_gain_db);
    return ESP_OK;
}

void muse_audio_power(bool on)
{
    lock();
    if (muse_board->mic_init) {
        if (on != s_open && muse_board->mic_power(on) == ESP_OK) s_open = on;
        unlock();
        return;
    }
    if (!s_spk || on == s_open) {
        unlock();
        return;
    }
    /* The shared I2S bus gets disabled twice (once per codec), which logs a
     * harmless error. */
    esp_log_level_t lvl = esp_log_level_get("i2s_common");
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    /* The codecs' supply comes up before they open and goes once both have
     * closed. The amp goes on once they're open and set, and off before they
     * close, so it never passes on their power-up or power-down. */
    if (on) {
        codec_power(true);
        if (open_codecs() == ESP_OK) {
            apply_volume(muse_settings_volume());
            apply_mic_gain(muse_settings_mic_gain());
            if (muse_board->audio_power) {
                muse_board->audio_power(true);
            }
        } else {
            /* Still closed (open_codecs closes the speaker if the mic fails)
             * and the amp still off: take the supply down again too. The
             * next wake tries again. */
            codec_power(false);
        }
    } else {
        if (muse_board->audio_power) {
            muse_board->audio_power(false);
        }
        esp_codec_dev_close(s_spk);
        if (s_mic) {
            esp_codec_dev_close(s_mic);
        }
        s_open = false;
        codec_power(false);
    }
    esp_log_level_set("i2s_common", lvl);
    unlock();
}

void muse_audio_set_volume(int volume)
{
    lock();
    apply_volume(volume);
    unlock();
}

void muse_audio_set_mic_gain(int db)
{
    lock();
    apply_mic_gain(db);
    unlock();
}

static float to_db(double mean_sq)
{
    return 10.0f * log10f((float)(mean_sq / (32768.0 * 32768.0)) + 1e-10f);
}

void muse_audio_selftest(void)
{
    if (muse_board->mic_init) {
        int16_t mono[MUSE_AUDIO_CHUNK];
        if (muse_audio_read(mono, MUSE_AUDIO_CHUNK) == ESP_OK) {
            ESP_LOGI(TAG, "PDM mic capture: %.1f dBFS", muse_audio_dbfs(mono, MUSE_AUDIO_CHUNK));
        }
        return;
    }
    enum { FLUSH = MUSE_AUDIO_RATE / 4, MEASURE = MUSE_AUDIO_RATE / 2 };

    /* Capture: drop the DMA backlog, then time a known number of frames. */
    for (int n = 0; n < FLUSH; n += MUSE_AUDIO_CHUNK) {
        read_input(MUSE_AUDIO_CHUNK);
    }
    double sl = 0, sr = 0, slr = 0, ml = 0, mr = 0;
    int pl = 0, pr = 0;
    int64_t t0 = esp_timer_get_time();
    for (int n = 0; n < MEASURE; n += MUSE_AUDIO_CHUNK) {
        read_input(MUSE_AUDIO_CHUNK);
        for (int i = 0; i < MUSE_AUDIO_CHUNK; i++) {
            int l = s_in_stereo[2 * i], r = s_in_stereo[2 * i + 1];
            ml += l;
            mr += r;
            sl += (double)l * l;
            sr += (double)r * r;
            slr += (double)l * r;
            pl = abs(l) > pl ? abs(l) : pl;
            pr = abs(r) > pr ? abs(r) : pr;
        }
    }
    float cap_hz = MEASURE * 1e6f / (float)(esp_timer_get_time() - t0);
    ml /= MEASURE;
    mr /= MEASURE;
    double vl = sl / MEASURE - ml * ml, vr = sr / MEASURE - mr * mr;
    double corr = (slr / MEASURE - ml * mr) / (sqrt(vl * vr) + 1e-9);

    /* Playback: fill the DMA queue with silence, then time more silence. */
    memset(s_out_stereo, 0, sizeof(s_out_stereo));
    for (int n = 0; n < FLUSH; n += MUSE_AUDIO_CHUNK) {
        esp_codec_dev_write(s_spk, s_out_stereo, sizeof(s_out_stereo));
    }
    t0 = esp_timer_get_time();
    for (int n = 0; n < MEASURE; n += MUSE_AUDIO_CHUNK) {
        esp_codec_dev_write(s_spk, s_out_stereo, sizeof(s_out_stereo));
    }
    float play_hz = MEASURE * 1e6f / (float)(esp_timer_get_time() - t0);

    ESP_LOGI(TAG, "self-test @ %d dB: mic L %.1f dBFS (peak %d, dc %d), R %.1f dBFS (peak %d, dc %d), corr %.2f",
             muse_settings_mic_gain(), to_db(vl), pl, (int)ml, to_db(vr), pr, (int)mr, corr);
    ESP_LOGI(TAG, "self-test: capture %.0f Hz, playback %.0f Hz (expected %d)", cap_hz, play_hz, MUSE_AUDIO_RATE);
}

/* Goertzel power of `f` in an interleaved stereo buffer, as dBFS of a sine. */
static float tone_db(const int16_t *st, int ch, size_t frames, float f)
{
    float k = 2.0f * cosf(2.0f * (float)M_PI * f / MUSE_AUDIO_RATE);
    float s1 = 0, s2 = 0;
    for (size_t i = 0; i < frames; i++) {
        float s0 = st[2 * i + ch] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float p = s1 * s1 + s2 * s2 - k * s1 * s2;
    float amp = 2.0f * sqrtf(p > 0 ? p : 0) / frames;   /* sine amplitude */
    return 20.0f * log10f(amp / 32768.0f + 1e-7f);
}

/*
 * Full-duplex loopback: plays tones on the speaker while recording both mics,
 * at each PGA step. Writes and reads alternate chunk by chunk; the DMA queues
 * absorb the phase offset.
 */
void muse_audio_loopback_test(int volume)
{
    if (muse_board->mic_init) {
        ESP_LOGW(TAG, "no speaker for loopback test");
        return;
    }
    enum { TONE = MUSE_AUDIO_RATE * 3 / 10, SKIP = MUSE_AUDIO_RATE / 10, MEAS = TONE - SKIP };
    static const float freqs[] = { 200, 400, 700, 1000, 2000, 3000, 4000, 6000 };
    static const int gains[] = { 0, 6, 12, 18, 24, 30, 33, 36 };
    const float amp = 0.25f * 32767.0f;   /* -12 dBFS tone */
    int16_t *cap = heap_caps_malloc(MEAS * CHANNELS * sizeof(int16_t), MUSE_BIG_CAPS);
    if (!cap) {
        return;
    }
    muse_audio_set_volume(volume);
    ESP_LOGI(TAG, "loopback: speaker vol %d, tone -12 dBFS; values = mic L/R tone level dBFS", volume);

    for (size_t g = 0; g < sizeof(gains) / sizeof(gains[0]); g++) {
        muse_audio_set_mic_gain(gains[g]);
        char line[256];
        int len = snprintf(line, sizeof(line), "gain %2d:", gains[g]);
        int peak = 0;
        /* Silence first for the noise floor, then each tone. */
        for (int t = -1; t < (int)(sizeof(freqs) / sizeof(freqs[0])); t++) {
            float f = t < 0 ? 0 : freqs[t];
            float phase = 0;
            size_t got = 0;
            for (int n = 0; n < TONE + SKIP; n += MUSE_AUDIO_CHUNK) {
                for (int i = 0; i < MUSE_AUDIO_CHUNK; i++) {
                    int16_t v = f ? (int16_t)(amp * sinf(phase)) : 0;
                    phase += 2.0f * (float)M_PI * f / MUSE_AUDIO_RATE;
                    s_out_stereo[2 * i] = s_out_stereo[2 * i + 1] = out_sample(v);
                }
                esp_codec_dev_write(s_spk, s_out_stereo, sizeof(s_out_stereo));
                read_input(MUSE_AUDIO_CHUNK);
                /* Skip the first SKIP*2 frames: DMA latency plus the previous tone's tail. */
                if (n >= 2 * SKIP && got + MUSE_AUDIO_CHUNK <= MEAS) {
                    memcpy(cap + got * CHANNELS, s_in_stereo, sizeof(s_in_stereo));
                    got += MUSE_AUDIO_CHUNK;
                }
            }
            for (size_t i = 0; i < got * CHANNELS; i++) {
                peak = abs(cap[i]) > peak ? abs(cap[i]) : peak;
            }
            if (t < 0) {
                double sl = 0, sr = 0;
                for (size_t i = 0; i < got; i++) {
                    sl += (double)cap[2 * i] * cap[2 * i];
                    sr += (double)cap[2 * i + 1] * cap[2 * i + 1];
                }
                len += snprintf(line + len, sizeof(line) - len, " floor %.0f/%.0f |", to_db(sl / got), to_db(sr / got));
                peak = 0;
            } else {
                len += snprintf(line + len, sizeof(line) - len, " %.0f:%.0f/%.0f", f,
                                tone_db(cap, 0, got, f), tone_db(cap, 1, got, f));
            }
        }
        ESP_LOGI(TAG, "%s | peak %.1f dBFS", line, 20.0 * log10((peak + 1) / 32768.0));
    }
    free(cap);
    muse_audio_set_mic_gain(muse_settings_mic_gain());
    muse_audio_set_volume(muse_settings_volume());
}

esp_err_t muse_audio_read(int16_t *mono, size_t frames)
{
    while (frames) {
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        /* A mic without a codec: its gain applies in float, ahead of the
         * high-pass, so a DC offset can't saturate the samples first. */
        bool direct = muse_board->mic_init || (!s_mic && muse_board->mic_read);
        if (direct) {
            if (muse_board->mic_read(mono, n) != ESP_OK) return ESP_FAIL;
        } else if (read_input(n) != ESP_OK) {
            return ESP_FAIL;
        }
        for (size_t i = 0; i < n; i++) {
            float x = direct ? (float)mono[i] * s_mic_scale
                     : muse_board->mic_slot < 0 ? 0.5f * ((float)s_in_stereo[2 * i] + (float)s_in_stereo[2 * i + 1])
                                                : (float)s_in_stereo[2 * i + muse_board->mic_slot];
            float y = s_hpf_a * (s_hpf_y1 + x - s_hpf_x1);
            s_hpf_x1 = x;
            s_hpf_y1 = y;
            if (i < MUSE_AUDIO_CHUNK) {
                s_pre_dig[i] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
            }
            if (!direct) y *= s_dig_scale;   /* direct mics already scaled in full */
            mono[i] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
        }
        mono += n;
        frames -= n;
    }
    return ESP_OK;
}

/* Voice-note AGC + soft limiter, in place, one chunk at a time.
 *
 * Why: the note path used to send muse_audio_read's output as is. At 45 dB the
 * digital stage (x2.8) saturates int16, and level swung per recording (peaks
 * 0.06..1.0 measured), so transcription failed at both ends.
 * Input is the chunk before the digital gain (PGA only), so nothing was clipped.
 *
 * Target: speech peaks about 0.6 FS (inside the 0.5..0.8 aim). A peak envelope
 * (instant attack, ~1.5 s release) sets the gain; gain drops at once and rises
 * at most 3 % per 20 ms chunk (~4x/s), so words don't pump.
 * Range: floor 1x (never below PGA-only), ceiling = user's digital stage x8
 * (+18 dB) so the Sound-page gain still sets the base sensitivity.
 * Silence guard: envelope under -44 dBFS holds the gain (no noise lift).
 * Limiter: above 0.7 FS a tanh knee eases toward 0.95 FS; no hard clip. */
#define AGC_TARGET   (0.60f * 32768.0f)
#define AGC_FLOOR    150.0f      /* ~ -47 dBFS peak: below this it is room noise */
#define AGC_RELEASE  0.987f      /* per 20 ms: envelope falls ~1.5 s to 1/e */
#define AGC_RISE     1.03f
#define AGC_HEADROOM 8.0f
#define LIM_KNEE     (0.70f * 32768.0f)
#define LIM_CEIL     (0.95f * 32768.0f)

/* Stationary-noise suppression (esp-sr WebRTC NS, 10 ms frames) ahead of the AGC, voice notes only.
 * Lazily created on first use (one small instance, no large resident buffers); if it can't be
 * created the note path just runs without it. The wake-word feed never goes through here. */
static int16_t s_ns_out[MUSE_AUDIO_CHUNK];
static const int16_t *note_denoise(size_t frames)
{
#if CONFIG_MUSE_WAKE_WORD_HI_MUSE
    static ns_handle_t ns;
    static bool tried;
    if (!ns && !tried) {
        tried = true;
        size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        ns = ns_create(10);
        ESP_LOGI(TAG, "note noise suppression %s (internal RAM -%d B)", ns ? "on" : "unavailable",
                 (int)(before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    }
    if (ns && frames == MUSE_AUDIO_CHUNK) {
        const size_t f = MUSE_AUDIO_RATE / 100;   /* 10 ms */
        for (size_t o = 0; o + f <= frames; o += f) {
            ns_process(ns, s_pre_dig + o, s_ns_out + o);
        }
        return s_ns_out;
    }
#endif
    (void)frames;
    return s_pre_dig;
}

void muse_audio_note_prepare(int16_t *mono, size_t frames)
{
    static float env, gain;
    if (frames > MUSE_AUDIO_CHUNK) frames = MUSE_AUDIO_CHUNK;
    const int16_t *src = note_denoise(frames);
    float base = s_dig_scale < 1.0f ? 1.0f : s_dig_scale;
    if (gain == 0.0f) gain = base;
    float peak = 0;
    for (size_t i = 0; i < frames; i++) {
        float a = fabsf((float)src[i]);
        if (a > peak) peak = a;
    }
    env = peak > env ? peak : env * AGC_RELEASE;
    float want = gain;
    if (env >= AGC_FLOOR) {
        want = AGC_TARGET / env;
        if (want < 1.0f) want = 1.0f;
        if (want > base * AGC_HEADROOM) want = base * AGC_HEADROOM;
    }
    float from = gain;
    if (want < gain) gain = from = want;           /* loud: drop immediately */
    else gain = want > gain * AGC_RISE ? gain * AGC_RISE : want;
    for (size_t i = 0; i < frames; i++) {
        float g = from + (gain - from) * ((float)(i + 1) / (float)frames);
        float y = (float)src[i] * g;
        float a = fabsf(y);
        if (a > LIM_KNEE) {
            a = LIM_KNEE + (LIM_CEIL - LIM_KNEE) * tanhf((a - LIM_KNEE) / (LIM_CEIL - LIM_KNEE));
            y = y < 0 ? -a : a;
        }
        mono[i] = (int16_t)y;
    }
}

esp_err_t muse_audio_write(const int16_t *mono, size_t frames)
{
    if (muse_board->mic_init) {
        (void)mono;
        vTaskDelay(pdMS_TO_TICKS((frames * 1000 + MUSE_AUDIO_RATE - 1) / MUSE_AUDIO_RATE));
        return ESP_OK;
    }
    /* Speaker off still plays silence at the same pace, so captions timed to
     * the audio scroll as if it were heard. */
    bool mute = !muse_settings_speaker_on();
    while (frames) {
        size_t n = frames > MUSE_AUDIO_CHUNK ? MUSE_AUDIO_CHUNK : frames;
        for (size_t i = 0; i < n; i++) {
            int16_t o = mute ? 0 : out_sample(mono[i]);
            s_out_stereo[2 * i] = o;
            s_out_stereo[2 * i + 1] = o;
        }
        if (esp_codec_dev_write(s_spk, s_out_stereo, n * CHANNELS * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            return ESP_FAIL;
        }
        mono += n;
        frames -= n;
    }
    return ESP_OK;
}

float muse_audio_dbfs(const int16_t *mono, size_t frames)
{
    if (!frames) {
        return -100.0f;
    }
    double acc = 0;
    for (size_t i = 0; i < frames; i++) {
        acc += (double)mono[i] * mono[i];
    }
    float db = to_db(acc / frames);
    return db < -100.0f ? -100.0f : db;
}

float muse_audio_level(const int16_t *mono, size_t frames)
{
    /* Map roughly -54..-12 dBFS onto 0..1. */
    float l = (muse_audio_dbfs(mono, frames) + 54.0f) / 42.0f;
    return l < 0 ? 0 : (l > 1 ? 1 : l);
}

void muse_audio_chirp(int rising)
{
    enum { MS = 90, N = MUSE_AUDIO_RATE * MS / 1000 };
    static int16_t buf[N];
    float f0 = rising ? 620.0f : 980.0f;
    float f1 = rising ? 1180.0f : 560.0f;
    float phase = 0;
    for (int i = 0; i < N; i++) {
        float x = (float)i / N;
        float f = f0 + (f1 - f0) * x;
        phase += 2.0f * (float)M_PI * f / MUSE_AUDIO_RATE;
        float env = (x < 0.1f ? x / 0.1f : 1.0f) * (1.0f - x) * (1.0f - x);
        /* A touch of the 3rd harmonic for a soft 8-bit flavour. */
        float s = sinf(phase) + 0.3f * sinf(3.0f * phase);
        buf[i] = (int16_t)(s * env * 7000.0f);
    }
    muse_audio_write(buf, N);
}
