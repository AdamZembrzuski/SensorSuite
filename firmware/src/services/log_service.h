#ifndef LOG_SERVICE_H_
#define LOG_SERVICE_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include <stdint.h>
#include <stddef.h>

#define RB_STAMP_SIZE   (32U * 1024U)  
#define RB_AMBIENT_SIZE (4U * 1024U)  

#define BULK_HDR_LEN        4
#define BULK_FLAG_LAST      0x01
#define STREAM_ID_STAMP        1
#define STREAM_ID_AMBIENT      2

#define BULK_PKT_MAX        256U

extern struct ring_buf rb_stamp;
extern struct ring_buf rb_ambient;

/** @brief Puts data into the ambient ring buffer
 * @param data Pointer to the data to be stored
 * @param len Length of the data in bytes
 * @return 0 for success, error code on failure
 */
int rb_ambient_put(const uint8_t *data, size_t len);

/** @brief Gets data from the ambient ring buffer
 * @param data Pointer to the buffer where retrieved data will be stored
 * @param len Maximum number of bytes to retrieve
 * @return 0 for success, error code on failure
 */
int rb_ambient_get(uint8_t *data, size_t len);

/** @brief Puts a timestamp value into the stamp ring buffer
 * @param value The timestamp value to be stored
 * @return 0 on success, negative error code on failure
 */
int rb_stamp_put(int32_t value);

/** @brief Gets a timestamp value from the stamp ring buffer
 * @param value Pointer to an int32_t variable where the retrieved timestamp will be stored
 * @return 0 on success, negative error code on failure
 */
int rb_stamp_get(int32_t *value);

/** @brief Gets the count of timestamp records currently stored in the stamp ring buffer
 * @return Number of timestamp records in the stamp ring buffer
 */
uint32_t rb_stamp_count(void);

/** @brief Initializes bulk streaming
 * @return void
 */
void bulk_stream_init(void);

/** @brief Starts bulk streaming
 * @return void
 */
void bulk_stream_start(void);

/** @brief Stops bulk streaming
 * @return void
 */
void bulk_stream_stop(void);


#endif