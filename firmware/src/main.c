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
#define FW_VERSION 10 //Major release 1, minor release 0


/* BLE definitions */
#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define SECURITY_DELAY_MS (1 * 1000)

/* Sensor definitions */
#define DETECTION_INTERVAL 240 // in ms (must end in 0, eg 80, 120, 500, NOT 143)
#define CONSEC_ADVERTISING_START 9600 // in ms (must be a multiple of DETECTION_INTERVAL)

#define VL53_DISTANCE_THRESH_MM 910

#define AMBIENT_PERIOD_MS (60 * 15 * 1000)


#define I2C_CHECK_PERIOD_MINS 30

/* Schedule definitions */
#define SCHEDULE_MAX_SLEEP_MS (3600 * 1000/2)

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

// Scheduling commands
#define CMND_SCHEDULE_8_16          0x30
#define CMND_SCHEDULE_8_18          0x31
#define CMND_SCHEDULE_9_21          0x32
#define CMND_SCHEDULE_DISABLED      0x33
#define CMND_SCHEDULE_WKND_DISABLE  0x34
#define CMND_SCHEDULE_WKND_ENABLE   0x35

static void adv_work_handler(struct k_work *work);
static void bt_off_work_handler(struct k_work *work);
static void security_work_handler(struct k_work *work);
static void counter_work_handler(struct k_work *work);
static void vl53_stop_work_handler(struct k_work *work);
static void ambient_work_handler(struct k_work *work);
static void i2c_check_work_handler(struct k_work *work);

/* BLE variables */

static struct bt_le_ext_adv_start_param start_param = {
    .timeout = 4500, // 45 seconds (4500 * 10ms)
    .num_events = 0, // Advertise until timeout
};

static struct bt_le_ext_adv *adv_set;

static atomic_t is_advertising = ATOMIC_INIT(0);

K_WORK_DEFINE(adv_work, adv_work_handler);
K_WORK_DELAYABLE_DEFINE(bt_off_work, bt_off_work_handler);
K_WORK_DELAYABLE_DEFINE(security_work, security_work_handler);

K_MUTEX_DEFINE(conn_mutex);
struct bt_conn *conn = NULL;

static const struct bt_data ad[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
        BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_SERVICE_UUID_VAL),
};

static struct interval_data {
    uint16_t min_ms;
    uint16_t max_ms;
    uint16_t latency;
    uint16_t timeout_ms;
} conn_intervals[] = {
    [CMND_CONN_INTERVAL_20ms] = { 15, 25, 0, 16000 },
    [CMND_CONN_INTERVAL_100ms] = { 90, 110, 0, 16000 },
    [CMND_CONN_INTERVAL_512ms] = { 500, 524, 0, 16000 },
    [CMND_CONN_INTERVAL_1024ms] = { 1000, 1024, 0, 30000 },
};


/* VL53L4CD variables */
static struct gpio_callback cb;

static uint8_t consec_detect = 0;
static int64_t carry_ms = 0;
static void counter_state_reset(void);

K_WORK_DELAYABLE_DEFINE(counter_work, counter_work_handler);
static struct k_work_sync counter_work_sync;

static const struct device *const i2c_dev_22 = DEVICE_DT_GET(DT_NODELABEL(i2c22));

K_WORK_DEFINE(vl53_stop_work, vl53_stop_work_handler);

/* SHT30 variables */
static const struct device *const sht30_dev = DEVICE_DT_GET(DT_NODELABEL(sht30));

static atomic_t ambient_paused = ATOMIC_INIT(0);

static volatile int32_t  last_temp_cdeg  = 0;   /* °C * 100 */
static volatile uint32_t last_humid_cpct = 0;   /* %RH * 100 */


K_WORK_DELAYABLE_DEFINE(ambient_work, ambient_work_handler);
static struct k_work_sync ambient_work_sync;


/* Generic sensing variables */

static K_MUTEX_DEFINE(i2c22_mutex);

K_WORK_DELAYABLE_DEFINE(i2c_check_work, i2c_check_work_handler);

