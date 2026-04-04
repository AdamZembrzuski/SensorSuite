/* log_service.c */

#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/atomic.h>

#include <errno.h>
#include <string.h>

#include "log_service.h"
#include "sense_service.h"

extern struct bt_conn *conn;

/* From BT_GATT_SERVICE_DEFINE(...) in sense_service.c */
extern struct bt_gatt_service sense_service_svc;

/* Ring buffers */
RING_BUF_DECLARE(rb_stamp, RB_STAMP_SIZE);
RING_BUF_DECLARE(rb_ambient, RB_AMBIENT_SIZE);

/* Bulk streaming packet storage */
static uint8_t bulk_pkt[BULK_PKT_MAX];

/* Streaming state */
static struct k_work_delayable bulk_stream_work;
static struct bt_gatt_notify_params bulk_ntf_params;

static atomic_t bulk_stream_active;
static atomic_t bulk_in_flight;

static uint8_t  bulk_stream_id = STREAM_ID_STAMP;
static uint16_t bulk_seq;

/* ---------------- Ring buffer API ---------------- */

int rb_ambient_put(const uint8_t *data, size_t len)
{
	if (!data || len == 0U) {
		return -EINVAL;
	}

	if (ring_buf_space_get(&rb_ambient) < len) {
		return -ENOSPC;
	}

	uint32_t wrote = ring_buf_put(&rb_ambient, data, len);
	return (wrote == len) ? 0 : -EIO;
}

int rb_ambient_get(uint8_t *data, size_t len)
{
	if (!data || len == 0U) {
		return -EINVAL;
	}

	if (ring_buf_size_get(&rb_ambient) < len) {
		return -ENOENT;
	}

	uint32_t got = ring_buf_get(&rb_ambient, data, len);
	return (got == len) ? 0 : -EIO;
}
int rb_stamp_put(int32_t value)
{
	if (ring_buf_space_get(&rb_stamp) < sizeof(value)) {
		return -ENOSPC;
	}

	uint32_t wrote = ring_buf_put(&rb_stamp, (const uint8_t *)&value, sizeof(value));
	return (wrote == sizeof(value)) ? 0 : -EIO;
}

int rb_stamp_get(int32_t *value)
{
	if (!value) {
		return -EINVAL;
	}

	if (ring_buf_size_get(&rb_stamp) < sizeof(*value)) {
		return -ENOENT;
	}

	uint32_t got = ring_buf_get(&rb_stamp, (uint8_t *)value, sizeof(*value));
	return (got == sizeof(*value)) ? 0 : -EIO;
}

uint32_t rb_stamp_count(void)
{
	return ring_buf_size_get(&rb_stamp) / sizeof(int32_t);
}

/* ---------------- Bulk streaming internals ---------------- */

/* Max payload bytes we can fit in one notification:
 * min(BULK_PKT_MAX, (MTU-3)) - BULK_HDR_LEN
 */
static uint16_t max_payload_bytes(struct bt_conn *c)
{
	uint16_t mtu = bt_gatt_get_mtu(c);
	uint16_t max_ntf = (mtu > 3U) ? (mtu - 3U) : 0U;

	if (max_ntf <= BULK_HDR_LEN) {
		return 0U;
	}

	uint16_t cap = MIN((uint16_t)BULK_PKT_MAX, max_ntf);
	return cap - BULK_HDR_LEN;
}

static void bulk_sent_cb(struct bt_conn *c, void *user_data)
{
	ARG_UNUSED(c);
	ARG_UNUSED(user_data);

	atomic_set(&bulk_in_flight, false);

	if (atomic_get(&bulk_stream_active)) {
		k_work_schedule(&bulk_stream_work, K_MSEC(5));
	}
}

static int bulk_notify_once(struct bt_conn *c, const uint8_t *data, uint16_t len)
{
	/* BULK value attribute is attrs[11] for current service layout */
	bulk_ntf_params.attr = &sense_service_svc.attrs[11];
	bulk_ntf_params.data = data;
	bulk_ntf_params.len  = len;
	bulk_ntf_params.func = bulk_sent_cb;
	bulk_ntf_params.user_data = NULL;

	return bt_gatt_notify_cb(c, &bulk_ntf_params);
}

