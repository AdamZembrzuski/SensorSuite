/*
 * main.c — AZSensorSuite firmware, main entry and lifecycle
 *
 * Copyright (c) 2026 Adam Zembrzuski
 * SPDX-License-Identifier: TAPR-OHL-1.0
 */

/**
 * @file main.c
 * @brief Firmware entry point, lifecycle management, and sensor orchestration
 *        for AZSensorSuite.
 *
 * Owns the BLE advertising state machine (extended advertising, connection
 * interval negotiation, bonding), the VL53L4CD interrupt-driven detection
 * pipeline (consecutive-event filtering, delta-timestamp logging, XSHUT
 * cycling), and the business-hours schedule that gates sensor power. Also
 * manages the SHT30 ambient polling path, ZMS-backed state persistence, and
 * the multi-bit software watchdog that guards the sensor and system work 
 * queues.
 */

#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/util.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>


#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/gap.h>

#include "sense_service.h"
#include "time_service.h"
#include "log_service.h"
#include "error_service.h"
#include "VL53L4CD_ULP_api.h"

/* ════════════════════════════════════════════════════════════════
 * Definitions
 * ════════════════════════════════════════════════════════════════ */

#define FW_VERSION               CONFIG_APP_FW_VERSION

/* BLE advertising — timeout values are in units of 10ms per Zephyr API */
#define DEVICE_NAME              CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN          (sizeof(DEVICE_NAME) - 1)
#define ADV_TIMEOUT              (CONFIG_APP_ADVERTISING_TIMEOUT_MS / 10)
#define INITIAL_ADV_TIMEOUT      (CONFIG_APP_INITIAL_ADVERTISING_TIMEOUT_MS / 10)
#define INTERVAL_UPDATE_DELAY_MS 300

/* VL53L4CD */
#define DETECTION_INTERVAL       CONFIG_APP_VL53_DETECTION_INTERVAL_MS
#define CONSEC_ADVERTISING_START CONFIG_APP_CONSEC_ADVERTISING_START_MS
#define VL53_DISTANCE_THRESH_MM  CONFIG_APP_VL53_DISTANCE_THRESH_MM
#define VL53_WATCHDOG_PERIOD_MS  CONFIG_APP_VL53_WATCHDOG_PERIOD_MS
#define VL53L4CD_SENSOR_ID       0xEBAA

/* SHT30 */
#define AMBIENT_PERIOD_MS        CONFIG_APP_SHT_READ_INTERVAL_MS

/* Schedule */
#define SCHEDULE_MAX_SLEEP_MS    CONFIG_APP_SCHEDULE_MAX_SLEEP_MS
#define SETTINGS_SCHED_KEY       "azss/sched"

/* Command bytes written to the command characteristic */
#define CMND_CONN_INTERVAL_20ms     0x00
#define CMND_CONN_INTERVAL_100ms    0x01
#define CMND_CONN_INTERVAL_512ms    0x02
#define CMND_CONN_INTERVAL_1024ms   0x03
#define CMND_STREAM_START           0x10
#define CMND_STREAM_STOP            0x11
#define CMND_LOG_CLEAR              0x13
#define CMND_FW_VER                 0x20
#define CMND_SCHEDULE_8_16          0x30
#define CMND_SCHEDULE_8_18          0x31
#define CMND_SCHEDULE_9_21          0x32
#define CMND_SCHEDULE_DISABLED      0x33
#define CMND_SCHEDULE_WKND_DISABLE  0x34
#define CMND_SCHEDULE_WKND_ENABLE   0x35

/* Watchdog */
#define WDT_TIMEOUT_MS            CONFIG_APP_WDT_TIMEOUT_MS
#define WDT_FEED_INTERVAL_MS      CONFIG_APP_WDT_FEED_INTERVAL_MS
#define WDT_HEARTBEAT_INTERVAL_MS CONFIG_APP_WDT_HEARTBEAT_INTERVAL_MS

#define WDT_BIT_SENSOR  BIT(0)
#define WDT_ALL_BITS    WDT_BIT_SENSOR /* currently redundant, can add more subsystems later */

/* ════════════════════════════════════════════════════════════════
 * Types
 * ════════════════════════════════════════════════════════════════ */

typedef enum {
    SCHEDULE_8_16     = 0,
    SCHEDULE_8_18     = 1,
    SCHEDULE_9_21     = 2,
    SCHEDULE_DISABLED = 3,
} schedule_preset_t;

struct schedule_state {
    schedule_preset_t active_schedule;
    bool weekends_disabled;
    bool vl53_sched_off; /* true when sensor is powered down outside business hours */
};

/* ════════════════════════════════════════════════════════════════
 * Forward declarations
 * ════════════════════════════════════════════════════════════════ */

