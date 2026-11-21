/* LTR-553ALS-WA proximity sensor (CoreS3 StackChan, top of the head, internal I2C 0x23).
 * Diagnostic stage: reads the raw PS value. Other boards: unsupported. */
#pragma once

#include <stdbool.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_MUSE_BOARD_M5STACK_CORES3
/* Returns at once; set-up and 100 ms polling run on their own task. Logs and stays disabled on failure. */
void muse_prox_start(void);
bool muse_prox_available(void);
/* Latest raw PS (11 bit, 0..2047), or -1 if unavailable. */
int muse_prox_ps(void);
bool muse_prox_saturated(void);
int muse_prox_part_id(void);   /* PART_ID read at init, -1 if never read */
#else
static inline void muse_prox_start(void) {}
static inline bool muse_prox_available(void) { return false; }
static inline int muse_prox_ps(void) { return -1; }
static inline bool muse_prox_saturated(void) { return false; }
static inline int muse_prox_part_id(void) { return -1; }
#endif

#ifdef __cplusplus
}
#endif
