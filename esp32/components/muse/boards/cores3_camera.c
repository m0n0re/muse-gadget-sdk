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
#include "driver/i2c_master.h"
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
#include "soc/lcd_cam_struct.h"

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
    uint32_t fourcc;   /* pixelformat G_FMT reported */
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

static void gc0308_dump_regs(const char *when, bool repair);

/*
 * The LCD_CAM's RGB<->YUV converter must be off for a raw pass-through. esp_video's DVP device leaves
 * esp_cam_ctlr_dvp_config_t.output_data_color_type at 0, which differs from the input type, so IDF's
 * esp_cam_ctlr_dvp_format_conversion() programs a YUV->RGB (or RGB565->?) conversion and sets
 * CAM_CONV_BYPASS = 1 (despite its name, 1 = converter enabled: TRM "CAM_CONV_BYPASS: 0 bypass, 1 enable").
 * esp32-camera never touches the converter (bypassed). That mangles the 8-bit stream (Y intact, U/V merged
 * into one component). The controller is created inside esp_video at STREAMON, so clear the bit afterwards;
 * stream_grab drops CAM_SETTLE_FRAMES frames first, so no mangled frame is used.
 */
static void cam_converter_bypass(const char *when)
{
    const uint32_t before = LCD_CAM.cam_rgb_yuv.cam_conv_bypass;
    LCD_CAM.cam_rgb_yuv.cam_conv_bypass = 0;
    ESP_LOGI(TAG, "LCD_CAM converter (%s): CAM_CONV_BYPASS was %u (1 = converter on), now %u; 8bits_data_inv %u, "
             "trans_mode %u, yuv_mode %u, yuv2yuv_mode %u, byte_order %u, bit_order %u",
             when, (unsigned)before, (unsigned)LCD_CAM.cam_rgb_yuv.cam_conv_bypass,
             (unsigned)LCD_CAM.cam_rgb_yuv.cam_conv_8bits_data_inv, (unsigned)LCD_CAM.cam_rgb_yuv.cam_conv_trans_mode,
             (unsigned)LCD_CAM.cam_rgb_yuv.cam_conv_yuv_mode, (unsigned)LCD_CAM.cam_rgb_yuv.cam_conv_yuv2yuv_mode,
             (unsigned)LCD_CAM.cam_ctrl.cam_byte_order, (unsigned)LCD_CAM.cam_ctrl.cam_bit_order);
}

/* pixfmt: what S_FMT asks for. repair_yuv: the register check may put 0x24/0x2e back to the YUYV table's
 * values (off for the RGB565 diagnostic, whose 0x24 is meant to differ). */