static void adv_work_handler(struct k_work *work);
static void adv_timeout_work_handler(struct k_work *work);
static void update_interval_work_handler(struct k_work *work);
static void counter_work_handler(struct k_work *work);
static void counter_state_reset(void);
static void vl53_stop_work_handler(struct k_work *work);
static void vl53_watchdog_work_handler(struct k_work *work);
static void vl53_sched_reinit_work_handler(struct k_work *work);
static void schedule_work_handler(struct k_work *work);
static int  save_schedule_settings(void);

#if IS_ENABLED(CONFIG_APP_SHT_ENABLE)
static void ambient_work_handler(struct k_work *work);
#endif

static void wdt_feed_work_handler(struct k_work *work);
static void wdt_heartbeat_sensor_work_handler(struct k_work *work);

/* ════════════════════════════════════════════════════════════════
 * State
 * ════════════════════════════════════════════════════════════════ */

LOG_MODULE_REGISTER(azss, LOG_LEVEL_DBG);

/* BLE */

K_MUTEX_DEFINE(conn_mutex);
struct bt_conn *conn = NULL; /* extern'd by log_service.c */

static struct bt_le_ext_adv_start_param start_param = {
    .timeout    = INITIAL_ADV_TIMEOUT,
    .num_events = 0,
};

static bool              adv_param_set = false; /* switches to ADV_TIMEOUT after first window */
static struct bt_le_ext_adv *adv_set;
static atomic_t          is_advertising = ATOMIC_INIT(0);
static bool              clear_bond_on_disconnect = false;
static uint8_t           pending_interval_cmnd;

K_WORK_DEFINE(adv_work, adv_work_handler);
K_WORK_DELAYABLE_DEFINE(adv_timeout_work, adv_timeout_work_handler);
K_WORK_DELAYABLE_DEFINE(update_interval_work, update_interval_work_handler);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_SERVICE_UUID_VAL),
};

/* Connection interval lookup, indexed by command byte */
static struct interval_data {
    uint16_t min_ms;
    uint16_t max_ms;
    uint16_t latency;
    uint16_t timeout_ms;
} conn_intervals[] = {
    [CMND_CONN_INTERVAL_20ms]   = {   15,   25, 0, 4096 },
    [CMND_CONN_INTERVAL_100ms]  = {   90,  110, 0, 4096 },
    [CMND_CONN_INTERVAL_512ms]  = {  500,  524, 0, 4096 },
    [CMND_CONN_INTERVAL_1024ms] = { 1000, 1024, 0, 8192 },
};

/* I2C */

static const struct device *const i2c_dev_22 = DEVICE_DT_GET(DT_NODELABEL(i2c22));
static K_MUTEX_DEFINE(i2c22_mutex);

/* VL53L4CD */

static struct gpio_callback vl53_int_gpio_cb;

static const struct gpio_dt_spec vl53_int_spec = {
    .dt_flags = GPIO_ACTIVE_LOW,
    .pin      = 8,
    .port     = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
};

static const struct gpio_dt_spec vl53_xshut_spec = {
    .dt_flags = GPIO_ACTIVE_LOW,
    .pin      = 7,
    .port     = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
};

/* consec_detect: counts detections within the advertising trigger window.
 * consec_valid:  counts independent crossing events for the detection filter. */
static uint8_t  consec_detect       = 0;
static uint8_t  consec_valid        = 0;
static int64_t  last_detection_time = 0;

/* carry_ms holds the sub-second remainder between logged events to avoid
 * accumulating rounding error across the delta timestamp chain. */
static int64_t  last_event_unix = 0;
static int64_t  delta_unix_ms   = 0;
static uint32_t delta_unix_sec  = 0;
static int64_t  carry_ms        = 0;
static atomic_t logging_paused  = ATOMIC_INIT(0);

/* xshut cycle causes a false sample */
static bool next_sample_xshut = false;

K_THREAD_STACK_DEFINE(sensor_wq_stack, CONFIG_APP_SENSOR_WQ_STACK_SIZE);
static struct k_work_q sensor_wq;

K_WORK_DELAYABLE_DEFINE(counter_work,           counter_work_handler);
K_WORK_DEFINE(vl53_stop_work,                   vl53_stop_work_handler);
K_WORK_DELAYABLE_DEFINE(vl53_watchdog_work,     vl53_watchdog_work_handler);
K_WORK_DELAYABLE_DEFINE(vl53_sched_reinit_work, vl53_sched_reinit_work_handler);

/* SHT30 */

#if IS_ENABLED(CONFIG_APP_SHT_ENABLE)
static const struct device *const sht30_dev = DEVICE_DT_GET(DT_NODELABEL(sht30));
static atomic_t          ambient_paused  = ATOMIC_INIT(0);
static volatile int32_t  last_temp_cdeg  = 0;  /* °C × 100 */
static volatile uint32_t last_humid_cpct = 0;  /* %RH × 100 */
K_WORK_DELAYABLE_DEFINE(ambient_work, ambient_work_handler);
#endif

/* Error LED */