K_THREAD_STACK_DEFINE(sensor_wq_stack, 2048);
static struct k_work_q sensor_wq;


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

static atomic_t logging_paused = ATOMIC_INIT(0);

LOG_MODULE_REGISTER(azss, LOG_LEVEL_DBG);

/* Scheduling variables */

typedef enum {
    SCHEDULE_8_16 = 0,   /* 08:00–16:00 */
    SCHEDULE_8_18 = 1,   /* 08:00–18:00 */
    SCHEDULE_9_21 = 2,   /* 09:00–21:00 */
    SCHEDULE_DISABLED = 3, /* Disabled */
} schedule_preset_t;

static const struct {
    uint8_t start_h;
    uint8_t end_h;
} schedule_hours[] = {
    [SCHEDULE_8_16] = { 8,  16 },
    [SCHEDULE_8_18] = { 8,  18 },
    [SCHEDULE_9_21] = { 9,  21 },
	[SCHEDULE_DISABLED] = { 0,  24 },
};


static void schedule_work_handler(struct k_work *work);
static void vl53_sched_reinit_work_handler(struct k_work *work);

K_WORK_DELAYABLE_DEFINE(schedule_work,          schedule_work_handler);
K_WORK_DELAYABLE_DEFINE(vl53_sched_reinit_work, vl53_sched_reinit_work_handler);

struct schedule_state {
    schedule_preset_t active_schedule;
    bool weekends_disabled;
    bool vl53_sched_off;
};

static struct schedule_state sched = {
    .active_schedule = SCHEDULE_DISABLED,
    .weekends_disabled = false,
    .vl53_sched_off = false,
};

K_MUTEX_DEFINE(schedule_mutex);

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

	if (atomic_get(&is_advertising)) {
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

	atomic_set(&is_advertising,  true);
}