static esp_err_t stream_open(stream_t *s, bool require_yuyv, uint32_t pixfmt, bool repair_yuv)
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
        .fmt.pix.pixelformat = pixfmt,
    };
    if (ioctl(s->fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "S_FMT %dx%d '%c%c%c%c' failed", CAM_W, CAM_H, (char)(pixfmt & 0xff),
                 (char)((pixfmt >> 8) & 0xff), (char)((pixfmt >> 16) & 0xff), (char)((pixfmt >> 24) & 0xff));
        return ESP_FAIL;
    }
    if (ioctl(s->fd, VIDIOC_G_FMT, &fmt) == 0) {
        /* esp_video's G_FMT leaves sizeimage at 0 (the buffer size lives in the stream's
         * buf info); the real frame size is the buffer length QUERYBUF reports below. */
        const uint32_t pf = fmt.fmt.pix.pixelformat;
        s->fourcc = pf;
        ESP_LOGI(TAG, "format %ux%u, pixelformat '%c%c%c%c' (%s), expected %u bytes/frame (sizeimage field %u)",
                 (unsigned)fmt.fmt.pix.width, (unsigned)fmt.fmt.pix.height, (char)(pf & 0xff),
                 (char)((pf >> 8) & 0xff), (char)((pf >> 16) & 0xff), (char)((pf >> 24) & 0xff),
                 pf == V4L2_PIX_FMT_YUYV ? "YUYV" : "NOT YUYV", (unsigned)CAM_FRAME_BYTES,
                 (unsigned)fmt.fmt.pix.sizeimage);
        if (fmt.fmt.pix.width != CAM_W || fmt.fmt.pix.height != CAM_H) {
            ESP_LOGE(TAG, "sensor is not at %dx%d", CAM_W, CAM_H);
            return ESP_FAIL;
        }
        if (pf != V4L2_PIX_FMT_YUYV && require_yuyv) {
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
    cam_converter_bypass("after STREAMON");
    vTaskDelay(pdMS_TO_TICKS(50));
    gc0308_dump_regs("after STREAMON", repair_yuv);
    return ESP_OK;
}


/*
 * Diagnostic: read back the GC0308's format-related registers over the shared SCCB bus (address
 * 0x21, 8-bit register, page 0) and log each against what esp_cam_sensor's table for
 * "YUV422_YUYV_320x240_20fps_subsample" writes (gc0308_select_page0_default +
 * gc0308_settings_yuv422_yuyv + init_reglist_DVP_8bit_320x240_20fps_subsample). exp = -1: the
 * table never writes it, so the value is the chip's power-on default. 0x25 is 0x0f only once
 * streaming; 0x1a bit 0 is cleared by set_stream. Mismatches are flagged with "DIFF".
 * repair: put 0x24 (output format) back to 0xa2 and clear 0x2e bit 0 (test pattern) if they differ.
 */
#define GC0308_ADDR 0x21
typedef struct { uint8_t reg; int exp; const char *what; } gc_reg_t;
static const gc_reg_t s_gc_regs[] = {
    { 0x00, 0x9b, "chip id" },
    { 0x24, 0xa2, "output format (a2 = YCbYCr, a0 = CbYCrY, a6 = RGB565, b1 = Y only)" },
    { 0x25, 0x0f, "output enable (0f = on)" },
    { 0x2e, -1, "debug mode (bit0 = test pattern)" },
    { 0x1a, 0x2a, "analog mode (bit0 clear = running)" },
    { 0x22, 0x57, "function enable (AWB etc.)" },
    { 0x23, -1, "function enable 2" },
    { 0x14, 0x11, "mirror/flip" },
    { 0x80, 0x03, "block enable 1" },
    { 0x81, -1, "block enable 2" },
    { 0x82, -1, "block enable 3" },
    { 0x83, -1, "block enable 4" },
    { 0xb1, 0x40, "saturation Cb" },
    { 0xb2, 0x40, "saturation Cr" },
    { 0xb3, 0x40, "luma contrast" },
    { 0xb6, 0xe0, "chroma enable/ABS" },
    { 0xd0, 0xcb, "AEC/ABS" },
    { 0xd2, 0x90, "AEC enable" },
    { 0xdb, 0x92, "colour/CC" },
    { 0xdc, 0xa5, "colour/CC" },
    { 0xdf, 0x23, "colour/CC" },
    { 0x50, 0x14, "crop/out window" },
    { 0x52, 0x41, "UV related" },
    { 0x53, 0x80, "UV gain" },
    { 0x54, 0x80, "UV gain" },
    { 0x55, 0x80, "UV gain" },
    { 0x56, 0x80, "UV gain" },
    { 0x93, 0x48, "cc matrix" },
    { 0x94, 0x02, "cc matrix" },
    { 0x95, 0x07, "cc matrix" },
    { 0x96, 0xe0, "cc matrix" },
    { 0x97, 0x40, "cc matrix" },
    { 0x98, 0xf0, "cc matrix" },
};

static esp_err_t gc_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
}

