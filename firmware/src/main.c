/* System includes*/
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/posix/time.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h> 
#include <zephyr/devicetree.h>  
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/settings/settings.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/arch/exception.h>
#include <zephyr/drivers/sensor.h>


/* Bluetooth includes */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/gap.h>


/* Application includes */
#include "sense_service.h"
#include "time_service.h"
#include "log_service.h"
#include "error_service.h"
#include "VL53L4CD_ULP_api.h"


/* Development includes */
#include <zephyr/logging/log.h>


/* Misc definitions */
#define FW_VERSION 3 //Major release 0, minor release 3


/* BLE definitions */
#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* Sensor definitions */
#define DETECTION_INTERVAL 240 // in ms (must end in 0, eg 80, 120, 500, NOT 143)
#define CONSEC_ADVERTISING_START 9600 // in ms (must be a multiple of DETECTION_INTERVAL)

#define VL53_DISTANCE_THRESH_MM 910

#define AMBIENT_PERIOD_MS (15U * 1000U)


#define I2C_CHECK_PERIOD_MINS 30


/* Command definitions */

// Connection interval commands, return 0 on success
#define CMND_CONN_INTERVAL_20ms 0x00
#define CMND_CONN_INTERVAL_100ms 0x01
#define CMND_CONN_INTERVAL_512ms 0x02
#define CMND_CONN_INTERVAL_1024ms 0x03

// Download commands, notifications must be enabled
#define CMND_STREAM_START 0x10
#define CMND_STREAM_STOP 0x11

// Firmware version command
#define CMND_FW_VER 0x20 

static void adv_work_handler(struct k_work *work);
static void bt_off_work_handler(struct k_work *work);
static void security_work_handler(struct k_work *work);
static void counter_work_handler(struct k_work *work);
static void vl53_stop_work_handler(struct k_work *work);
static void vl53_start_work_handler(struct k_work *work);
static void ambient_work_handler(struct k_work *work);
static void i2c_check_work_handler(struct k_work *work);

/* BLE variables */
static bool bt_ready = false;

static struct bt_le_ext_adv_start_param start_param = {
    .timeout = 4500, // 45 seconds (4500 * 10ms)
    .num_events = 0, // Advertise until timeout
};

static struct bt_le_ext_adv *adv_set;

static bool is_advertising = false;

K_WORK_DEFINE(adv_work, adv_work_handler);
K_WORK_DELAYABLE_DEFINE(bt_off_work, bt_off_work_handler);
K_WORK_DELAYABLE_DEFINE(security_work, security_work_handler);

struct bt_conn *conn = NULL;

static const struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
        BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_SERVICE_UUID_VAL),
};


/* VL53L4CD variables */
static struct gpio_callback cb;

static uint8_t consec_detect = 0;
static int64_t carry_ms = 0;
static void counter_state_reset(void);

K_WORK_DELAYABLE_DEFINE(counter_work, counter_work_handler);

static const struct device *const i2c_dev_22 = DEVICE_DT_GET(DT_NODELABEL(i2c22));

K_WORK_DEFINE(vl53_stop_work, vl53_stop_work_handler);
K_WORK_DEFINE(vl53_start_work, vl53_start_work_handler);

/* SHT30 variables */
static const struct device *const sht30_dev = DEVICE_DT_GET(DT_NODELABEL(sht30));

static volatile bool ambient_paused = false;

static int32_t  last_temp_cdeg  = 0;   /* °C * 100 */
static uint32_t last_humid_cpct = 0;   /* %RH * 100 */


K_WORK_DELAYABLE_DEFINE(ambient_work, ambient_work_handler);


/* Generic sensing variables */

static K_MUTEX_DEFINE(i2c22_mutex);

K_WORK_DELAYABLE_DEFINE(i2c_check_work, i2c_check_work_handler);


/* GPIO variables */

static const struct gpio_dt_spec err_led = {
	.dt_flags = GPIO_ACTIVE_HIGH,
	.pin = 5, // set to 5 for prod PCB, 10 or 14 for devkit
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
};

static const struct gpio_dt_spec GPIO1_spec = {
	.dt_flags = GPIO_ACTIVE_LOW,
	.pin = 8,
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio1))
};