static const struct gpio_dt_spec err_led = {
    .dt_flags = GPIO_ACTIVE_HIGH,
#if IS_ENABLED(CONFIG_APP_BOARD_IS_PROD)
    .pin  = 5,
#else
    .pin  = 10,
#endif
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
};

/* Schedule */

static const struct {
    uint8_t start_h;
    uint8_t end_h;
} schedule_hours[] = {
    [SCHEDULE_8_16]     = {  8, 16 },
    [SCHEDULE_8_18]     = {  8, 18 },
    [SCHEDULE_9_21]     = {  9, 21 },
    [SCHEDULE_DISABLED] = {  0, 24 },
};

/* weekends field: -1 = leave unchanged, 0 = enable weekends, 1 = disable weekends */
static const struct {
    schedule_preset_t preset;
    int8_t            weekends;
} schedule_cmnds[] = {
    [CMND_SCHEDULE_8_16]         = { SCHEDULE_8_16,       -1 },
    [CMND_SCHEDULE_8_18]         = { SCHEDULE_8_18,       -1 },
    [CMND_SCHEDULE_9_21]         = { SCHEDULE_9_21,       -1 },
    [CMND_SCHEDULE_DISABLED]     = { SCHEDULE_DISABLED,   -1 },
    [CMND_SCHEDULE_WKND_DISABLE] = { SCHEDULE_DISABLED,    1 },
    [CMND_SCHEDULE_WKND_ENABLE]  = { SCHEDULE_DISABLED,    0 },
};

static struct schedule_state sched = {
    .active_schedule   = SCHEDULE_DISABLED,
    .weekends_disabled = false,
    .vl53_sched_off    = false,
};

K_MUTEX_DEFINE(schedule_mutex);
K_WORK_DELAYABLE_DEFINE(schedule_work, schedule_work_handler);

/* watchdog */
const struct device *wdt_dev = DEVICE_DT_GET_OR_NULL(DT_ALIAS(wdt0));

static atomic_t wdt_bits      = ATOMIC_INIT(0);
static int      wdt_channel_id = -1;

K_WORK_DELAYABLE_DEFINE(wdt_feed_work,             wdt_feed_work_handler);
K_WORK_DELAYABLE_DEFINE(wdt_heartbeat_sensor_work, wdt_heartbeat_sensor_work_handler);

/* ════════════════════════════════════════════════════════════════
 * BLE — advertising
 * ════════════════════════════════════════════════════════════════ */

static bool is_ble_connected(void)
{
    k_mutex_lock(&conn_mutex, K_FOREVER);
    bool connected = (conn != NULL);
    k_mutex_unlock(&conn_mutex);
    return connected;
}

static void advertising_start(void)
{
    if (atomic_get(&is_advertising) || is_ble_connected()) {
        return;
    }
    k_work_cancel_delayable(&adv_timeout_work);
    k_work_submit(&adv_work);
}

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
    if (err == -ENOMEM || err == -EBUSY) {
        LOG_WRN("bt_le_ext_adv_start busy (%d), retrying", err);
        k_work_submit(&adv_work);
        return;
    } else if (err) {
        LOG_ERR("bt_le_ext_adv_start failed: %d", err);
        error_fatal(FATAL_BT_INIT);
    }

    atomic_set(&is_advertising, true);
    LOG_INF("Advertising enabled");
}

static void adv_sent_cb(struct bt_le_ext_adv *adv, struct bt_le_ext_adv_sent_info *info)
{
    ARG_UNUSED(adv);
    ARG_UNUSED(info);

    atomic_set(&is_advertising, false);
    /* Switch from the longer initial window to the normal timeout after first expiry */
    if (!adv_param_set) {
        start_param.timeout = ADV_TIMEOUT;
        adv_param_set = true;
    }
    k_work_schedule(&adv_timeout_work, K_MSEC(100));
}

static const struct bt_le_ext_adv_cb adv_callbacks = {
    .sent = adv_sent_cb,
};

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

static void adv_timeout_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (is_ble_connected() || atomic_get(&is_advertising)) {
        return;
    }

    if (adv_set) {
        (void)bt_le_ext_adv_stop(adv_set);
    }

    atomic_set(&is_advertising, false);
    LOG_DBG("Advertising disabled");
}

/* ════════════════════════════════════════════════════════════════
 * BLE — connection management
 * ════════════════════════════════════════════════════════════════ */

static int request_conn_interval_ms(struct bt_conn *conn, uint16_t min_ms,
                                    uint16_t max_ms, uint16_t latency,
                                    uint16_t timeout_ms)
{
    if (!conn) {
        return -ENOTCONN;
    }

    /* Zephyr connection interval units are 1.25ms; multiply by 4/5 to convert */
    struct bt_le_conn_param param = {
        .interval_min = (min_ms * 4) / 5,
        .interval_max = (max_ms * 4) / 5,
        .latency      = latency,
        .timeout      = timeout_ms / 10,
    };

    return bt_conn_le_param_update(conn, &param);
}