static void gc0308_dump_regs(const char *when, bool repair)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GC0308_ADDR,
        .scl_speed_hz = 100000,
    };
    if (!bus || i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "GC0308 regs (%s): cannot open SCCB device 0x%02x", when, GC0308_ADDR);
        return;
    }
    uint8_t v = 0;
    const uint8_t page0[] = { 0xfe, 0x00 };
    i2c_master_transmit(dev, page0, sizeof(page0), 100);   /* the reads below are page 0 */
    ESP_LOGI(TAG, "GC0308 regs (%s), page 0:", when);
    for (size_t i = 0; i < sizeof(s_gc_regs) / sizeof(s_gc_regs[0]); i++) {
        const gc_reg_t *r = &s_gc_regs[i];
        if (gc_read(dev, r->reg, &v) != ESP_OK) {
            ESP_LOGW(TAG, "  reg 0x%02x read failed (%s)", r->reg, r->what);
            continue;
        }
        if (r->exp < 0) {
            ESP_LOGI(TAG, "  reg 0x%02x = 0x%02x  (table: not written)  %s", r->reg, v, r->what);
        } else if (v == r->exp) {
            ESP_LOGI(TAG, "  reg 0x%02x = 0x%02x  (table 0x%02x, ok)  %s", r->reg, v, r->exp, r->what);
        } else {
            ESP_LOGW(TAG, "  reg 0x%02x = 0x%02x  DIFF (table 0x%02x)  %s", r->reg, v, r->exp, r->what);
        }
    }
    if (repair) {
        if (gc_read(dev, 0x24, &v) == ESP_OK && v != 0xa2) {
            const uint8_t w[] = { 0x24, 0xa2 };
            ESP_LOGW(TAG, "output format reg 0x24 was 0x%02x, rewriting 0xa2", v);
            i2c_master_transmit(dev, w, sizeof(w), 100);
        }
        if (gc_read(dev, 0x2e, &v) == ESP_OK && (v & 1)) {
            const uint8_t w[] = { 0x2e, (uint8_t)(v & ~1) };
            ESP_LOGW(TAG, "test pattern bit set in reg 0x2e (0x%02x), clearing", v);
            i2c_master_transmit(dev, w, sizeof(w), 100);
        }
    }
    i2c_master_bus_rm_device(dev);
}