static const struct gpio_dt_spec XSHUT_spec = {
	.dt_flags = GPIO_ACTIVE_LOW,
	.pin = 7,
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio1))
};


/* Logging variables */
static int64_t last_detection_time = 0;

static int64_t last_event_unix = 0;
static int64_t delta_unix_ms = 0;
static uint32_t delta_unix_sec = 0;

static volatile bool logging_paused = false;

LOG_MODULE_REGISTER(azss, LOG_LEVEL_DBG);


/* FUNCTIONS */



/* Function to request a connection interval */
static int request_conn_interval_ms(struct bt_conn *conn, uint16_t min_ms, uint16_t max_ms, uint16_t latency, uint16_t timeout_ms)
{
    int err;
    struct bt_le_conn_param param;

    if (!conn) {
        return -ENOTCONN;
    }

    // Convert ms to 1.25ms units
    param.interval_min = (min_ms * 4) / 5;
    param.interval_max = (max_ms * 4) / 5;
    
    param.latency = latency;
    
    param.timeout = timeout_ms / 10;

    err = bt_conn_le_param_update(conn, &param);

    return err;
}


/* Advertising work handler */
static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (is_advertising) {
		return;
	}

	if (adv_set == NULL) {
		LOG_ERR("adv_set is NULL");
		error_fatal(FATAL_BT_INIT);
	}

	int err = bt_le_ext_adv_start(adv_set, &start_param);
	if (err) {
		LOG_ERR("bt_le_ext_adv_start failed: %d", err);
		error_fatal(FATAL_BT_INIT);
	}

	is_advertising = true;
}

/* Security work handler */
static void security_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!conn) {
		LOG_DBG("no conn");
		return;
	}

	int err = bt_conn_set_security(conn, BT_SECURITY_L2);
	LOG_DBG("bt_conn_set_security returned %d", err);
}

/* Raw value to useful data conversions*/

/*uint16_t dummy_raw_data(void){
        static uint16_t i = 0;
        i++;
        i = (uint16_t)(i ? i : 1);
        i ^= (uint16_t)(i << 7);
        i ^= (uint16_t)(i >> 9);
        i ^= (uint16_t)(i << 8);
        return i;
}*/

/* Error throwing PMs */

static void pm_i2c22_get_or_fatal(void)
{
	int err = pm_device_runtime_get(i2c_dev_22);
	if (err) {
		LOG_ERR("pm_device_runtime_get failed: %d", err);
		error_fatal(FATAL_VL53_INIT);
	}

	k_usleep(100);
}

static void pm_i2c22_put_or_fatal(void)
{
	int err = pm_device_runtime_put(i2c_dev_22);
	if (err) {
		LOG_ERR("pm_device_runtime_put failed: %d", err);
		error_fatal(FATAL_VL53_INIT);
	}
}

static void ambient_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (ambient_paused) {
        return;
    }

    struct sensor_value temp, humidity;

    k_mutex_lock(&i2c22_mutex, K_FOREVER);
    pm_i2c22_get_or_fatal();

    int err = sensor_sample_fetch(sht30_dev);
    if (err) {
        LOG_ERR("SHT30 fetch failed: %d", err);
        pm_i2c22_put_or_fatal();
        k_mutex_unlock(&i2c22_mutex);
        goto reschedule;
    }

    sensor_channel_get(sht30_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
    sensor_channel_get(sht30_dev, SENSOR_CHAN_HUMIDITY, &humidity);

    pm_i2c22_put_or_fatal();
    k_mutex_unlock(&i2c22_mutex);

    last_temp_cdeg  = temp.val1 * 100 + temp.val2 / 10000;
    last_humid_cpct = humidity.val1 * 100 + humidity.val2 / 10000;

    LOG_DBG("SHT30: %d.%02d C  %u.%02u %%RH",
            temp.val1, temp.val2 / 10000,
            humidity.val1, humidity.val2 / 10000);

    uint8_t sample[2];
    sample[0] = (int8_t)CLAMP(temp.val1,     -128, 127);
    sample[1] = (uint8_t)CLAMP(humidity.val1,     0, 100);

    err = rb_ambient_put(sample, sizeof(sample));
    if (err == -ENOSPC) {
        LOG_WRN("Ambient ring buffer full, dropping sample");
    } else if (err) {
        LOG_ERR("rb_ambient_put failed: %d", err);
    }

reschedule:
    k_work_schedule(&ambient_work, K_MSEC(AMBIENT_PERIOD_MS));
}

