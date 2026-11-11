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

/*
 * CoreS3 camera: GC0308 on the DVP port, through esp_video (V4L2). Pins are the
 * BSP's, except XCLK, which the BSP leaves unassigned: M5Stack's documentation
 * has the sensor clocked from GPIO2 at 20 MHz. SCCB shares the BSP's I2C bus
 * (SCL GPIO11, SDA GPIO12). The sensor's power switch is on the AW9523 expander,
 * which bsp_feature_enable() drives.
 *
 * Everything is brought up for one picture and torn down after it (see the
 * header), so internal RAM stays free for the wake word, TTS and the servos.
 * Frame buffers are PSRAM (esp_video's), and so are the JPEG's.
 */
#include "cores3_camera.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#define CAM_W 320
#define CAM_H 240
#define CAM_XCLK_GPIO GPIO_NUM_2
#define CAM_BUFS 2
#define CAM_FRAME_BYTES (CAM_W * CAM_H * 2)   /* YUYV: 2 bytes per pixel */
#define CAM_SETTLE_FRAMES 8      /* frames dropped first, for auto exposure and white balance */
#define CAM_FRAME_TIMEOUT_S 2
#define JPEG_QUALITY 80
#define JPEG_OUT_MAX (96 * 1024) /* 320x240 at quality 80 is about 10-20 KB */

static const char *TAG = "cores3_cam";

typedef struct {
    size_t internal_free, psram_free;
} heap_mark_t;

static heap_mark_t heap_mark(void)
{
    heap_mark_t m = { heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM) };
    return m;
}

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: internal free %u, largest %u; psram free %u, largest %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/* After a capture everything taken for it must be back: a drop is a leak. */
static void log_heap_delta(const heap_mark_t *before)
{
    heap_mark_t now = heap_mark();
    long di = (long)now.internal_free - (long)before->internal_free;
    long dp = (long)now.psram_free - (long)before->psram_free;
    if (di < -256 || dp < -256) {
        ESP_LOGW(TAG, "LEAK? free after vs before the capture: internal %+ld, psram %+ld bytes", di, dp);
    } else {
        ESP_LOGI(TAG, "free after vs before the capture: internal %+ld, psram %+ld bytes (restored)", di, dp);
    }
}

static esp_err_t camera_up(void)
{
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        return err;
    }
    err = bsp_feature_enable(BSP_FEATURE_CAMERA, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera power on failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    const esp_video_init_dvp_config_t dvp = {
        .sccb_config = {
            .init_sccb = false,
            .i2c_handle = bsp_i2c_get_handle(),
            .freq = 100000,
        },
        .reset_pin = BSP_CAMERA_RST,
        .pwdn_pin = -1,
        .dvp_pin = {
            .data_width = 8,
            .data_io = { BSP_CAMERA_D0, BSP_CAMERA_D1, BSP_CAMERA_D2, BSP_CAMERA_D3,
                         BSP_CAMERA_D4, BSP_CAMERA_D5, BSP_CAMERA_D6, BSP_CAMERA_D7 },
            .vsync_io = BSP_CAMERA_VSYNC,
            .de_io = BSP_CAMERA_HSYNC,
            .pclk_io = BSP_CAMERA_PCLK,
            .xclk_io = CAM_XCLK_GPIO,
        },
        .xclk_freq = BSP_CAMERA_XCLK_CLOCK_MHZ * 1000000,
    };
    const esp_video_init_config_t cfg = { .dvp = &dvp };
    err = esp_video_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(err));
        bsp_feature_enable(BSP_FEATURE_CAMERA, false);
    }
    return err;
}

static void camera_down(void)
{
    esp_video_deinit();
    bsp_feature_enable(BSP_FEATURE_CAMERA, false);
}

/* One YUYV frame of the stream, left mapped for the caller to encode from. */
typedef struct {
    int fd;
    void *map[CAM_BUFS];
    size_t map_len[CAM_BUFS];
    bool streaming;
} stream_t;

static void stream_close(stream_t *s)
{
    if (s->streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s->fd, VIDIOC_STREAMOFF, &type);
    }
    for (int i = 0; i < CAM_BUFS; i++) {
        if (s->map[i]) {
            munmap(s->map[i], s->map_len[i]);
        }
    }
    if (s->fd >= 0) {
        close(s->fd);
    }
}

