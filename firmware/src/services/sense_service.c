#include <zephyr/types.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <errno.h>

#include "sense_service.h"
#include "time_service.h"



static uint32_t object_count = 0;
static int32_t temperature = 0;
static uint32_t humidity = 0;
static int64_t  timestamp = 0;
static uint8_t cmnd_ret = 0;

static bool notify_count_enabled;
static bool notify_temp_enabled;
static bool notify_humid_enabled;

static struct sense_cb sense_cb;

/* All ccc cfg changed currently unused */
static void sense_ccc_count_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_count_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void sense_ccc_temp_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_temp_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void sense_ccc_humid_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_humid_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void sense_ccc_bulk_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(value);
}

static ssize_t count_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			 uint16_t len, uint16_t offset)
{
	const uint32_t *value = attr->user_data;


	if (sense_cb.count_cb) {
		object_count = sense_cb.count_cb();
		return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
	}

	return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);

}

static ssize_t temp_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{	
	const int32_t *value = attr->user_data;


	if (sense_cb.temp_cb) {
		temperature = sense_cb.temp_cb();
		return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
	}

	return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
}

static ssize_t humid_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
    const uint32_t *value = attr->user_data;


	if (sense_cb.humid_cb) {
		humidity = sense_cb.humid_cb();
		return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
	}

	return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);

}

static ssize_t time_read_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data, sizeof(uint64_t));
}

static ssize_t time_read_curr_cb(struct bt_conn *conn,
                                 const struct bt_gatt_attr *attr,
                                 void *buf, uint16_t len, uint16_t offset)
{
    int64_t now_ms = get_current_timestamp();
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             &now_ms, sizeof(now_ms));
}

static ssize_t time_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			   uint16_t len, uint16_t offset, uint8_t flags)
{
	if (len != sizeof(int64_t)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

    if (offset != 0) {
    	return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    int64_t val = sys_get_le64(buf);

    if (val <= 0){
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }

	set_real_time(val);


	int64_t *stored_time = attr->user_data;
	*stored_time = val;

    return len;

}

static ssize_t cmnd_read_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data, sizeof(uint8_t));
}


static ssize_t cmnd_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			   uint16_t len, uint16_t offset, uint8_t flags)
{
    if (len != 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

    if (offset != 0) {
    	return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    uint8_t val = *((uint8_t *)buf);

    /*if (val > 0x40){
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }*/

    if (sense_cb.cmnd_cb){
        uint8_t *storage = attr->user_data;
        *storage = sense_cb.cmnd_cb(val);
    }

    return len;
}

/* Service Declaration */
BT_GATT_SERVICE_DEFINE(
	sense_service_svc, BT_GATT_PRIMARY_SERVICE(BT_SERVICE_UUID),

	BT_GATT_CHARACTERISTIC(BT_COUNT_UUID, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, count_cb, NULL, &object_count),
	BT_GATT_CCC(sense_ccc_count_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    BT_GATT_CHARACTERISTIC(BT_TEMP_UUID, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, temp_cb, NULL, &temperature),
	BT_GATT_CCC(sense_ccc_temp_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    BT_GATT_CHARACTERISTIC(BT_HUMIDITY_UUID, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, humid_cb, NULL, &humidity),
	BT_GATT_CCC(sense_ccc_humid_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    BT_GATT_CHARACTERISTIC(BT_BULK_UUID, BT_GATT_CHRC_NOTIFY,
			       0, NULL, NULL, NULL),
	BT_GATT_CCC(sense_ccc_bulk_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_CURR_TIMESTAMP_UUID, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, time_read_curr_cb, NULL, NULL),

    BT_GATT_CHARACTERISTIC(BT_BASE_TIMESTAMP_UUID, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ,
			       BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_READ_ENCRYPT, time_read_cb, time_write_cb, &timestamp),

    BT_GATT_CHARACTERISTIC(BT_COMMAND_UUID, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ,
                    BT_GATT_PERM_WRITE_ENCRYPT | BT_GATT_PERM_READ_ENCRYPT, cmnd_read_cb, cmnd_cb, &cmnd_ret),
    );

/* A function to register application callbacks for the  characteristics  */
int sense_service_init(struct sense_cb *callbacks)
{	
	if (callbacks == NULL) {
		return -EINVAL;
	}

	sense_cb = *callbacks;

	return 0;
}

/*int sense_service_object_count_notify(uint32_t object_count)
{
	if (!notify_count_enabled) {
		return -EACCES;
	}

	return bt_gatt_notify(NULL, &sense_service_svc.attrs[2], &object_count, sizeof(object_count));
}

int sense_service_temperature_notify(int32_t temperature)
{
	if (!notify_temp_enabled) {
		return -EACCES;
	}

	return bt_gatt_notify(NULL, &sense_service_svc.attrs[5], &temperature, sizeof(temperature));
}

int sense_service_humidity_notify(uint32_t humidity)
{
	if (!notify_humid_enabled) {
		return -EACCES;
	}

	return bt_gatt_notify(NULL, &sense_service_svc.attrs[8], &humidity, sizeof(humidity));
}*/

