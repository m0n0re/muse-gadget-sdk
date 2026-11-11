/*
 * Local extension of this fork: the 12 RGB LEDs (WS2812-class) on the front of the StackChan
 * base, driven through the body's IO expander (0x6F, M5IOE1 family; see muse_servo.h for the
 * shared access). CoreS3 only; other boards get empty stubs.
 *
 * The LEDs follow the avatar mode (muse_state) unless led.set / led.off took over; the
 * next mode change hands them back.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_BOARD_M5STACK_CORES3
/* After muse_servo_start (the expander handle). Returns at once; set-up and the mode
 * follow-up run on the LED task. Logs and stays disabled on failure. */
void muse_led_start(void);
bool muse_led_available(void);
/* Manual colour for all 12: r/g/b 0..255, brightness 0..100 (%, 100 = the hardware cap). */
bool muse_led_set(int r, int g, int b, int brightness);
bool muse_led_off(void);
/* Diagnostic: cycles pin/LED_CFG hypotheses, all 12 white for 4 s each (about 24 s, async).
 * False if already running or unavailable. While it runs, set/off return false (busy). */
bool muse_led_diag(void);
bool muse_led_busy(void);
#else
static inline void muse_led_start(void) {}
static inline bool muse_led_available(void) { return false; }
static inline bool muse_led_set(int r, int g, int b, int brightness) { return false; }
static inline bool muse_led_off(void) { return false; }
static inline bool muse_led_diag(void) { return false; }
static inline bool muse_led_busy(void) { return false; }
#endif

#ifdef __cplusplus
}
#endif