/* Callback functions for sense service */
static uint32_t main_count_cb(void){
        return rb_stamp_count();
}

static int32_t main_temp_cb(void)
{
    return (int32_t)(int8_t)CLAMP(last_temp_cdeg / 100, -128, 127);
}

static uint32_t main_humid_cb(void)
{
    return (uint32_t)CLAMP(last_humid_cpct / 100, 0U, 100U);
}

static uint8_t main_cmnd_cb(uint8_t command){
        switch (command){
                case CMND_CONN_INTERVAL_20ms:
                        return request_conn_interval_ms(conn, 15, 25, 0, 16000) ? 1 : 0;
                
                case CMND_CONN_INTERVAL_100ms:
                        return request_conn_interval_ms(conn, 90, 110, 0, 16000) ? 1 : 0;

                case CMND_CONN_INTERVAL_512ms:
                        return request_conn_interval_ms(conn, 500, 524, 0, 16000) ? 1 : 0;

                case CMND_CONN_INTERVAL_1024ms:
                        return request_conn_interval_ms(conn, 1000, 1024, 0, 30000) ? 1 : 0;

                case CMND_STREAM_START:
                        bulk_stream_start();
                        return 0;

                case CMND_STREAM_STOP:
                        bulk_stream_stop();
                        return 0;

                case CMND_FW_VER:
                        return FW_VERSION;

                default:
                        return 1;
                }
}

static struct sense_cb sense_callbacks = {
.count_cb = main_count_cb,
.temp_cb = main_temp_cb,
.humid_cb = main_humid_cb,
.cmnd_cb = main_cmnd_cb
};


/* Callback after advertising timeout*/
static void adv_sent_cb(struct bt_le_ext_adv *adv, struct bt_le_ext_adv_sent_info *info)
{
    ARG_UNUSED(adv);
    ARG_UNUSED(info);
    
    is_advertising = false;
    k_work_schedule(&bt_off_work, K_MSEC(100));
}

static const struct bt_le_ext_adv_cb adv_callbacks = {
    .sent = adv_sent_cb,
};


/* Advanced advertising intialisation */
static int advertising_set_init(void)
{
    int err;

    err = bt_le_ext_adv_create(BT_LE_ADV_CONN_FAST_2, &adv_callbacks, &adv_set);
    if (err) {
        LOG_ERR("Failed to create advertising set (err %d)", err);
        return err;
    }

    err = bt_le_ext_adv_set_data(adv_set, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("Failed to set advertising data (err %d)", err);
        return err;
    }

    return 0;
}


/* Bluetooth initialisation */
static int bt_init_single(void)
{
	int err;

	if (bt_ready) {
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		error_fatal(FATAL_BT_INIT);
	}

	err = sense_service_init(&sense_callbacks);
	if (err) {
		LOG_ERR("sense_service_init failed: %d", err);
		error_fatal(FATAL_BT_INIT);
	}

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		err = settings_load();
		if (err) {
			LOG_ERR("settings_load failed: %d", err);
			error_fatal(FATAL_BT_INIT);
		}
	}

	err = advertising_set_init();
	if (err) {
		LOG_ERR("advertising_set_init failed: %d", err);
		error_fatal(FATAL_BT_INIT);
	}

	bt_ready = true;
	return 0;
}


/* Advertising start helper */
static void advertising_start(void)
{

        if (is_advertising || conn != NULL) {
                return;
        }

        k_work_cancel_delayable(&bt_off_work);

        (void)bt_init_single();

        k_work_submit(&adv_work);
}


