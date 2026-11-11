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
static int s_man[4];                /* last manual r,g,b,brightness, guarded by s_mu */
static bool s_redraw;               /* re-apply the mode colour on the next poll, guarded by s_mu */
static volatile bool s_diag;        /* led.diag running: every other led command is busy */

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

/* ---- led.diag: cycle the open hypotheses and light all 12 white at each, 4 s apiece ---- */

#define DIAG_STEP_MS 4000

typedef struct { const char *name; uint16_t pin; uint8_t cfg; bool toggle; } diag_step_t;

static const diag_step_t DIAG_STEPS[] = {
    { "A", 0x2000, LED_COUNT, false },
    { "B", 0x2000, LED_COUNT | LED_CFG_REFRESH, false },
    { "C", 0x4000, LED_COUNT, false },
    { "D", 0x4000, LED_COUNT | LED_CFG_REFRESH, false },
    /* Extras: refresh bit then straight back to 0x0c, in case the bit is a self-clearing strobe. */
    { "E", 0x2000, LED_COUNT | LED_CFG_REFRESH, true },
    { "F", 0x4000, LED_COUNT | LED_CFG_REFRESH, true },
};

static esp_err_t diag_pin(uint16_t bit)
{
    esp_err_t e = muse_ioe_update16(REG_OD, bit, 0);
    if (e == ESP_OK) e = muse_ioe_update16(REG_OUT, bit, bit);
    if (e == ESP_OK) e = muse_ioe_update16(REG_MODE, bit, bit);
    return e;
}

static void diag_task(void *arg)
{
    (void)arg;
    /* Level that every step shows: white at the hardware cap, same scaling as led.set 100 %. */
    int v = (int)lroundf(255 * HW_CAP);
    uint8_t ram[LED_COUNT * 2];
    uint16_t c = rgb565(v, v, v);
    for (int i = 0; i < LED_COUNT; i++) { ram[2 * i] = (uint8_t)c; ram[2 * i + 1] = (uint8_t)(c >> 8); }

    /* Remember the state of the alternative pin (bit 14) to put it back afterwards. */
    uint8_t m0[2] = {0}, o0[2] = {0}, d0[2] = {0};
    bool saved = muse_ioe_read(REG_MODE, m0, 2) == ESP_OK && muse_ioe_read(REG_OUT, o0, 2) == ESP_OK &&
                 muse_ioe_read(REG_OD, d0, 2) == ESP_OK;
    ESP_LOGI(TAG, "led.diag start: white %d,%d,%d (rgb565 0x%04x), %d ms per step; steps A..F", v, v, v, c,
             DIAG_STEP_MS);
    for (size_t i = 0; i < sizeof(DIAG_STEPS) / sizeof(DIAG_STEPS[0]); i++) {
        const diag_step_t *st = &DIAG_STEPS[i];
        /* Data pin: configure the one under test (the other keeps whatever state it has). */
        esp_err_t e = diag_pin(st->pin);
        if (e == ESP_OK) e = muse_ioe_write(REG_LED_RAM, ram, sizeof(ram));
        uint8_t cfg = st->cfg;
        if (e == ESP_OK) e = muse_ioe_write(REG_LED_CFG, &cfg, 1);
        ESP_LOGI(TAG, "led.diag step %s: pin=%d cfg=0x%02x%s -> %s", st->name, st->pin == 0x2000 ? 13 : 14, st->cfg,
                 st->toggle ? " (refresh then back to 0x0c after 50 ms)" : "", esp_err_to_name(e));
        if (st->toggle && e == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            uint8_t back = LED_COUNT;
            e = muse_ioe_write(REG_LED_CFG, &back, 1);
            ESP_LOGI(TAG, "led.diag step %s: cfg toggled back to 0x%02x -> %s", st->name, back, esp_err_to_name(e));
        }
        vTaskDelay(pdMS_TO_TICKS(DIAG_STEP_MS));
        ESP_LOGI(TAG, "led.diag step %s end", st->name);
    }
    /* Restore: bit 14 as it was, data pin back on bit 13, count only, then the old display. */
    if (saved) {
        uint16_t b = 0x4000;
        uint16_t mo = (uint16_t)(m0[0] | (m0[1] << 8)), oo = (uint16_t)(o0[0] | (o0[1] << 8)),
                 dd = (uint16_t)(d0[0] | (d0[1] << 8));
        if ((mo & b) == 0) muse_ioe_update16(REG_MODE, b, 0);
        muse_ioe_update16(REG_OUT, b, oo & b);
        muse_ioe_update16(REG_OD, b, dd & b);
    }
    diag_pin(LED_PIN_BIT);
    uint8_t cfg = LED_COUNT;
    muse_ioe_write(REG_LED_CFG, &cfg, 1);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_cur[0] = s_cur[1] = s_cur[2] = -1;
    if (s_manual) show_locked(s_man[0], s_man[1], s_man[2], s_man[3] / 100.0f, "diag-restore manual");
    else s_redraw = true;
    s_diag = false;
    xSemaphoreGive(s_mu);
    ESP_LOGI(TAG, "led.diag done: pin bit 13 / cfg 0x%02x restored, display %s", cfg,
             s_manual ? "back to the manual colour" : "back to following the mode");
    vTaskDelete(NULL);
}

bool muse_led_diag(void)
{
    if (!s_ok) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    bool busy = s_diag;
    s_diag = true;
    xSemaphoreGive(s_mu);
    if (busy) return false;
    TaskHandle_t t;
    if (xTaskCreateWithCaps(diag_task, "led_diag", 4096, NULL, 3, &t, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_diag = false;
        return false;
    }
    return true;
}

bool muse_led_busy(void) { return s_diag; }

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
        if (s_diag) {
            /* led.diag owns the strip */
        } else if (m != last) {
            if (s_manual) ESP_LOGI(TAG, "mode %d: manual colour released", m);
            s_manual = false;
            last = m;
            breath_ms = 0;
            if (mode_color(m, &c)) show_locked(c.r, c.g, c.b, c.scale, c.name);
            else write_color_locked(0, 0, 0, "off");
        } else if (s_redraw) {
            s_redraw = false;
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
    if (!s_ok || s_diag) return false;
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    brightness = brightness < 0 ? 0 : brightness > 100 ? 100 : brightness;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_diag) { xSemaphoreGive(s_mu); return false; }
    s_manual = true;
    s_man[0] = r; s_man[1] = g; s_man[2] = b; s_man[3] = brightness;
    ESP_LOGI(TAG, "led.set r=%d g=%d b=%d brightness=%d%%", r, g, b, brightness);
    bool ok = show_locked(r, g, b, brightness / 100.0f, "manual");
    xSemaphoreGive(s_mu);
    return ok;
}

bool muse_led_off(void)
{
    if (!s_ok || s_diag) return false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_diag) { xSemaphoreGive(s_mu); return false; }
    s_manual = true;
    s_man[0] = s_man[1] = s_man[2] = s_man[3] = 0;
    bool ok = write_color_locked(0, 0, 0, "manual off");
    xSemaphoreGive(s_mu);
    return ok;
}

#endif