static void update_interval_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    k_mutex_lock(&conn_mutex, K_FOREVER);
    if (conn) {
        struct bt_conn *conn_ref = bt_conn_ref(conn);
        k_mutex_unlock(&conn_mutex);

        struct interval_data data = conn_intervals[pending_interval_cmnd];
        int err = request_conn_interval_ms(conn_ref,
                                           data.min_ms, data.max_ms,
                                           data.latency, data.timeout_ms);
        bt_conn_unref(conn_ref);
        if (err) {
            LOG_ERR("Failed to update connection interval: %d", err);
        }
    } else {
        k_mutex_unlock(&conn_mutex);
    }
}

static void on_connected(struct bt_conn *conn_c, uint8_t err)
{
    if (err) {
        return;
    }

    k_work_cancel_delayable(&adv_timeout_work);

    k_mutex_lock(&conn_mutex, K_FOREVER);
    if (conn) {
        bt_conn_unref(conn);
    }
    conn = bt_conn_ref(conn_c);
    k_mutex_unlock(&conn_mutex);

    atomic_set(&is_advertising, false);

    int err_cfg = gpio_pin_interrupt_configure_dt(&vl53_int_spec, GPIO_INT_DISABLE);
    if (err_cfg) {
        LOG_ERR("Failed to disable GPIO interrupt (%d)", err_cfg);
        error_fatal(FATAL_GPIO_INIT);
    }

    k_work_cancel_delayable(&counter_work);
    atomic_set(&logging_paused, true);

#if IS_ENABLED(CONFIG_APP_SHT_ENABLE)
    k_work_cancel_delayable(&ambient_work);
    atomic_set(&ambient_paused, true);
#endif

    counter_state_reset();

    if (!sched.vl53_sched_off) {
        k_work_submit_to_queue(&sensor_wq, &vl53_stop_work);
    }
}

static void on_disconnected(struct bt_conn *conn_dc, uint8_t reason)
{
    ARG_UNUSED(conn_dc);

    bulk_stream_stop();

    k_mutex_lock(&conn_mutex, K_FOREVER);
    if (conn) {
        bt_conn_unref(conn);
        conn = NULL;
    }
    k_mutex_unlock(&conn_mutex);

    if (clear_bond_on_disconnect) {
        clear_bond_on_disconnect = false;
        bt_unpair(BT_ID_DEFAULT, bt_conn_get_dst(conn_dc));
    }

    LOG_DBG("Disconnected, reason 0x%02X", reason);

    if (!sched.vl53_sched_off) {
        atomic_set(&logging_paused, false);
        k_work_reschedule_for_queue(&sensor_wq, &vl53_sched_reinit_work, K_MSEC(10));
    }

#if IS_ENABLED(CONFIG_APP_SHT_ENABLE)
    atomic_set(&ambient_paused, false);
    k_work_reschedule_for_queue(&sensor_wq, &ambient_work, K_NO_WAIT);
#endif

    k_work_schedule(&adv_timeout_work, K_MSEC(100));

    if (IS_ENABLED(CONFIG_APP_ADVERTISE_ON_DISCONNECT)) {
        advertising_start();
    }
}

static void security_changed(struct bt_conn *conn_s, bt_security_t level,
                             enum bt_security_err err)
{
    if (err == BT_SECURITY_ERR_PIN_OR_KEY_MISSING) {
        /* Stay connected — central will initiate fresh pairing */
        LOG_WRN("PIN/key missing — waiting for re-pair");
    } else if (err == BT_SECURITY_ERR_AUTH_FAIL) {
        LOG_WRN("Auth failed — clearing bond after disconnect");
        clear_bond_on_disconnect = true;
        bt_conn_disconnect(conn_s, BT_HCI_ERR_AUTH_FAIL);
    }
}

static void pairing_complete(struct bt_conn *conn_p, bool bonded)
{
    LOG_DBG("Pairing complete, bonded: %d", bonded);
}

static void pairing_failed(struct bt_conn *conn_p, enum bt_security_err reason)
{
    LOG_WRN("Pairing failed, reason: %d", reason);
}

static struct bt_conn_cb connection_callbacks = {
    .connected        = on_connected,
    .disconnected     = on_disconnected,
    .security_changed = security_changed,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete,
    .pairing_failed   = pairing_failed,
};

static struct bt_conn_auth_cb auth_cb = {
    /* All NULL = NoInputNoOutput / Level 2 */
};

/* ════════════════════════════════════════════════════════════════
 * I2C power management
 * ════════════════════════════════════════════════════════════════ */

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

/* ════════════════════════════════════════════════════════════════
 * VL53L4CD
 * ════════════════════════════════════════════════════════════════ */

static void vl53_int_cb(const struct device *dev,
                        struct gpio_callback *cb,
                        uint32_t pins)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    k_work_schedule(&counter_work, K_NO_WAIT);
}

