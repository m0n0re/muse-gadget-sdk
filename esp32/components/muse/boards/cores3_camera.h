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

#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CoreS3's GC0308 (320x240, DVP) as a camera_driver_t. Nothing is held between
 * pictures: each capture powers the camera, starts esp_video, takes one frame
 * (after a few for exposure to settle), encodes it to JPEG in PSRAM, and shuts
 * everything down again, so the camera costs no internal RAM while idle.
 */
const camera_driver_t *cores3_camera_driver(void);

/*
 * Diagnostic (camera.raw): one frame exactly as the sensor delivered it, no
 * U/V swap and no JPEG, copied into a PSRAM buffer the caller free()s.
 * *fourcc is the pixelformat esp_video reported.
 */
esp_err_t cores3_camera_capture_raw(uint8_t **data, size_t *len, int *width, int *height, uint32_t *fourcc);

/* Diagnostics: camera.raw (as is), camera.testbar (GC0308 test pattern, reg 0x2e bit 0, on for the frame) and
 * camera.raw565 (sensor switched to RGB565 through VIDIOC_S_SENSOR_FMT for the frame). All restore on the way out. */
typedef enum { CORES3_RAW_NORMAL = 0, CORES3_RAW_TESTBAR, CORES3_RAW_RGB565 } cores3_raw_mode_t;
esp_err_t cores3_camera_capture_raw_mode(cores3_raw_mode_t mode, uint8_t **data, size_t *len, int *width,
                                         int *height, uint32_t *fourcc);

#ifdef __cplusplus
}
#endif
