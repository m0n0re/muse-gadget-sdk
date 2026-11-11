/*
 * Local extension of this fork: StackChan base LEDs. See muse_led.h.
 *
 * Control path (from M5's M5IOE1 library as I know it; NOTHING in this tree confirms it, so
 * every step is read back and logged):
 *   - LED data leaves the expander on its LED pin (IO14, bit 13 of the 16-bit pin registers).
 *     That pin is kept an enabled push-pull output (MODE 0x03 / OUT 0x05 / OD 0x13, the
 *     16-bit pair registers the servo code already uses for VM EN).
 *   - LED_CFG (0x30, 8 bit): bits 5..0 = LED count, bit 6 = refresh (push the RAM to the strip).
 *   - LED RAM from 0x40, 2 bytes per LED, RGB565, low byte first.
 *   - The 5 V bus boost (CoreS3 AW9523 P1_7 = BOOST_EN, expander pin 15 in the BSP numbering)
 *     feeds the strip per M5's ESPHome notes; it is switched on through the BSP's expander.
 * If the strip stays dark, the log lines "led: ..." say which readback differed.
 */
#include "muse_led.h"

#if CONFIG_MUSE_BOARD_M5STACK_CORES3

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "muse_servo.h"
#include "muse_state.h"

static const char *TAG = "led";

#define LED_COUNT 12
#define LED_PIN_BIT 0x2000          /* IO14 = bit 13 */
#define REG_MODE 0x03
#define REG_OUT 0x05
#define REG_OD 0x13
#define REG_LED_CFG 0x30
#define REG_LED_RAM 0x40
#define LED_CFG_REFRESH 0x40
#define BOOST_EN_PIN IO_EXPANDER_PIN_NUM_15   /* AW9523 P1_7 */
#define HW_CAP 0.25f                /* brightness 100 % = this fraction of full */
#define POLL_MS 100
#define BREATH_STEP_MS 150
#define BREATH_PERIOD_MS 3200

static SemaphoreHandle_t s_mu;
static volatile bool s_ok;
static bool s_manual;               /* guarded by s_mu */
static int s_cur[3] = { -1, -1, -1 };   /* last colour written (after scaling), guarded by s_mu */

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* r,g,b are final 0..255 values (already scaled). Skips the bus when nothing changed. */
static bool write_color_locked(int r, int g, int b, const char *why)
{
    if (r == s_cur[0] && g == s_cur[1] && b == s_cur[2]) return true;
    uint8_t ram[LED_COUNT * 2];
    uint16_t c = rgb565(r, g, b);
    for (int i = 0; i < LED_COUNT; i++) {
        ram[2 * i] = (uint8_t)c;
        ram[2 * i + 1] = (uint8_t)(c >> 8);
    }
    uint8_t cfg = LED_COUNT | LED_CFG_REFRESH;
    esp_err_t err = muse_ioe_write(REG_LED_RAM, ram, sizeof(ram));
    if (err == ESP_OK) err = muse_ioe_write(REG_LED_CFG, &cfg, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write (%s) rgb %d,%d,%d failed: %s", why, r, g, b, esp_err_to_name(err));
        s_cur[0] = s_cur[1] = s_cur[2] = -1;
        return false;
    }
    s_cur[0] = r; s_cur[1] = g; s_cur[2] = b;
    ESP_LOGI(TAG, "%s: rgb %d,%d,%d (rgb565 0x%04x)", why, r, g, b, c);
    return true;
}

static bool show_locked(int r, int g, int b, float scale, const char *why)
{
    scale = scale < 0 ? 0 : scale > 1 ? 1 : scale;
    float k = scale * HW_CAP;
    return write_color_locked((int)lroundf(r * k), (int)lroundf(g * k), (int)lroundf(b * k), why);
}

typedef struct { int r, g, b; float scale; const char *name; } mode_color_t;

static bool mode_color(muse_mode_t m, mode_color_t *c)
{
    switch (m) {
    case MUSE_MODE_BOOT:
    case MUSE_MODE_IDLE:      *c = (mode_color_t){ 255, 150, 70, 0.30f, "idle" }; return true;
    case MUSE_MODE_LISTENING: *c = (mode_color_t){ 80, 255, 110, 0.80f, "listening" }; return true;
    case MUSE_MODE_THINKING:  *c = (mode_color_t){ 70, 120, 255, 0.70f, "thinking" }; return true;
    case MUSE_MODE_SPEAKING:  *c = (mode_color_t){ 255, 190, 110, 0.70f, "speaking" }; return true;
    case MUSE_MODE_ERROR:     *c = (mode_color_t){ 255, 20, 20, 0.90f, "error" }; return true;
    default:                  return false;   /* OFF: dark */
    }
}