static int vl53_int_init(void)
{
    /* Production PCB has an external pull-up; devkit does not */
#if IS_ENABLED(CONFIG_APP_BOARD_IS_PROD)
    int err = gpio_pin_configure_dt(&vl53_int_spec, GPIO_INPUT);
#else
    int err = gpio_pin_configure_dt(&vl53_int_spec, GPIO_INPUT | GPIO_PULL_UP);
#endif
    if (err) {
        LOG_ERR("vl53_int configure failed (%d)", err);
        return -1;
    }

    err = gpio_pin_interrupt_configure_dt(&vl53_int_spec, GPIO_INT_EDGE_TO_ACTIVE);
    if (err) {
        LOG_ERR("vl53_int interrupt configure failed (%d)", err);
        return -2;
    }

    gpio_init_callback(&vl53_int_gpio_cb, vl53_int_cb, BIT(vl53_int_spec.pin));
    err = gpio_add_callback(vl53_int_spec.port, &vl53_int_gpio_cb);
    if (err) {
        LOG_ERR("Failed to add vl53_int callback (%d)", err);
        return -3;
    }

    return 0;
}

static void VL53L4CD_user_init(void)
{
    uint8_t  status;
    uint8_t  dev        = 0;
    uint16_t sensor_id  = 0;
    uint8_t  fatal_code = FATAL_VL53_INIT;

    gpio_pin_set_dt(&vl53_xshut_spec, 1);
    k_msleep(10);
    gpio_pin_set_dt(&vl53_xshut_spec, 0);
    k_msleep(10);

    next_sample_xshut = true;

    k_mutex_lock(&i2c22_mutex, K_FOREVER);
    pm_i2c22_get_or_fatal();

    status = VL53L4CD_ULP_GetSensorId(dev, &sensor_id);
    if (status || sensor_id != VL53L4CD_SENSOR_ID) {
        LOG_ERR("VL53L4CD not detected");
        fatal_code = FATAL_VL53_INIT;
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

    status = VL53L4CD_ULP_SetROI(dev, 16);
    if (status) {
        LOG_ERR("SetROI failed: %u", status);
        goto cleanup;
    }

    status = VL53L4CD_ULP_StartRanging(dev);
    if (status) {
        LOG_ERR("StartRanging failed: %u", status);
        fatal_code = FATAL_VL53_INIT;
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
        error_fatal(FATAL_VL53_INIT);
    }
}

static void vl53_sched_reinit_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    VL53L4CD_user_init();

    int err = gpio_pin_interrupt_configure_dt(&vl53_int_spec, GPIO_INT_EDGE_TO_ACTIVE);
    if (err) {
        LOG_ERR("Failed to re-enable GPIO interrupt (%d)", err);
        error_fatal(FATAL_GPIO_INIT);
    }

    LOG_DBG("VL53L4CD re-initialised");
}

static void vl53_watchdog_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    if (atomic_get(&logging_paused) || sched.vl53_sched_off || is_ble_connected()) {
        goto reschedule;
    }

    int64_t since_last = k_uptime_get() - last_detection_time;
    if (since_last >= VL53_WATCHDOG_PERIOD_MS) {
        LOG_WRN("VL53 watchdog: no interrupt in %lld ms, reinitialising", since_last);
        k_work_reschedule_for_queue(&sensor_wq, &vl53_sched_reinit_work, K_NO_WAIT);
    }

    int ret = log_persist_state(last_event_unix, carry_ms, get_current_timestamp());
    if (ret) {
        LOG_WRN("Failed to persist log state: %d", ret);
    }

reschedule:
    k_work_reschedule_for_queue(&sensor_wq, &vl53_watchdog_work,
                                K_MSEC(VL53_WATCHDOG_PERIOD_MS));
}

static void counter_state_reset(void)
{
    consec_detect = 0;
    consec_valid  = 0;
    carry_ms      = 0;
}

