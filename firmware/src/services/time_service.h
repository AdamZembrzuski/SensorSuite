#ifndef TIME_SERVICE_H_
#define TIME_SERVICE_H_

#include <zephyr/types.h>


/** @brief Sets the base time for the system.
 * @param current_unix_time_ms The current time in Unix epoch format (milliseconds)
 * @return void
 */
void set_real_time(int64_t current_unix_time_ms);

/** @brief Returns the current timestamp in Unix epoch format (milliseconds)
 * @return Current timestamp in milliseconds since January 1, 1970
 */
int64_t get_current_timestamp(void);

#endif