/* Connection callbacks */
static void on_connected(struct bt_conn *conn_c, uint8_t err)
{
	if (err) {
		return;
	}

	k_work_cancel_delayable(&bt_off_work);

	struct bt_conn *newc = bt_conn_ref(conn_c);
	if (conn) {
		bt_conn_unref(conn);
	}

	conn = newc;

	is_advertising = false;

	int err_cfg = gpio_pin_interrupt_configure_dt(&GPIO1_spec, GPIO_PULL_UP | GPIO_INT_DISABLE);
	if (err_cfg) {
		LOG_ERR("Failed to disable GPIO interrupt (%d)", err_cfg);
		error_fatal(FATAL_GPIO_INIT);
	}

	k_work_cancel_delayable(&counter_work);
	k_work_cancel_delayable(&ambient_work);

	counter_state_reset();

	logging_paused = true;
	ambient_paused = true;

	k_work_submit(&vl53_stop_work);
	//k_work_reschedule(&security_work, K_MSEC(2000)); OBSOLETE - client handles security
}

static void on_disconnected(struct bt_conn *conn_dc, uint8_t reason)
{
	ARG_UNUSED(conn_dc);

	bulk_stream_stop();
	k_work_cancel_delayable(&security_work);

	if (conn) {
		bt_conn_unref(conn);
		conn = NULL;
	}

	LOG_INF("Disconnected, reason 0x%02X", reason);

	int err = gpio_pin_interrupt_configure_dt(&GPIO1_spec, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Failed to re-enable GPIO interrupt (%d)", err);
		error_fatal(FATAL_GPIO_INIT);
	}

	logging_paused = false;
	ambient_paused = false;

	k_work_submit(&vl53_start_work);
	k_work_reschedule(&ambient_work, K_NO_WAIT);
	k_work_schedule(&bt_off_work, K_MSEC(100));
}

/*static void recycled_cb(void)
{
        advertising_start();
}*/

static struct bt_conn_cb connection_callbacks = {
        .connected = on_connected,
        .disconnected = on_disconnected,
        //.recycled         = recycled_cb,
};


/* Bluetooth disable work handler */
static void bt_off_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (conn != NULL || is_advertising) {
        return;
    }

    if (adv_set) {
        (void)bt_le_ext_adv_stop(adv_set);
        //(void)bt_le_ext_adv_delete(adv_set);
        //adv_set = NULL;
    }

    //int err = bt_disable();
    //if (err) {
    //    LOG_WRN("bt_disable failed: %d", err);
    //    return;
    //}

    //bt_ready = false;     
    is_advertising = false;
    LOG_DBG("Advertising disabled");
}

/* GPIO1 interrupt callback */
static void GPIO1_cb(const struct device *dev,
            struct gpio_callback *cb,
            uint32_t pins){

        ARG_UNUSED(dev);
        ARG_UNUSED(cb);
        ARG_UNUSED(pins);


        k_work_schedule(&counter_work, K_NO_WAIT);
}

static int GPIO1_cb_init(void)
{
    int err = gpio_pin_configure_dt(&GPIO1_spec, GPIO_PULL_UP | GPIO_INPUT);
    //int err = gpio_pin_configure_dt(&GPIO1_spec, GPIO_INPUT);
    if(err){
        LOG_DBG("int1 input init failed (%d)",err);
        return -1;
    }

    LOG_DBG("Pin is %u",GPIO1_spec.pin);

    err = gpio_pin_interrupt_configure_dt(&GPIO1_spec, GPIO_INT_EDGE_TO_ACTIVE);
    if(err){
        LOG_DBG("int1 interrupt init failed (%d)",err);
        return -2;
    }

    LOG_DBG("Pin is %u",GPIO1_spec.pin);

    gpio_init_callback(&cb, GPIO1_cb,BIT(GPIO1_spec.pin));
    err = gpio_add_callback(GPIO1_spec.port,&cb);
    if(err){
        LOG_DBG("Failed to add int1 callback (%d)",err);
        return -3;
    }
    return 0;
}


