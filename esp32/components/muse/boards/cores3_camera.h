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

#ifdef __cplusplus
}
#endif
