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

#include "muse_look.h"

#if CONFIG_MUSE_BOARD_M5STACK_CORES3

#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "boards/cores3_camera.h"
#include "camera.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "muse_state.h"
#include "muse_wifi.h"
#if CONFIG_MUSE_HATCH
#include "muse_chat.h"
#endif

#ifndef CONFIG_MUSE_CAMERA_ENDPOINT
#define CONFIG_MUSE_CAMERA_ENDPOINT ""
#endif

#define LOOK_HTTP_TIMEOUT_MS 8000
#define LOOK_REPLY_MAX 255
#define LOOK_FILE_MAX 127

static const char *TAG = "look";

static atomic_flag s_active = ATOMIC_FLAG_INIT;
static muse_look_cb s_cb;
static void *s_ctx;

void muse_look_init(void)
{
    camera_register(cores3_camera_driver());
    const camera_driver_t *d = camera_get();
    ESP_LOGI(TAG, "camera: %s, endpoint %s", d ? d->name : "none",
             CONFIG_MUSE_CAMERA_ENDPOINT[0] ? CONFIG_MUSE_CAMERA_ENDPOINT : "(not configured)");
}

/* The endpoint's reply is JSON with the stored name ({"filename": "..."}, or
 * "file" / "name"), or just the name as plain text. Copies it to `file`. */
static bool parse_name(const char *body, char *file, size_t cap)
{
    while (isspace((unsigned char)*body)) {
        body++;
    }
    const char *name = NULL;
    cJSON *root = NULL;
    if (*body == '{') {
        root = cJSON_Parse(body);
        static const char *keys[] = { "filename", "file", "name" };
        for (size_t i = 0; root && !name && i < sizeof(keys) / sizeof(keys[0]); i++) {
            cJSON *it = cJSON_GetObjectItem(root, keys[i]);
            if (cJSON_IsString(it) && it->valuestring) {
                name = it->valuestring;
            }
        }
    } else {
        name = body;
    }
    bool ok = false;
    if (name) {
        size_t n = 0;
        while (*name && n + 1 < cap && *name != '\n' && *name != '\r' && *name != '"') {
            file[n++] = *name++;
        }
        while (n && isspace((unsigned char)file[n - 1])) {
            n--;
        }
        file[n] = 0;
        ok = n > 0;
    }
    cJSON_Delete(root);
    return ok;
}

/* POSTs the JPEG; the reply names it. Returns false with code and message set. */
static bool upload(const camera_frame_t *f, char *file, const char **code, const char **message)
{
    esp_http_client_config_t cfg = {};
    cfg.url = CONFIG_MUSE_CAMERA_ENDPOINT;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = LOOK_HTTP_TIMEOUT_MS;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        *code = "upload_failed";
        *message = "http client init failed";
        return false;
    }
    char reply[LOOK_REPLY_MAX + 1];
    int total = 0;
    bool ok = false;
    *code = "upload_failed";
    *message = "could not reach the camera endpoint";
    esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    if (esp_http_client_open(c, (int)f->len) == ESP_OK &&
        esp_http_client_write(c, (const char *)f->jpeg, (int)f->len) == (int)f->len &&
        esp_http_client_fetch_headers(c) >= 0) {
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            *message = "the camera endpoint did not answer 200";
            ESP_LOGW(TAG, "endpoint answered %d", status);
        } else {
            int n;
            while (total < LOOK_REPLY_MAX && (n = esp_http_client_read(c, reply + total, LOOK_REPLY_MAX - total)) > 0) {
                total += n;
            }
            reply[total] = 0;
            if (parse_name(reply, file, LOOK_FILE_MAX + 1)) {
                ok = true;
            } else {
                *code = "bad_response";
                *message = "the camera endpoint's answer has no file name";
                ESP_LOGW(TAG, "unusable answer: %.80s", reply);
            }
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

static void look_task(void *arg)
{
    (void)arg;
    const char *code = NULL, *message = NULL;
    char file[LOOK_FILE_MAX + 1] = "";
    size_t bytes = 0;
    bool ok = false;

    camera_frame_t frame;
    esp_err_t err = camera_capture(&frame);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "capture failed: %s", esp_err_to_name(err));
        code = "capture_failed";
        message = "the camera could not take a picture";
    } else {
        bytes = frame.len;
        ok = upload(&frame, file, &code, &message);
        camera_release(&frame);
        if (ok) {
            ESP_LOGI(TAG, "delivered %u bytes as %s", (unsigned)bytes, file);
        }
    }
    /* A press that began a turn meanwhile keeps its own mode. */
    if (muse_state_mode(NULL) == MUSE_MODE_THINKING) {
        muse_state_set_mode(MUSE_MODE_IDLE);
    }
    muse_look_cb cb = s_cb;
    void *ctx = s_ctx;
    atomic_flag_clear(&s_active);
    cb(ctx, ok, code, message, file, bytes);
    vTaskDeleteWithCaps(NULL);
}

bool muse_look_start(muse_look_cb cb, void *ctx, const char **code, const char **message)
{
    if (!CONFIG_MUSE_CAMERA_ENDPOINT[0]) {
        *code = "camera_not_configured";
        *message = "camera endpoint not configured";
        return false;
    }
    if (!camera_get() || !muse_wifi_connected()) {
        *code = "unavailable";
        *message = camera_get() ? "not connected to the network" : "no camera";
        return false;
    }
    if (atomic_flag_test_and_set(&s_active)) {
        *code = "busy";
        *message = "another picture is being taken";
        return false;
    }
    /* Any interaction wakes a sleeping avatar; busy is anything but idle after. */
    muse_state_set_asleep(false);
    bool busy = muse_state_mode(NULL) != MUSE_MODE_IDLE;
#if CONFIG_MUSE_HATCH
    busy = busy || muse_hatch_busy();
#endif
    if (busy) {
        atomic_flag_clear(&s_active);
        *code = "busy";
        *message = "a conversation, call or speech is active";
        return false;
    }
    s_cb = cb;
    s_ctx = ctx;
    muse_state_set_mode(MUSE_MODE_THINKING);
    if (xTaskCreateWithCaps(look_task, "look", 10240, NULL, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        muse_state_set_mode(MUSE_MODE_IDLE);
        atomic_flag_clear(&s_active);
        *code = "unavailable";
        *message = "out of memory for the camera task";
        return false;
    }
    return true;
}

#endif /* CONFIG_MUSE_BOARD_M5STACK_CORES3 */