static esp_err_t stream_open(stream_t *s)
{
    memset(s, 0, sizeof(*s));
    s->fd = open(BSP_CAMERA_DEVICE, O_RDONLY);
    if (s->fd < 0) {
        ESP_LOGE(TAG, "open %s failed", BSP_CAMERA_DEVICE);
        return ESP_FAIL;
    }
    const struct timeval tmo = { .tv_sec = CAM_FRAME_TIMEOUT_S, .tv_usec = 0 };
    ioctl(s->fd, VIDIOC_S_DQBUF_TIMEOUT, &tmo);   /* else a dead sensor blocks forever */

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = CAM_W,
        .fmt.pix.height = CAM_H,
        .fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV,
    };
    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "S_FMT %dx%d YUYV failed", CAM_W, CAM_H);
        return ESP_FAIL;
    }
    if (ioctl(s->fd, VIDIOC_G_FMT, &fmt) == 0) {
        /* esp_video's G_FMT leaves sizeimage at 0 (the buffer size lives in the stream's
         * buf info); the real frame size is the buffer length QUERYBUF reports below. */
        const uint32_t pf = fmt.fmt.pix.pixelformat;
        ESP_LOGI(TAG, "format %ux%u, pixelformat '%c%c%c%c' (%s), expected %u bytes/frame (sizeimage field %u)",
                 (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height, (char)(pf & 0xff),
                 (char)((pf >> 8) & 0xff), (char)((pf >> 16) & 0xff), (char)((pf >> 24) & 0xff),
                 pf == V4L2_PIX_FMT_YUYV ? "YUYV" : "NOT YUYV", (unsigned)CAM_FRAME_BYTES,
                 (unsigned)fmt.fmt.pix.sizeimage);
        if (fmt.fmt.pix.width != CAM_W || fmt.fmt.pix.height != CAM_H) {
            ESP_LOGE(TAG, "sensor is not at %dx%d", CAM_W, CAM_H);
            return ESP_FAIL;
        }
        if (pf != V4L2_PIX_FMT_YUYV) {
            ESP_LOGE(TAG, "pixel format is not YUYV; the JPEG encoder is set up for YUYV");
            return ESP_FAIL;
        }
    }

    struct v4l2_requestbuffers req = {
        .count = CAM_BUFS,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s->fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS failed");
        return ESP_FAIL;
    }
    for (int i = 0; i < CAM_BUFS; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(s->fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QUERYBUF failed");
            return ESP_FAIL;
        }
        s->map[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, buf.m.offset);
        if (!s->map[i] || s->map[i] == MAP_FAILED) {
            s->map[i] = NULL;
            ESP_LOGE(TAG, "mmap failed");
            return ESP_FAIL;
        }
        s->map_len[i] = buf.length;
        if (buf.length < CAM_FRAME_BYTES) {
            ESP_LOGE(TAG, "buffer %d is %u bytes, a frame needs %u", i, (unsigned)buf.length,
                     (unsigned)CAM_FRAME_BYTES);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "buffer %d: %u bytes at %p (%s)", i, (unsigned)buf.length, s->map[i],
                 esp_ptr_external_ram(s->map[i]) ? "PSRAM" : "internal");
        if (ioctl(s->fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "QBUF failed");
            return ESP_FAIL;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s->fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON failed");
        return ESP_FAIL;
    }
    s->streaming = true;
    return ESP_OK;
}

/*
 * The GC0308 puts Cr (V) where esp_video's "YUYV" says Cb (U): bytes come out Y0 V Y1 U (YVYU),
 * while the JPEG encoder's YCbYCr is Y0 U Y1 V. The result was magenta/green false colour with a
 * correct picture. esp_jpeg_enc has no YVYU input, so swap the two chroma bytes of every pixel
 * pair in place, in the PSRAM frame buffer (no extra memory, internal RAM untouched).
 */
static void yvyu_to_yuyv_inplace(uint8_t *frame, size_t bytes)
{
    for (size_t i = 0; i + 3 < bytes; i += 4) {
        uint8_t t = frame[i + 1];
        frame[i + 1] = frame[i + 3];
        frame[i + 3] = t;
    }
}

/* Drops CAM_SETTLE_FRAMES, then returns the next one (still dequeued) in *out. */
static esp_err_t stream_grab(stream_t *s, const uint8_t **out, size_t *len)
{
    for (int n = 0; n <= CAM_SETTLE_FRAMES; n++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        if (ioctl(s->fd, VIDIOC_DQBUF, &buf) != 0 || (buf.flags & V4L2_BUF_FLAG_ERROR)) {
            ESP_LOGE(TAG, "no frame (DQBUF failed at frame %d)", n);
            return ESP_ERR_TIMEOUT;
        }
        if (n == CAM_SETTLE_FRAMES) {
            *out = s->map[buf.index];
            *len = buf.bytesused;
            return ESP_OK;
        }
        if (ioctl(s->fd, VIDIOC_QBUF, &buf) != 0) {
            return ESP_FAIL;
        }
    }
    return ESP_FAIL;
}

static esp_err_t cam_init(void)
{
    log_heap("camera ready (nothing held until a capture)");
    return ESP_OK;
}

static heap_mark_t s_before;   /* one capture at a time (muse_look's s_active) */

static esp_err_t cam_capture(camera_frame_t *out)
{
    int64_t t0 = esp_timer_get_time();
    s_before = heap_mark();
    const heap_mark_t *before = &s_before;
    log_heap("capture start");
    esp_err_t err = camera_up();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *jpg = NULL;
    stream_t s;
    err = stream_open(&s);
    if (err == ESP_OK) {
        log_heap("camera running");
        const uint8_t *yuv = NULL;
        size_t yuv_len = 0;
        err = stream_grab(&s, &yuv, &yuv_len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "frame: %u bytes (expected %u)", (unsigned)yuv_len, (unsigned)CAM_FRAME_BYTES);
        }
        if (err == ESP_OK && yuv_len < (size_t)CAM_FRAME_BYTES) {
            ESP_LOGE(TAG, "short frame: %u bytes", (unsigned)yuv_len);
            err = ESP_ERR_INVALID_SIZE;
        }
        if (err == ESP_OK) {
            yvyu_to_yuyv_inplace((uint8_t *)yuv, CAM_FRAME_BYTES);
            ESP_LOGI(TAG, "swapped U/V (sensor YVYU -> encoder YUYV) in place in the frame buffer");
            jpg = heap_caps_malloc(JPEG_OUT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            jpeg_enc_config_t jc = DEFAULT_JPEG_ENC_CONFIG();
            jc.width = CAM_W;
            jc.height = CAM_H;
            jc.src_type = JPEG_PIXEL_FORMAT_YCbYCr;   /* Y0 Cb Y1 Cr, after the U/V swap above */
            jc.subsampling = JPEG_SUBSAMPLE_420;
            jc.quality = JPEG_QUALITY;
            jpeg_enc_handle_t enc = NULL;
            int jlen = 0;
            if (!jpg || jpeg_enc_open(&jc, &enc) != JPEG_ERR_OK) {
                ESP_LOGE(TAG, "JPEG encoder unavailable");
                err = ESP_ERR_NO_MEM;
            } else {
                if (jpeg_enc_process(enc, yuv, CAM_W * CAM_H * 2, jpg, JPEG_OUT_MAX, &jlen) != JPEG_ERR_OK ||
                    jlen <= 0) {
                    ESP_LOGE(TAG, "JPEG encode failed");
                    err = ESP_FAIL;
                } else {
                    out->jpeg = jpg;
                    out->len = (size_t)jlen;
                    out->width = CAM_W;
                    out->height = CAM_H;
                    out->priv = jpg;
                }
                jpeg_enc_close(enc);
            }
        }
    }
    stream_close(&s);
    camera_down();
    if (err != ESP_OK) {
        free(jpg);
        log_heap("capture failed, camera off");
        log_heap_delta(before);
        return err;
    }
    log_heap("capture done, camera off (JPEG still held)");
    ESP_LOGI(TAG, "captured %ux%u JPEG, %u bytes, %d ms", (unsigned)out->width, (unsigned)out->height,
             (unsigned)out->len, (int)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

static void cam_release(camera_frame_t *frame)
{
    free(frame->priv);
    log_heap("frame released");
    log_heap_delta(&s_before);
}

static const camera_driver_t s_driver = {
    .name = "GC0308 (M5Stack CoreS3)",
    .max_width = CAM_W,
    .max_height = CAM_H,
    .init = cam_init,
    .capture = cam_capture,
    .release = cam_release,
};

const camera_driver_t *cores3_camera_driver(void)
{
    return &s_driver;
}