/* Security work handler */
/* static void security_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

    int err = -1;

	if (!conn) {
		LOG_ERR("no conn");
		return;
	}

    struct bt_conn_info info;
    bt_conn_get_info(conn, &info);

	if (info.security.level < BT_SECURITY_L2) {
        err = bt_conn_set_security(conn, BT_SECURITY_L2);
        LOG_DBG("Manual security request: %d", err);
    } else {
        LOG_DBG("Security level sufficient: %d", info.security.level);
    }
	LOG_DBG("bt_conn_set_security returned %d", err);
} */


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

    if (atomic_get(&ambient_paused)) {
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
    k_work_schedule_for_queue(&sensor_wq, &ambient_work, K_MSEC(AMBIENT_PERIOD_MS));
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
    case CMND_CONN_INTERVAL_100ms:
    case CMND_CONN_INTERVAL_512ms:
    case CMND_CONN_INTERVAL_1024ms:{
        int ret = 1;
        k_mutex_lock(&conn_mutex, K_FOREVER);
        if(conn){
            struct bt_conn *conn_ref = bt_conn_ref(conn);
            k_mutex_unlock(&conn_mutex);
            struct interval_data data = conn_intervals[command];
            ret = request_conn_interval_ms(
                conn_ref,
                data.min_ms,
                data.max_ms,
                data.latency,
                data.timeout_ms
            ) == 0 ? 0 : 1;
            bt_conn_unref(conn_ref);
        }
        else{
            k_mutex_unlock(&conn_mutex);
        }
        return ret;

        }   

    case CMND_STREAM_START:
            bulk_stream_start();
            return 0;

    case CMND_STREAM_STOP:
            bulk_stream_stop();
            return 0;

    case CMND_FW_VER:
            return FW_VERSION;

    case CMND_SCHEDULE_8_16:
        k_mutex_lock(&schedule_mutex, K_FOREVER);
        sched.active_schedule = SCHEDULE_8_16;
        k_mutex_unlock(&schedule_mutex);
        k_work_reschedule(&schedule_work, K_NO_WAIT);
        return 0;

    case CMND_SCHEDULE_8_18:
        k_mutex_lock(&schedule_mutex, K_FOREVER);
        sched.active_schedule = SCHEDULE_8_18;
        k_mutex_unlock(&schedule_mutex);
        k_work_reschedule(&schedule_work, K_NO_WAIT);
        return 0;

    case CMND_SCHEDULE_9_21:
        k_mutex_lock(&schedule_mutex, K_FOREVER);
        sched.active_schedule = SCHEDULE_9_21;
        k_mutex_unlock(&schedule_mutex);
        k_work_reschedule(&schedule_work, K_NO_WAIT);
        return 0;

    case CMND_SCHEDULE_DISABLED:
        k_mutex_lock(&schedule_mutex, K_FOREVER);
        sched.active_schedule = SCHEDULE_DISABLED;
        k_mutex_unlock(&schedule_mutex);
        k_work_reschedule(&schedule_work, K_NO_WAIT);
        return 0;

    case CMND_SCHEDULE_WKND_DISABLE:
        k_mutex_lock(&schedule_mutex, K_FOREVER);
        sched.weekends_disabled = true;
        k_mutex_unlock(&schedule_mutex);
        k_work_reschedule(&schedule_work, K_NO_WAIT);
        return 0;

    case CMND_SCHEDULE_WKND_ENABLE:
        k_mutex_lock(&schedule_mutex, K_FOREVER);
        sched.weekends_disabled = false;
        k_mutex_unlock(&schedule_mutex);
        k_work_reschedule(&schedule_work, K_NO_WAIT);
        return 0;

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
    
    atomic_set(&is_advertising,  false);
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



static bool is_ble_connected(void)
{
    k_mutex_lock(&conn_mutex, K_FOREVER);
    bool connected = (conn != NULL);
    k_mutex_unlock(&conn_mutex);
    return connected;
}

/* Advertising start helper */
static void advertising_start(void)
{

        if (atomic_get(&is_advertising) || is_ble_connected()) {
                return;
        }

        k_work_cancel_delayable(&bt_off_work);

        k_work_submit(&adv_work);
}

/* Connection callbacks */
static void on_connected(struct bt_conn *conn_c, uint8_t err)
{
	if (err) {
		return;
	}

	k_work_cancel_delayable(&bt_off_work);

    k_mutex_lock(&conn_mutex, K_FOREVER);
	
	if (conn) {
		bt_conn_unref(conn);
	}
	conn = bt_conn_ref(conn_c);
	
	k_mutex_unlock(&conn_mutex);
	atomic_set(&is_advertising,  false);

	int err_cfg = gpio_pin_interrupt_configure_dt(&GPIO1_spec, GPIO_INT_DISABLE);
	if (err_cfg) {
		LOG_ERR("Failed to disable GPIO interrupt (%d)", err_cfg);
		error_fatal(FATAL_GPIO_INIT);
	}

	k_work_cancel_delayable_sync(&counter_work, &counter_work_sync);
	k_work_cancel_delayable_sync(&ambient_work, &ambient_work_sync);

	counter_state_reset();

	atomic_set(&logging_paused,  true);
	atomic_set(&ambient_paused,  true);

	if (!sched.vl53_sched_off) {
		k_work_submit_to_queue(&sensor_wq, &vl53_stop_work);
	}

	/**_work_reschedule(&security_work, K_MSEC(SECURITY_DELAY_MS)); Deprecated*/
}

static void on_disconnected(struct bt_conn *conn_dc, uint8_t reason)
{
	ARG_UNUSED(conn_dc);

	bulk_stream_stop();
	k_work_cancel_delayable(&security_work);

    k_mutex_lock(&conn_mutex, K_FOREVER);
	if (conn) {
		bt_conn_unref(conn);
		conn = NULL;
	}
	k_mutex_unlock(&conn_mutex);

	LOG_DBG("Disconnected, reason 0x%02X", reason);

	if (!sched.vl53_sched_off){
		atomic_set(&logging_paused,  false);
		k_work_reschedule_for_queue(&sensor_wq, &vl53_sched_reinit_work, K_MSEC(10));
	}
	atomic_set(&ambient_paused,  false);
	k_work_reschedule_for_queue(&sensor_wq, &ambient_work, K_NO_WAIT);
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

    if (is_ble_connected() || atomic_get(&is_advertising)) {
        return;
    }

    if (adv_set) {
        (void)bt_le_ext_adv_stop(adv_set);
    }
   
    atomic_set(&is_advertising,  false);
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
        LOG_ERR("GPIO1 input init failed (%d)",err);
        return -1;
    }

    LOG_DBG("Pin is %u",GPIO1_spec.pin);

    err = gpio_pin_interrupt_configure_dt(&GPIO1_spec, GPIO_INT_EDGE_TO_ACTIVE);
    if(err){
        LOG_ERR("GPIO1 interrupt init failed (%d)",err);
        return -2;
    }

    LOG_DBG("Pin is %u",GPIO1_spec.pin);

    gpio_init_callback(&cb, GPIO1_cb,BIT(GPIO1_spec.pin));
    err = gpio_add_callback(GPIO1_spec.port,&cb);
    if(err){
        LOG_ERR("Failed to add GPIO1 callback (%d)",err);
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
	if (atomic_get(&logging_paused)) {
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
		&& !atomic_get(&is_advertising)
		&& !is_ble_connected()) {
	
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


static void i2c_check_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (sched.vl53_sched_off) {
        k_work_reschedule_for_queue(&sensor_wq, &i2c_check_work, K_MINUTES(I2C_CHECK_PERIOD_MINS));
        return;
    }

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

	k_work_reschedule_for_queue(&sensor_wq, &i2c_check_work, K_MINUTES(I2C_CHECK_PERIOD_MINS));

}

/*static void i2c22_scan(void)
{
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        int err = i2c_write(i2c_dev_22, NULL, 0, addr);
        if (err == 0) {
            LOG_DBG("I2C ACK at 0x%02X", addr);
        }
    }
}*/


static bool is_business_hours(void)
{
    if (!is_time_set()) {
        return true;
    }

    schedule_preset_t active;
    bool weekend_active;

    k_mutex_lock(&schedule_mutex, K_FOREVER);
    active   = sched.active_schedule;
    weekend_active = sched.weekends_disabled;
    k_mutex_unlock(&schedule_mutex);

    int64_t unix_sec = get_current_timestamp() / 1000;

    if (weekend_active) {
        /* Unix epoch (Jan 1 1970) was a Thursday = day 4.
         * 0 = Sunday, 6 = Saturday.                        */
        uint8_t dow = (uint8_t)(((unix_sec / 86400) + 4) % 7);
        if (dow == 0 || dow == 6) {
            return false;
        }
    }

    uint32_t sec_in_day = (uint32_t)(unix_sec % 86400);
    uint8_t  hour       = (uint8_t)(sec_in_day / 3600);

    return (hour >= schedule_hours[active].start_h &&
            hour <  schedule_hours[active].end_h);
}

static int64_t ms_until_next_transition(void)
{
    if (!is_time_set()) {
        return 60 * 1000;
    }

    schedule_preset_t active;

    k_mutex_lock(&schedule_mutex, K_FOREVER);
    active = sched.active_schedule;
    k_mutex_unlock(&schedule_mutex);

    int64_t unix_sec   = get_current_timestamp() / 1000;
    int64_t sec_in_day = unix_sec % 86400;

    int64_t start_sec = (int64_t)schedule_hours[active].start_h * 3600;
    int64_t end_sec   = (int64_t)schedule_hours[active].end_h   * 3600;

    int64_t day_base = unix_sec - sec_in_day;
    int64_t next_sec;

    if (sec_in_day < start_sec) {
        next_sec = day_base + start_sec;
    } else if (sec_in_day < end_sec) {
        next_sec = day_base + end_sec;
    } else {
        next_sec = day_base + 86400 + start_sec;
    }

    int64_t delay_ms = (next_sec - unix_sec) * 1000;
    return MAX(delay_ms, 1000);
}

static void vl53_sched_reinit_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    /* Re-initialise fully — XSHUT cycle resets all sensor registers. */
    VL53L4CD_user_init();

	int err = gpio_pin_interrupt_configure_dt(&GPIO1_spec,
												GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Failed to re-enable GPIO interrupt (%d)", err);
		error_fatal(FATAL_GPIO_INIT);
	}

    LOG_DBG("VL53L4CD re-initialised");
}

static void schedule_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    bool should_be_active = is_business_hours();
    bool currently_off;
    schedule_preset_t active;

    k_mutex_lock(&schedule_mutex, K_FOREVER);
    currently_off = sched.vl53_sched_off;
    active = sched.active_schedule;

    if (should_be_active && currently_off) {
        sched.vl53_sched_off = false;
        k_mutex_unlock(&schedule_mutex);

        gpio_pin_set_dt(&XSHUT_spec, 0);

        if (!is_ble_connected()) {
            atomic_set(&logging_paused, false);
            k_work_reschedule_for_queue(&sensor_wq, &vl53_sched_reinit_work, K_MSEC(10));
            k_work_reschedule_for_queue(&sensor_wq, &ambient_work, K_NO_WAIT);
        }

        LOG_DBG("Entering business hours (%02d:00–%02d:00)",
                schedule_hours[active].start_h,
                schedule_hours[active].end_h);

    } else if (!should_be_active && !currently_off) {
        sched.vl53_sched_off = true;
        k_mutex_unlock(&schedule_mutex);

        atomic_set(&logging_paused, true);

        k_work_cancel_delayable(&counter_work);

        if (!is_ble_connected()) {
            int err = gpio_pin_interrupt_configure_dt(&GPIO1_spec,
                                                      GPIO_INT_DISABLE);
            if (err) {
                LOG_ERR("Failed to disable GPIO interrupt (%d)", err);
                error_fatal(FATAL_GPIO_INIT);
            }
        }

        gpio_pin_set_dt(&XSHUT_spec, 1);

        LOG_DBG("Outside business hours, VL53L4CD powered down");

    } else {
        k_mutex_unlock(&schedule_mutex);
    }

    int64_t delay_ms = MIN(ms_until_next_transition(), SCHEDULE_MAX_SLEEP_MS);
    k_work_reschedule(&schedule_work, K_MSEC(delay_ms));
}