static void counter_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    k_mutex_lock(&i2c22_mutex, K_FOREVER);
    pm_i2c22_get_or_fatal();

    int64_t uptime_now = k_uptime_get();
    int64_t diff_now   = uptime_now - last_detection_time;

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

    if (next_sample_xshut) {
        next_sample_xshut = false;
        return;
    }

    /* A gap wider than 1.1× the measurement period means this is a new
     * independent crossing rather than a bounce of the previous interrupt */
    if (diff_now >= DETECTION_INTERVAL + DETECTION_INTERVAL / 10) {
        int64_t timestamp_unix = get_current_timestamp();

        if (last_event_unix != 0) {
            delta_unix_ms = timestamp_unix - last_event_unix;
            if (delta_unix_ms < 0) {
                delta_unix_ms = 0;
            }
            consec_detect = 0;
        } else {
            delta_unix_ms = 0;
        }

        /* Accumulate sub-second remainder to avoid rounding error drift */
        int64_t total_delta_ms = carry_ms + delta_unix_ms;
        int64_t total_delta_s  = total_delta_ms / 1000;
        int32_t rem_delta_ms   = (int32_t)(total_delta_ms - total_delta_s * 1000);

        if (total_delta_s > INT32_MAX) {
            delta_unix_sec = INT32_MAX;
            carry_ms       = 999;
        } else {
            delta_unix_sec = (int32_t)total_delta_s;
            carry_ms       = rem_delta_ms;
        }

        consec_valid++;

#if IS_ENABLED(CONFIG_APP_CONSEC_DETECT_FILTER)
        if (consec_valid >= CONFIG_APP_CONSEC_DETECT_THRESHOLD) {
#endif
            rb_stamp_put(delta_unix_sec);
            if (delta_unix_ms > 60*1000){
                int ret = log_persist_state(last_event_unix, carry_ms, get_current_timestamp());
                if (ret) {
                    LOG_WRN("Failed to persist log state: %d", ret);
                }
            }
            LOG_DBG("count | %u", rb_stamp_count());
#if IS_ENABLED(CONFIG_APP_CONSEC_DETECT_FILTER)
        }
#endif

        last_event_unix = timestamp_unix;

    } else if (consec_detect >= CONSEC_ADVERTISING_START / DETECTION_INTERVAL
               && !atomic_get(&is_advertising)
               && !is_ble_connected()) {

        advertising_start();
        consec_detect = 0;
        consec_valid  = 0;

    } else {
        consec_detect++;
        consec_valid = 0;
    }

    last_detection_time = uptime_now;
}

/* ════════════════════════════════════════════════════════════════
 * SHT30
 * ════════════════════════════════════════════════════════════════ */

#if IS_ENABLED(CONFIG_APP_SHT_ENABLE)
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
    sample[0] = (int8_t)CLAMP(temp.val1,      -128, 127);
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
#endif

/* ════════════════════════════════════════════════════════════════
 * Sense service callbacks and command handler
 * ════════════════════════════════════════════════════════════════ */

static uint32_t main_count_cb(void)
{
    return log_total_stamp_count();
}

static int32_t main_temp_cb(void)
{
#if IS_ENABLED(CONFIG_APP_SHT_ENABLE)
    return (int32_t)(int8_t)CLAMP(last_temp_cdeg / 100, -128, 127);
#else
    return 0;
#endif
}

static uint32_t main_humid_cb(void)
{
#if IS_ENABLED(CONFIG_APP_SHT_ENABLE)
    return (uint32_t)CLAMP(last_humid_cpct / 100, 0U, 100U);
#else
    return 0;
#endif
}

static uint8_t main_cmnd_cb(uint8_t command)
{
    switch (command) {

    case CMND_CONN_INTERVAL_20ms:
    case CMND_CONN_INTERVAL_100ms:
    case CMND_CONN_INTERVAL_512ms:
    case CMND_CONN_INTERVAL_1024ms:
        if (bulk_stream_is_active()) {
            return 1;
        }
        pending_interval_cmnd = command;
        k_work_schedule(&update_interval_work, K_MSEC(INTERVAL_UPDATE_DELAY_MS));
        return 0;

    case CMND_STREAM_START:
        bulk_stream_start();
        return 0;

    case CMND_STREAM_STOP:
        bulk_stream_stop();
        return 0;

    case CMND_FW_VER:
        return FW_VERSION;

    case CMND_SCHEDULE_8_16:
    case CMND_SCHEDULE_8_18:
    case CMND_SCHEDULE_9_21:
    case CMND_SCHEDULE_DISABLED:
    case CMND_SCHEDULE_WKND_DISABLE:
    case CMND_SCHEDULE_WKND_ENABLE:
        if (command >= ARRAY_SIZE(schedule_cmnds)) {
            return 1;
        }
        k_mutex_lock(&schedule_mutex, K_FOREVER);
        if (schedule_cmnds[command].weekends == -1) {
            sched.active_schedule = schedule_cmnds[command].preset;
        } else {
            sched.weekends_disabled = (bool)schedule_cmnds[command].weekends;
        }
        k_mutex_unlock(&schedule_mutex);
        save_schedule_settings();
        k_work_reschedule(&schedule_work, K_NO_WAIT);
        return 0;

    case CMND_LOG_CLEAR:
        log_service_clear();
        return 0;

    default:
        return 1;
    }
}

static struct sense_cb sense_callbacks = {
    .count_cb = main_count_cb,
    .temp_cb  = main_temp_cb,
    .humid_cb = main_humid_cb,
    .cmnd_cb  = main_cmnd_cb,
};

/* ════════════════════════════════════════════════════════════════
 * Schedule and settings persistence
 * ════════════════════════════════════════════════════════════════ */

static int save_schedule_settings(void)
{
    struct {
        uint8_t active_schedule;
        bool    weekends_disabled;
    } s = {
        .active_schedule   = (uint8_t)sched.active_schedule,
        .weekends_disabled = sched.weekends_disabled,
    };
    return settings_save_one(SETTINGS_SCHED_KEY, &s, sizeof(s));
}