/* Diagnostic: read-modify-write one page-0 GC0308 register over SCCB. Returns the value read back. */
static int gc_update_reg(uint8_t reg, uint8_t mask, uint8_t value)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = GC0308_ADDR,
        .scl_speed_hz = 100000,
    };
    if (!bus || i2c_master_bus_add_device(bus, &dc, &dev) != ESP_OK) {
        return -1;
    }
    int result = -1;
    uint8_t v = 0;
    const uint8_t page0[] = { 0xfe, 0x00 };
    i2c_master_transmit(dev, page0, sizeof(page0), 100);
    if (gc_read(dev, reg, &v) == ESP_OK) {
        const uint8_t w[] = { reg, (uint8_t)((v & ~mask) | (value & mask)) };
        if (i2c_master_transmit(dev, w, sizeof(w), 100) == ESP_OK && gc_read(dev, reg, &v) == ESP_OK) {
            result = v;
        }
    }
    i2c_master_bus_rm_device(dev);
    return result;
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
    err = stream_open(&s, true, V4L2_PIX_FMT_YUYV, true);
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
            jpg = heap_caps_malloc(JPEG_OUT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            jpeg_enc_config_t jc = DEFAULT_JPEG_ENC_CONFIG();
            jc.width = CAM_W;
            jc.height = CAM_H;
            jc.src_type = JPEG_PIXEL_FORMAT_YCbYCr;   /* Y0 Cb Y1 Cr: the standard YUYV order, no swap */
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

/* RGB565 diagnostic: the YUYV table stays loaded and only 0x24 changes to 0xa6 (the value of esp_cam_sensor's
 * gc0308_settings_rgb565_be), handed to esp_video as a "customer" sensor format through VIDIOC_S_SENSOR_FMT, so
 * the DVP driver is told the stream is RGB565 (big-endian: V4L2 RGB565X) too. esp_video keeps the pointer, so
 * both structs are static. Nothing outlives the capture: camera_down() powers the sensor off, and the next
 * capture starts from the YUYV default again. */
static const struct { uint8_t reg, val; } s_rgb565_regs[] = { { 0xfe, 0x00 }, { 0x24, 0xa6 } };
static esp_cam_sensor_format_t s_rgb565_fmt;
static esp_cam_sensor_format_t s_yuyv_fmt;

static esp_err_t sensor_to_rgb565(int fd)
{
    if (ioctl(fd, VIDIOC_G_SENSOR_FMT, &s_yuyv_fmt) != 0) {
        ESP_LOGE(TAG, "G_SENSOR_FMT failed");
        return ESP_FAIL;
    }
    s_rgb565_fmt = s_yuyv_fmt;
    s_rgb565_fmt.name = "DVP_8bit_RGB565_BE_320x240 (0x24=0xa6 on the YUYV table)";
    s_rgb565_fmt.format = ESP_CAM_SENSOR_PIXFORMAT_RGB565_BE;
    s_rgb565_fmt.regs = s_rgb565_regs;
    s_rgb565_fmt.regs_size = sizeof(s_rgb565_regs) / sizeof(s_rgb565_regs[0]);
    if (ioctl(fd, VIDIOC_S_SENSOR_FMT, &s_rgb565_fmt) != 0) {
        ESP_LOGE(TAG, "S_SENSOR_FMT (RGB565_BE) failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "sensor format switched to RGB565_BE (reg 0x24 <- 0xa6)");
    return ESP_OK;
}

esp_err_t cores3_camera_capture_raw_mode(cores3_raw_mode_t mode, uint8_t **data, size_t *len, int *width,
                                         int *height, uint32_t *fourcc)
{
    static const char *const names[] = { "raw", "testbar", "raw565" };
    const char *name = names[mode];
    heap_mark_t before = heap_mark();
    ESP_LOGI(TAG, "%s capture start", name);
    log_heap("raw capture start");
    esp_err_t err = camera_up();
    if (err != ESP_OK) {
        return err;
    }
    uint8_t *copy = NULL;
    stream_t s;
    memset(&s, 0, sizeof(s));
    s.fd = -1;
    bool pattern_on = false;
    if (mode == CORES3_RAW_RGB565) {
        /* The fd must be open to switch the sensor format; S_FMT comes after, so open it here. */
        int fd = open(BSP_CAMERA_DEVICE, O_RDONLY);
        if (fd < 0) {
            ESP_LOGE(TAG, "open %s failed", BSP_CAMERA_DEVICE);
            err = ESP_FAIL;
        } else {
            err = sensor_to_rgb565(fd);
            close(fd);   /* the sensor keeps the format; stream_open reopens the node and asks for RGB565X */
        }
    }
    if (err == ESP_OK) {
        err = mode == CORES3_RAW_RGB565 ? stream_open(&s, false, V4L2_PIX_FMT_RGB565X, false)
                                        : stream_open(&s, false, V4L2_PIX_FMT_YUYV, true);
    }
    if (err == ESP_OK && mode == CORES3_RAW_TESTBAR) {
        int r = gc_update_reg(0x2e, 0x01, 0x01);   /* test pattern on; stream_grab drops CAM_SETTLE_FRAMES first */
        pattern_on = r >= 0;
        ESP_LOGI(TAG, "testbar: test pattern %s (reg 0x2e = 0x%02x)", pattern_on ? "enabled" : "FAILED to enable",
                 r < 0 ? 0 : r);
        if (!pattern_on) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        const uint8_t *frame = NULL;
        size_t frame_len = 0;
        err = stream_grab(&s, &frame, &frame_len);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "%s frame: %u bytes (expected %u), unconverted", name, (unsigned)frame_len,
                     (unsigned)CAM_FRAME_BYTES);
            copy = heap_caps_malloc(frame_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!copy) {
                err = ESP_ERR_NO_MEM;
            } else {
                memcpy(copy, frame, frame_len);
                *data = copy;
                *len = frame_len;
                *width = CAM_W;
                *height = CAM_H;
                *fourcc = s.fourcc;
            }
        }
    }
    if (pattern_on) {
        int r = gc_update_reg(0x2e, 0x01, 0x00);
        ESP_LOGI(TAG, "testbar: test pattern %s (reg 0x2e = 0x%02x)", r >= 0 && !(r & 1) ? "restored" : "RESTORE FAILED",
                 r < 0 ? 0 : r);
    }
    if (mode == CORES3_RAW_RGB565 && s.fd >= 0) {
        /* Back to the YUV422 table's output format before the sensor is powered off. */
        if (ioctl(s.fd, VIDIOC_S_SENSOR_FMT, &s_yuyv_fmt) == 0) {
            ESP_LOGI(TAG, "raw565: sensor format restored to YUYV");
        } else {
            ESP_LOGW(TAG, "raw565: could not restore YUYV (the next capture re-initialises the sensor anyway)");
        }
    }
    stream_close(&s);
    camera_down();
    if (err != ESP_OK) {
        free(copy);
    }
    ESP_LOGI(TAG, "%s capture %s", name, err == ESP_OK ? "ok" : "FAILED");
    log_heap("raw capture done, camera off");
    log_heap_delta(&before);
    return err;
}

esp_err_t cores3_camera_capture_raw(uint8_t **data, size_t *len, int *width, int *height, uint32_t *fourcc)
{
    return cores3_camera_capture_raw_mode(CORES3_RAW_NORMAL, data, len, width, height, fourcc);
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