static int send_header_only(struct bt_conn *c, uint8_t stream_id, uint8_t flags)
{
	bulk_pkt[0] = stream_id;
	sys_put_le16(bulk_seq, &bulk_pkt[1]);
	bulk_pkt[3] = flags;

	int err = bulk_notify_once(c, bulk_pkt, BULK_HDR_LEN);
	if (err == 0) {
		bulk_seq++;
		atomic_set(&bulk_in_flight, true);
	}
	return err;
}

/* Sends ONE notification worth of data (or a LAST marker).
 * Returns:
 *   0        -> queued one notification (in flight)
 *  -ENOMEM   -> no buffers; caller should retry later
 *  <0        -> fatal; stop
 */
static int stream_send_one(struct bt_conn *c)
{
	uint16_t payload_cap = max_payload_bytes(c);
	if (payload_cap == 0U) {
		return -EINVAL;
	}

	struct ring_buf *rb = (bulk_stream_id == STREAM_ID_STAMP) ? &rb_stamp : &rb_ambient;

	/* Stamps are int32_t records -> keep payload multiple of 4 */
	if (bulk_stream_id == STREAM_ID_STAMP) {
		payload_cap = (payload_cap / 4U) * 4U;
		if (payload_cap == 0U) {
			return -EINVAL;
		}
	}

	/* Header (seq only increments when notify queues successfully) */
	bulk_pkt[0] = bulk_stream_id;
	sys_put_le16(bulk_seq, &bulk_pkt[1]);
	bulk_pkt[3] = 0;

	uint8_t *rb_ptr = NULL;
	uint32_t claimed = ring_buf_get_claim(rb, &rb_ptr, payload_cap);

	if (claimed == 0U) {
		/* Stream empty -> send LAST */
		int err = send_header_only(c, bulk_stream_id, BULK_FLAG_LAST);
		if (err != 0) {
			return err; /* -ENOMEM => retry */
		}

		/* Advance stream state immediately (packet is in-flight) */
		if (bulk_stream_id == STREAM_ID_STAMP) {
			bulk_stream_id = STREAM_ID_AMBIENT;
			return 0;
		}

		/* Ambient LAST sent -> done */
		atomic_set(&bulk_stream_active, false);
		return 0;
	}

	memcpy(&bulk_pkt[BULK_HDR_LEN], rb_ptr, claimed);

	int err = bulk_notify_once(c, bulk_pkt, (uint16_t)(BULK_HDR_LEN + claimed));
	if (err == 0) {
		/* Consume only after notify successfully queued */
		(void)ring_buf_get_finish(rb, claimed);
		bulk_seq++;
		atomic_set(&bulk_in_flight, true);
		return 0;
	}

	/* -ENOMEM: do not finish/advance, retry later */
	if (err == -ENOMEM) {
		return -ENOMEM;
	}

	/* Other errors: stop */
	(void)ring_buf_get_finish(rb, 0);
	atomic_set(&bulk_stream_active, false);
	return err;
}

/* Work handler: queue ONE notification per run */
static void bulk_stream_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!atomic_get(&bulk_stream_active) || atomic_get(&bulk_in_flight) || (conn == NULL)) {
		return;
	}

	int err = stream_send_one(conn);

	if (err == -ENOMEM) {
		/* Backpressure: retry later */
		k_work_schedule(&bulk_stream_work, K_MSEC(10));
		return;
	}

	if (err < 0) {
		atomic_set(&bulk_stream_active, false);
	}
	/* On success (err==0), next step is scheduled by bulk_sent_cb() */
}

/* ---------------- Public streaming API ---------------- */

void bulk_stream_init(void)
{
	k_work_init_delayable(&bulk_stream_work, bulk_stream_work_handler);
	atomic_set(&bulk_stream_active, false);
	atomic_set(&bulk_in_flight, false);
}

void bulk_stream_start(void)
{
	if (conn == NULL) {
		return;
	}
	if (atomic_get(&bulk_stream_active)) {
		return;
	}

	bulk_seq = 0;
	bulk_stream_id = STREAM_ID_STAMP;
	atomic_set(&bulk_stream_active, true);
	atomic_set(&bulk_in_flight, false);

	k_work_schedule(&bulk_stream_work, K_NO_WAIT);
}

void bulk_stream_stop(void)
{
	atomic_set(&bulk_stream_active, false);
	atomic_set(&bulk_in_flight, false);
	k_work_cancel_delayable(&bulk_stream_work);
}