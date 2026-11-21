/* Si12T capacitive touch IC (CoreS3 StackChan, 3 pads on top of the head, internal I2C 0x68).
 * Diagnostic stage: reads the 3 channel strengths (0..3) every 50 ms. Other boards: unsupported.
 * (Module/file names kept from the earlier LTR-553 attempt.) */
#pragma once

#include <stdbool.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_BOARD_M5STACK_CORES3
/* Returns at once; set-up and 50 ms polling run on their own task. Logs and stays disabled on failure. */
void muse_prox_start(void);
bool muse_prox_available(void);
/* Strength of pad ch (0..2): 0 none, 1 weak, 2 medium, 3 strong. 0 if unavailable. */
int muse_prox_touch(int ch);
#else
static inline void muse_prox_start(void) {}
static inline bool muse_prox_available(void) { return false; }
static inline int muse_prox_touch(int ch) { (void)ch; return 0; }
#endif

#ifdef __cplusplus
}
#endif
