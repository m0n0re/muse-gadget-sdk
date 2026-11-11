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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_BOARD_M5STACK_CORES3
/*
 * camera.look, a local extension of this fork (CoreS3): take one picture with
 * the GC0308 and POST the JPEG to CONFIG_MUSE_CAMERA_ENDPOINT, which answers
 * with the name the picture was stored under.
 *
 * muse_look_init registers the camera and logs the heap; call it once at start.
 */
void muse_look_init(void);

/*
 * `cb` runs exactly once, on the look task, when the picture has been delivered
 * (ok, `file` is the name the endpoint gave, `bytes` the JPEG's size) or has
 * failed (`code` is "capture_failed", "upload_failed" or "bad_response"). While
 * it runs the avatar is in MUSE_MODE_THINKING (the LEDs follow), and goes back
 * to idle after. Returns false at once, with *code and *message set, if it
 * can't be taken: "camera_not_configured" (no endpoint), "unavailable" (no
 * camera, or no network), "busy" (a conversation, call, speech or another
 * picture is active). A sleeping avatar is woken.
 */
typedef void (*muse_look_cb)(void *ctx, bool ok, const char *code, const char *message, const char *file,
                             size_t bytes);
bool muse_look_start(muse_look_cb cb, void *ctx, const char **code, const char **message);

/*
 * camera.raw, a diagnostic: the same as camera.look but the unconverted sensor
 * frame is POSTed (application/octet-stream) to the endpoint's server at /raw.
 * `fourcc` is the pixelformat the driver reported, as 4 characters.
 */
typedef void (*muse_raw_cb)(void *ctx, bool ok, const char *code, const char *message, const char *file,
                            size_t bytes, int width, int height, const char *fourcc);
typedef enum { MUSE_RAW_NORMAL = 0, MUSE_RAW_TESTBAR, MUSE_RAW_RGB565 } muse_raw_mode_t;
bool muse_raw_start(muse_raw_mode_t mode, muse_raw_cb cb, void *ctx, const char **code, const char **message);
#else
static inline void muse_look_init(void) {}
#endif

#ifdef __cplusplus
}
#endif
