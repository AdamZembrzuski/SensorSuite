#include <zephyr/kernel.h>
#include <zephyr/types.h>

static int64_t unix_base_time_ms = 0; 

//No synchronisation necessary
void set_real_time(int64_t current_unix_time_ms) {

    unix_base_time_ms = current_unix_time_ms - k_uptime_get();

}

int64_t get_current_timestamp(void) {

    return unix_base_time_ms + k_uptime_get();

}