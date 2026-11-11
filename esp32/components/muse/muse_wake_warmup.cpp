/*
 * Wake word: run the C++ function-local-static machinery once, early, from a
 * normal task. See muse_wake_warmup() in muse_wake.h.
 */
#include "muse_wake.h"

#if CONFIG_MUSE_WAKE_WORD_HI_MUSE

#include "dl_module_creator.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "muse_wake";

namespace {
struct Probe {
    int v = 1;
};
}  // namespace

extern "C" void muse_wake_warmup(void)
{
    static bool done;   // constant-initialised: no guard involved
    if (done) return;
    done = true;
    ESP_LOGI(TAG, "warmup: scheduler state %d (1=not started, 2=running), task \"%s\", core %d",
             (int)xTaskGetSchedulerState(), pcTaskGetName(NULL), (int)xPortGetCoreID());
    /* First guarded static in the program (if nothing before us): creates the
     * runtime's init mutex/semaphore now, while internal RAM is unfragmented. */
    static Probe probe;
    (void)probe;
    /* esp-dl's singleton, the one wakenetx's build_model() takes later. */
    dl::module::ModuleCreator *mc = dl::module::ModuleCreator::get_instance();
    ESP_LOGI(TAG, "warmup: ModuleCreator singleton %p ready", (void *)mc);
}

#endif