static int sched_settings_set(const char *key, size_t len,
                              settings_read_cb read_cb, void *cb_arg)
{
    ARG_UNUSED(key);

    struct {
        uint8_t active_schedule;
        bool    weekends_disabled;
    } s;

    if (len != sizeof(s)) {
        return -EINVAL;
    }

    if (read_cb(cb_arg, &s, sizeof(s)) != sizeof(s)) {
        return -EIO;
    }

    k_mutex_lock(&schedule_mutex, K_FOREVER);
    sched.active_schedule   = (schedule_preset_t)s.active_schedule;
    sched.weekends_disabled = s.weekends_disabled;
    k_mutex_unlock(&schedule_mutex);

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(azss_sched, "azss", NULL,
                                sched_settings_set, NULL, NULL);

static bool is_business_hours(void)
{
    schedule_preset_t active;
    bool weekend_active;

    k_mutex_lock(&schedule_mutex, K_FOREVER);
    active         = sched.active_schedule;
    weekend_active = sched.weekends_disabled;
    k_mutex_unlock(&schedule_mutex);

    int64_t unix_sec = get_current_timestamp() / 1000;

    if (weekend_active) {
        /* Unix epoch (Jan 1 1970) was a Thursday = day 4; 0 = Sunday, 6 = Saturday */
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
    schedule_preset_t active;

    k_mutex_lock(&schedule_mutex, K_FOREVER);
    active = sched.active_schedule;
    k_mutex_unlock(&schedule_mutex);

    int64_t unix_sec   = get_current_timestamp() / 1000;
    int64_t sec_in_day = unix_sec % 86400;
    int64_t start_sec  = (int64_t)schedule_hours[active].start_h * 3600;
    int64_t end_sec    = (int64_t)schedule_hours[active].end_h   * 3600;
    int64_t day_base   = unix_sec - sec_in_day;
    int64_t next_sec;

    if (sec_in_day < start_sec) {
        next_sec = day_base + start_sec;
    } else if (sec_in_day < end_sec) {
        next_sec = day_base + end_sec;
    } else {
        next_sec = day_base + 86400 + start_sec; /* tomorrow's start */
    }

    return MAX((next_sec - unix_sec) * 1000, 1000);
}

static void schedule_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    bool should_be_active = is_business_hours();
    bool currently_off;
    schedule_preset_t active;

    k_mutex_lock(&schedule_mutex, K_FOREVER);
    currently_off = sched.vl53_sched_off;
    active        = sched.active_schedule;

    if (should_be_active && currently_off) {
        sched.vl53_sched_off = false;
        k_mutex_unlock(&schedule_mutex);

        gpio_pin_set_dt(&vl53_xshut_spec, 0);

        if (!is_ble_connected()) {
            atomic_set(&logging_paused, false);
            k_work_reschedule_for_queue(&sensor_wq, &vl53_sched_reinit_work, K_MSEC(10));
#if IS_ENABLED(CONFIG_APP_SHT_ENABLE)
            k_work_reschedule_for_queue(&sensor_wq, &ambient_work, K_NO_WAIT);
#endif
        }

        LOG_DBG("Entering business hours (%02d:00-%02d:00)",
                schedule_hours[active].start_h,
                schedule_hours[active].end_h);

    } else if (!should_be_active && !currently_off) {
        sched.vl53_sched_off = true;
        k_mutex_unlock(&schedule_mutex);

        atomic_set(&logging_paused, true);
        k_work_cancel_delayable(&counter_work);

        if (!is_ble_connected()) {
            int err = gpio_pin_interrupt_configure_dt(&vl53_int_spec, GPIO_INT_DISABLE);
            if (err) {
                LOG_ERR("Failed to disable GPIO interrupt (%d)", err);
                error_fatal(FATAL_GPIO_INIT);
            }
        }

        gpio_pin_set_dt(&vl53_xshut_spec, 1);
        LOG_DBG("Outside business hours, VL53L4CD powered down");

    } else {
        k_mutex_unlock(&schedule_mutex);
    }

    int64_t delay_ms = MIN(ms_until_next_transition(), SCHEDULE_MAX_SLEEP_MS);
    k_work_reschedule(&schedule_work, K_MSEC(delay_ms));
}

static void time_changed_cb(void)
{
    k_work_reschedule(&schedule_work, K_NO_WAIT);
}

/* ════════════════════════════════════════════════════════════════
 * Watchdog
 * ════════════════════════════════════════════════════════════════ */

static void wdt_feed_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    atomic_val_t bits = atomic_and(&wdt_bits, 0);

    if (wdt_dev && device_is_ready(wdt_dev) && wdt_channel_id >= 0) {
        if ((bits & WDT_ALL_BITS) == WDT_ALL_BITS) {
            wdt_feed(wdt_dev, wdt_channel_id);
        } else {
            LOG_WRN("WDT feed skipped — missing bits: 0x%02x",
                    (uint8_t)(~bits & WDT_ALL_BITS));
        }
    }

    k_work_reschedule(&wdt_feed_work, K_MSEC(WDT_FEED_INTERVAL_MS));
}


