#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include "time_service.h"

static int64_t unix_base_time_ms = 0; 
static bool time_set = false;
static time_changed_cb_t time_changed_cb = NULL;

//No thread synchronisation necessary
void set_real_time(int64_t current_unix_time_ms) {

    unix_base_time_ms = current_unix_time_ms - k_uptime_get();
    time_set = true;

    if (time_changed_cb) {
        time_changed_cb();
    }

}

int64_t get_current_timestamp(void) {

    return unix_base_time_ms + k_uptime_get();

}

bool is_time_set(void) {
    return time_set;
}

int time_service_init(time_changed_cb_t cb) {
    time_changed_cb = cb;
    return 0;
}