static void led_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));   /* muse_state_init runs a little after muse_led_start */
    /* 1. The 5 V boost, through the BSP's AW9523 (shared with its own users, so only our pin). */
    esp_io_expander_handle_t io = bsp_io_expander_init();
    if (io) {
        esp_err_t e = esp_io_expander_set_dir(io, BOOST_EN_PIN, IO_EXPANDER_OUTPUT);
        if (e == ESP_OK) e = esp_io_expander_set_level(io, BOOST_EN_PIN, 1);
        ESP_LOGI(TAG, "BOOST_EN (AW9523 P1_7) high: %s", esp_err_to_name(e));
    } else {
        ESP_LOGW(TAG, "BOOST_EN: no BSP io expander handle");
    }

    /* 2. The LED pin of the M5IOE1: push-pull, output, high. */
    esp_err_t err = muse_ioe_update16(REG_OD, LED_PIN_BIT, 0);
    if (err == ESP_OK) err = muse_ioe_update16(REG_OUT, LED_PIN_BIT, LED_PIN_BIT);
    if (err == ESP_OK) err = muse_ioe_update16(REG_MODE, LED_PIN_BIT, LED_PIN_BIT);
    uint8_t mode[2] = {0}, out[2] = {0}, od[2] = {0};
    if (err == ESP_OK) err = muse_ioe_read(REG_MODE, mode, 2);
    if (err == ESP_OK) err = muse_ioe_read(REG_OUT, out, 2);
    if (err == ESP_OK) err = muse_ioe_read(REG_OD, od, 2);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED pin set-up failed: %s: LEDs disabled", esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "LED pin (bit 13): MODE=0x%02x%02x OUT=0x%02x%02x OD=0x%02x%02x", mode[1], mode[0], out[1], out[0],
             od[1], od[0]);

    /* 3. LED count, then a readback of it and of the first RAM word after a first write. */
    uint8_t cfg = LED_COUNT;
    err = muse_ioe_write(REG_LED_CFG, &cfg, 1);
    uint8_t cfg_rb = 0xFF;
    if (err == ESP_OK) err = muse_ioe_read(REG_LED_CFG, &cfg_rb, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED_CFG (0x%02x) access failed: %s: LEDs disabled", REG_LED_CFG, esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "LED_CFG wrote 0x%02x, read back 0x%02x (%s)", cfg, cfg_rb,
             (cfg_rb & 0x3F) == LED_COUNT ? "count took" : "count NOT taken: the register map may differ");

    xSemaphoreTake(s_mu, portMAX_DELAY);
    mode_color_t c;
    float secs;
    muse_mode_t last = muse_state_mode(&secs);
    if (mode_color(last, &c)) show_locked(c.r, c.g, c.b, c.scale, c.name);
    else write_color_locked(0, 0, 0, "off");
    uint8_t ram_rb[2] = {0};
    if (muse_ioe_read(REG_LED_RAM, ram_rb, 2) == ESP_OK) {
        ESP_LOGI(TAG, "LED RAM[0] reads back 0x%02x%02x", ram_rb[1], ram_rb[0]);
    }
    s_ok = true;
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "LEDs ready: %d LEDs, cap %.0f%%, following the avatar mode", LED_COUNT, HW_CAP * 100);

    int64_t breath_ms = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(last == MUSE_MODE_SPEAKING ? BREATH_STEP_MS : POLL_MS));
        muse_mode_t m = muse_state_mode(&secs);
        xSemaphoreTake(s_mu, portMAX_DELAY);
        if (m != last) {
            if (s_manual) ESP_LOGI(TAG, "mode %d: manual colour released", m);
            s_manual = false;
            last = m;
            breath_ms = 0;
            if (mode_color(m, &c)) show_locked(c.r, c.g, c.b, c.scale, c.name);
            else write_color_locked(0, 0, 0, "off");
        } else if (!s_manual && m == MUSE_MODE_SPEAKING && mode_color(m, &c)) {
            breath_ms += BREATH_STEP_MS;
            float ph = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)(breath_ms % BREATH_PERIOD_MS) / BREATH_PERIOD_MS);
            show_locked(c.r, c.g, c.b, c.scale * (0.15f + 0.85f * ph), "speaking-breath");
        }
        xSemaphoreGive(s_mu);
    }
}

void muse_led_start(void)
{
    s_mu = xSemaphoreCreateMutex();
    TaskHandle_t t;
    if (!s_mu || xTaskCreateWithCaps(led_task, "led", 4096, NULL, 3, &t, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "LED task not started: LEDs disabled");
    }
}

bool muse_led_available(void) { return s_ok; }

bool muse_led_set(int r, int g, int b, int brightness)
{
    if (!s_ok) return false;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    brightness = brightness < 0 ? 0 : brightness > 100 ? 100 : brightness;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_manual = true;
    ESP_LOGI(TAG, "led.set r=%d g=%d b=%d brightness=%d%%", r, g, b, brightness);
    bool ok = show_locked(r, g, b, brightness / 100.0f, "manual");
    xSemaphoreGive(s_mu);
    return ok;
}

bool muse_led_off(void)
{
    if (!s_ok) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_manual = true;
    bool ok = write_color_locked(0, 0, 0, "manual off");
    xSemaphoreGive(s_mu);
    return ok;
}

#endif