/* VL53L4CD initialisation with default settings */
static void VL53L4CD_user_init(void)
{
    uint8_t status;
    uint8_t dev = 0;
    uint16_t sensor_id = 0;
    uint8_t fatal_code = FATAL_VL53_INIT;  /* default, overridden where needed */

    k_mutex_lock(&i2c22_mutex, K_FOREVER);
    pm_i2c22_get_or_fatal();

    status = VL53L4CD_ULP_GetSensorId(dev, &sensor_id);
    if (status || sensor_id != 0xEBAA) {
        LOG_ERR("VL53L4CD not detected");
        fatal_code = FATAL_VL53_ID;
        goto cleanup;
    }

    status = VL53L4CD_ULP_SensorInit(dev);
    if (status) {
        LOG_ERR("SensorInit failed: %u", status);
        goto cleanup;
    }

    status = VL53L4CD_ULP_SetInterruptConfiguration(dev, VL53_DISTANCE_THRESH_MM, 1);
    if (status) {
        LOG_ERR("SetInterruptConfiguration failed: %u", status);
        goto cleanup;
    }

    status = VL53L4CD_ULP_SetInterMeasurementInMs(dev, (int)DETECTION_INTERVAL);
    if (status) {
        LOG_ERR("SetInterMeasurementInMs failed: %u", status);
        goto cleanup;
    }

    status = VL53L4CD_ULP_SetMacroTiming(dev, 50);
    if (status) {
        LOG_ERR("SetMacroTiming failed: %u", status);
        goto cleanup;
    }

    status = VL53L4CD_ULP_SetSigmaThreshold(dev, 70);
    if (status) {
        LOG_ERR("SetSigmaThreshold failed: %u", status);
        goto cleanup;
    }

    status = VL53L4CD_ULP_SetSignalThreshold(dev, 600);
    if (status) {
        LOG_ERR("SetSignalThreshold failed: %u", status);
        goto cleanup;
    }

    status = VL53L4CD_ULP_SetROI(dev, 16); //Max ROI
    if (status) {
        LOG_ERR("SetROI failed: %u", status);
        goto cleanup;
    }

    status = VL53L4CD_ULP_StartRanging(dev);
    if (status) {
        LOG_ERR("StartRanging failed: %u", status);
        fatal_code = FATAL_VL53_START;
        goto cleanup;
    }


    pm_i2c22_put_or_fatal();
    k_mutex_unlock(&i2c22_mutex);
	return;

cleanup:
    pm_i2c22_put_or_fatal();
    k_mutex_unlock(&i2c22_mutex);
    error_fatal(fatal_code);
}


/* Counter work handler */
static void counter_work_handler(struct k_work *work)
{
	if (logging_paused) {
		return;
	}

	k_mutex_lock(&i2c22_mutex, K_FOREVER);
	pm_i2c22_get_or_fatal();

	int64_t uptime_now = k_uptime_get();
	int64_t diff_now = uptime_now - last_detection_time;

	uint8_t status = VL53L4CD_ULP_ERROR_NONE;
	for (int attempt = 1; attempt <= 3; attempt++) {
		status = VL53L4CD_ULP_ClearInterrupt((uint16_t)0);
		if (status == VL53L4CD_ULP_ERROR_NONE) {
			break;
		}
		LOG_ERR("ClearInterrupt failed attempt %d/3: %d", attempt, status);
	}
	if (status != VL53L4CD_ULP_ERROR_NONE) {
		pm_i2c22_put_or_fatal();
		k_mutex_unlock(&i2c22_mutex);
		error_fatal(FATAL_VL53_CLEAR_IRQ);
	}

	pm_i2c22_put_or_fatal();
	k_mutex_unlock(&i2c22_mutex);


	if (diff_now >= DETECTION_INTERVAL + DETECTION_INTERVAL/10 ) {
			int64_t timestamp_unix = get_current_timestamp();

			if (last_event_unix != 0){
					delta_unix_ms = timestamp_unix - last_event_unix;
					if (delta_unix_ms < 0) {
							delta_unix_ms = 0; 
					}
					consec_detect = 0;
			}
			else{
					delta_unix_ms = 0;
			}

			int64_t total_delta_ms = carry_ms + delta_unix_ms;

			int64_t total_delta_s = total_delta_ms / 1000;
			int32_t rem_delta_ms = (int32_t)(total_delta_ms - total_delta_s * 1000);
			
			if (total_delta_s > INT32_MAX) {
					delta_unix_sec = INT32_MAX;
					carry_ms = 999;
			}

			else {
					delta_unix_sec = (int32_t)total_delta_s;
					carry_ms = rem_delta_ms;
			}
							
			
			
			rb_stamp_put(delta_unix_sec);

			last_event_unix = timestamp_unix;
			LOG_DBG("count | %u", rb_stamp_count());
	}

	else if (consec_detect >= CONSEC_ADVERTISING_START / DETECTION_INTERVAL   
		&& !is_advertising
		&& conn == NULL) {
	
			advertising_start();

			consec_detect = 0;
	}

	else{
			consec_detect ++;
	}

	last_detection_time = uptime_now;
	

}

