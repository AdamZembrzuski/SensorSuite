#ifndef SENSE_SERVICE_H_
#define SENSE_SERVICE_H_

#include <zephyr/types.h>

// BASE_UUID 229a0000-ad33-4a06-9bce-c34201743655

#define BT_SERVICE_UUID_VAL BT_UUID_128_ENCODE(0x229a0001, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_COUNT_UUID_VAL BT_UUID_128_ENCODE(0x229a0002, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_TEMP_UUID_VAL BT_UUID_128_ENCODE(0x229a0003, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_HUMIDITY_UUID_VAL BT_UUID_128_ENCODE(0x229a0004, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_BULK_UUID_VAL BT_UUID_128_ENCODE(0x229a0005, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_CURR_TIMESTAMP_UUID_VAL BT_UUID_128_ENCODE(0x229a0006, 0xad33, 0x4a06, 0x9bce, 0xc34201743655)
#define BT_BASE_TIMESTAMP_UUID_VAL BT_UUID_128_ENCODE(0x229a0007, 0xad33, 0x4a06, 0x9bce, 0xc34201743655) 
#define BT_COMMAND_UUID_VAL BT_UUID_128_ENCODE(0x229a0008, 0xad33, 0x4a06, 0x9bce, 0xc34201743655) 

#define BT_SERVICE_UUID BT_UUID_DECLARE_128(BT_SERVICE_UUID_VAL)
#define BT_COUNT_UUID BT_UUID_DECLARE_128(BT_COUNT_UUID_VAL)
#define BT_TEMP_UUID BT_UUID_DECLARE_128(BT_TEMP_UUID_VAL)
#define BT_HUMIDITY_UUID BT_UUID_DECLARE_128(BT_HUMIDITY_UUID_VAL)
#define BT_BULK_UUID BT_UUID_DECLARE_128(BT_BULK_UUID_VAL)
#define BT_CURR_TIMESTAMP_UUID BT_UUID_DECLARE_128(BT_CURR_TIMESTAMP_UUID_VAL)
#define BT_BASE_TIMESTAMP_UUID BT_UUID_DECLARE_128(BT_BASE_TIMESTAMP_UUID_VAL)
#define BT_COMMAND_UUID BT_UUID_DECLARE_128(BT_COMMAND_UUID_VAL)

typedef uint32_t (*count_cb_t)(void);
typedef int32_t (*temp_cb_t)(void);
typedef uint32_t (*humid_cb_t)(void);

typedef uint8_t (*cmnd_cb_t)(uint8_t command);

struct sense_cb {
    count_cb_t  count_cb;
    temp_cb_t   temp_cb;
    humid_cb_t  humid_cb;
    cmnd_cb_t   cmnd_cb;
};
/** @brief Initialises the sense service with the given callbacks
 * @param callbacks A pointer to a struct sense_cb containing the
 * application callbacks for the characteristics. Can be NULL if 
 * no callbacks are needed.
 * @return 0 on success
 */
int sense_service_init(struct sense_cb *callbacks);

/** @brief Notifies the object count characteristic with the given value
 * @param object_count The object count to notify
 * @return 0 on success, -EACCES if notifications are not enabled
 */
int sense_service_object_count_notify(uint32_t object_count);

/** @brief Notifies the temperature characteristic with the given value
 * @param temperature The temperature to notify
 * @return 0 on success, -EACCES if notifications are not enabled
 */
int sense_service_temperature_notify(int32_t temperature);

/** @brief Notifies the humidity characteristic with the given value
 * @param humidity The humidity to notify
 * @return 0 on success, -EACCES if notifications are not enabled
 */
int sense_service_humidity_notify(uint32_t humidity);

/** @brief Notifies the bulk characteristic with the given data
 * @param data A pointer to the data to notify
 * @param len The length of the data to notify
 * @return 0 on success, -EACCES if notifications are not enabled, 
 * -EINVAL if data is NULL or len is 0
 */
int sense_service_bulk_notify(const uint8_t *data, uint16_t len);

#endif