static void time_changed_cb(void){
	k_work_reschedule(&schedule_work, K_NO_WAIT);
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

	err = time_service_init(time_changed_cb);
	if (err) {
		error_fatal(FATAL_GPIO_INIT);
	}

	err = bt_conn_cb_register(&connection_callbacks);
	if (err) {
		error_fatal(FATAL_BT_INIT);
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

    err = gpio_pin_configure_dt(&XSHUT_spec, GPIO_PULL_UP | GPIO_OUTPUT_INACTIVE);
	if (err) {
        error_fatal(FATAL_VL53_INIT);
	}

	gpio_pin_set_dt(&XSHUT_spec, 0);

	err = pm_device_runtime_enable(i2c_dev_22);
	if (err) {
		error_fatal(FATAL_VL53_INIT);
	}

    VL53L4CD_user_init();

	if (GPIO1_cb_init()) {
		error_fatal(FATAL_GPIO_INIT);
	}

    bulk_stream_init();

    k_work_queue_init(&sensor_wq);
    k_work_queue_start(&sensor_wq, sensor_wq_stack,
                    K_THREAD_STACK_SIZEOF(sensor_wq_stack),
                    K_PRIO_PREEMPT(10), NULL);

    k_work_schedule(&schedule_work, K_NO_WAIT);

	k_work_schedule_for_queue(&sensor_wq, &ambient_work, K_MINUTES(1));

	k_work_schedule_for_queue(&sensor_wq, &i2c_check_work, K_MINUTES(20));

    LOG_DBG("Successful init");

	k_sleep(K_FOREVER);
}