static void counter_state_reset(void)
{
    consec_detect = 0;
    carry_ms = 0;
}

static void vl53_stop_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&i2c22_mutex, K_FOREVER);
	pm_i2c22_get_or_fatal();
	uint8_t status = VL53L4CD_ULP_StopRanging(0);
	pm_i2c22_put_or_fatal();
	k_mutex_unlock(&i2c22_mutex);

	if (status) {
		LOG_ERR("VL53L4CD stop failed: %u", status);
		error_fatal(FATAL_VL53_START);
	}
}

static void vl53_start_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&i2c22_mutex, K_FOREVER);
	pm_i2c22_get_or_fatal();
	uint8_t status = VL53L4CD_ULP_StartRanging(0);
	pm_i2c22_put_or_fatal();
	k_mutex_unlock(&i2c22_mutex);

	if (status) {
		LOG_ERR("VL53L4CD start failed: %u", status);
		error_fatal(FATAL_VL53_START);
	}
}

static void i2c_check_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&i2c22_mutex, K_FOREVER);
	pm_i2c22_get_or_fatal();
	uint16_t sensor_id = 0;

	uint8_t status = VL53L4CD_ULP_GetSensorId((uint8_t)0, &sensor_id);
	if (status || (sensor_id != 0xEBAA)) {
		LOG_ERR("VL53L4CD not detected");
		pm_i2c22_put_or_fatal();
		k_mutex_unlock(&i2c22_mutex);
		error_fatal(FATAL_VL53_ID);
	}

	pm_i2c22_put_or_fatal();
	k_mutex_unlock(&i2c22_mutex);

	k_work_reschedule(&i2c_check_work, K_MINUTES(I2C_CHECK_PERIOD_MINS));

}

static void i2c22_scan(void)
{
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        int err = i2c_write(i2c_dev_22, NULL, 0, addr);
        if (err == 0) {
            LOG_INF("I2C ACK at 0x%02X", addr);
        }
    }
}

/* Main thread*/
int main(void)
{
	if (!device_is_ready(err_led.port)) {
		for (;;) { }
	}

	if (!device_is_ready(GPIO1_spec.port)) {
		for (;;) { }
	}

	if (!device_is_ready(i2c_dev_22)) {
		for (;;) { }
	}


	int err = error_service_init(&err_led);
	if (err) {
		for (;;) { }
	}

	err = gpio_pin_configure_dt(&XSHUT_spec, GPIO_PULL_UP | GPIO_OUTPUT_INACTIVE);
	if (err) {
		for (;;) { }
	}

	gpio_pin_set_dt(&XSHUT_spec, 0);

	err = pm_device_runtime_enable(i2c_dev_22);
	if (err) {
		error_fatal(FATAL_VL53_INIT);
	}

	bulk_stream_init();

    VL53L4CD_user_init();

	if (GPIO1_cb_init()) {
		error_fatal(FATAL_GPIO_INIT);
	}

	err = bt_conn_cb_register(&connection_callbacks);
	if (err) {
		error_fatal(FATAL_BT_INIT);
	}


	k_work_schedule(&ambient_work, K_NO_WAIT);

	k_work_schedule(&i2c_check_work, K_MINUTES(20));

    LOG_DBG("Successful init");

	k_sleep(K_SECONDS(30));

	i2c22_scan();

	k_sleep(K_FOREVER);
}