static void wdt_heartbeat_sensor_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    atomic_or(&wdt_bits, WDT_BIT_SENSOR);
    k_work_reschedule_for_queue(&sensor_wq, &wdt_heartbeat_sensor_work,
                                K_MSEC(WDT_HEARTBEAT_INTERVAL_MS));
}

/* ════════════════════════════════════════════════════════════════
 * Main
 * ════════════════════════════════════════════════════════════════ */

int main(void)
{
    uint32_t reset_reason = NRF_RESET->RESETREAS;
    NRF_RESET->RESETREAS  = reset_reason; /* clear by writing back */
    LOG_INF("Reset reason: 0x%08X", reset_reason);

    int err = 0;

    if (!device_is_ready(err_led.port)) {
        LOG_ERR("Error LED GPIO device not ready");
        LOG_PANIC();
        for (;;) { }
    }

    err = error_service_init(&err_led);
    if (err) {
        LOG_ERR("Error service init failed: %d", err);
        LOG_PANIC();
        for (;;) { }
    }

    if (wdt_dev && device_is_ready(wdt_dev)) {
        struct wdt_timeout_cfg wdt_cfg = {
            .window.min = 0U,
            .window.max = WDT_TIMEOUT_MS,
            .callback   = NULL,
            .flags      = WDT_FLAG_RESET_SOC,
        };

        wdt_channel_id = wdt_install_timeout(wdt_dev, &wdt_cfg);
        if (wdt_channel_id < 0) {
            LOG_ERR("WDT install timeout failed: %d", wdt_channel_id);
            error_fatal(FATAL_GPIO_INIT);
        }

        wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);
    }

    if (!device_is_ready(i2c_dev_22)) {
        LOG_ERR("I2C device not ready");
        error_fatal(FATAL_VL53_INIT);
    }

    err = time_service_init(time_changed_cb);
    if (err) {
        error_fatal(FATAL_GPIO_INIT);
    }

    err = bt_conn_cb_register(&connection_callbacks);
    if (err) {
        error_fatal(FATAL_BT_INIT);
    }

    err = bt_conn_auth_cb_register(&auth_cb);
    if (err) {
        error_fatal(FATAL_BT_INIT);
    }

    err = bt_conn_auth_info_cb_register(&auth_info_cb);
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

    err = settings_load();
    if (err) {
        LOG_ERR("settings_load failed: %d", err);
        error_fatal(FATAL_BT_INIT);
    }

    err = advertising_set_init();
    if (err) {
        LOG_ERR("advertising_set_init failed: %d", err);
        error_fatal(FATAL_BT_INIT);
    }

    err = gpio_pin_configure_dt(&vl53_xshut_spec, GPIO_PULL_UP | GPIO_OUTPUT_INACTIVE);
    if (err) {
        error_fatal(FATAL_VL53_INIT);
    }

    gpio_pin_set_dt(&vl53_xshut_spec, 0);

    err = pm_device_runtime_enable(i2c_dev_22);
    if (err) {
        error_fatal(FATAL_VL53_INIT);
    }

    VL53L4CD_user_init();

    if (vl53_int_init()) {
        error_fatal(FATAL_GPIO_INIT);
    }

    bulk_stream_init();

    int64_t restored_last_event = 0;
    int64_t restored_carry_ms   = 0;
    int64_t seeded_time_ms      = 0;

    if (log_restore_state(&restored_last_event, &restored_carry_ms, &seeded_time_ms)) {
        last_event_unix = restored_last_event;
        carry_ms        = restored_carry_ms;
        set_real_time(seeded_time_ms);
        LOG_INF("persist restored: last_event=%lld carry=%lld time=%lld",
                (long long)last_event_unix,
                (long long)carry_ms,
                (long long)seeded_time_ms);
    } else {
        LOG_INF("no persist record — fresh start");
    }

    k_work_queue_init(&sensor_wq);
    k_work_queue_start(&sensor_wq, sensor_wq_stack,
                       K_THREAD_STACK_SIZEOF(sensor_wq_stack),
                       K_PRIO_PREEMPT(10), NULL);

    k_work_schedule(&schedule_work, K_NO_WAIT);

    k_work_schedule(&wdt_feed_work, K_MSEC(WDT_FEED_INTERVAL_MS));
    k_work_schedule_for_queue(&sensor_wq, &wdt_heartbeat_sensor_work,
                            K_MSEC(WDT_HEARTBEAT_INTERVAL_MS));

    k_work_schedule_for_queue(&sensor_wq, &vl53_watchdog_work,
                              K_MSEC(VL53_WATCHDOG_PERIOD_MS));
    
#if IS_ENABLED(CONFIG_APP_SHT_ENABLE)
    k_work_schedule_for_queue(&sensor_wq, &ambient_work, K_NO_WAIT);
#endif                          


    advertising_start();

    LOG_DBG("Successful init");

    k_sleep(K_FOREVER);
}