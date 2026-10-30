/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MD_RECEIVE_H_
#define ZEPHYR_SOC_QCOM_SAIL_MD_RECEIVE_H_

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>

#define SAIL_MD_RECEIVE_CHANNELS 2U
#define SAIL_MD_RECEIVE_BYTES    4U
#define SAIL_MD_RECEIVE_DEPTH    16U

struct sail_md_receive_event {
	uint32_t channel;
	uint8_t bytes[SAIL_MD_RECEIVE_BYTES];
};

enum sail_md_receive_state {
	SAIL_MD_RECEIVE_NEW,
	SAIL_MD_RECEIVE_STOPPED,
	SAIL_MD_RECEIVE_STARTING,
	SAIL_MD_RECEIVE_RUNNING,
	SAIL_MD_RECEIVE_STOPPING,
	SAIL_MD_RECEIVE_FAULTED,
};

struct sail_md_receive_stats {
	enum sail_md_receive_state state;
	uint32_t callbacks[SAIL_MD_RECEIVE_CHANNELS];
	uint32_t queued[SAIL_MD_RECEIVE_CHANNELS];
	uint32_t overflow[SAIL_MD_RECEIVE_CHANNELS];
	uint32_t inactive[SAIL_MD_RECEIVE_CHANNELS];
	uint32_t malformed, queue_errors;
	int first_queue_error, last_start_error, last_stop_error, cleanup_error;
	uint8_t registered_mask, possibly_enabled_mask;
	uint32_t pending;
};

/* Caller-owned, zero-initialized receiver; must outlive the boot. Fields are private. */
struct sail_md_receive {
	const struct device *device;
	struct k_msgq queue;
	char storage[SAIL_MD_RECEIVE_DEPTH * sizeof(struct sail_md_receive_event)] __aligned(4);
	struct sail_md_receive_stats stats;
};

/*
 * Bind a receiver to a ready mbox device with MTU 4 and two channels. All calls
 * in this API are thread context only on a single CPU, and the mbox driver
 * calls must not sleep, since they run with local IRQs locked. The caller must
 * own both channels exclusively; a previous client's callback is not restored.
 * Call once per object.
 *
 * @retval -EALREADY Already initialized.
 * @retval -ENODEV Device not ready.
 * @retval -ENOTSUP Wrong MTU or channel count.
 */
int sail_md_receive_init(struct sail_md_receive *receiver, const struct device *device);

/*
 * Register both callbacks, then enable both channels. IRQ routing must be set
 * up first. Channel 0 is non-secure, channel 1 secure. On failure the channels
 * are cleaned up, but events queued during the attempt are kept; check
 * last_start_error before using them. Requires STOPPED and an empty queue.
 *
 * @retval -EALREADY Already running.
 * @retval -EBUSY Not stopped, channels still held, or events pending.
 */
int sail_md_receive_start(struct sail_md_receive *receiver);

/*
 * Stop accepting events, disable both channels and unregister each channel
 * once it is known to be disabled. Both channels are attempted even after an
 * error. A failure leaves the receiver FAULTED; a later stop retries cleanup.
 * Queued events remain readable. Zero-latency interrupts are not covered.
 *
 * @return 0 or the first driver error.
 */
int sail_md_receive_stop(struct sail_md_receive *receiver);

/*
 * Dequeue one event without waiting. The bytes and channel are as received.
 *
 * @retval -ENOMSG Queue empty.
 */
int sail_md_receive_get(struct sail_md_receive *receiver, struct sail_md_receive_event *event);

/*
 * Copy the saturating counters and current state. A full queue drops the
 * newest event; zero overflow does not prove no message was missed in hardware.
 */
int sail_md_receive_stats_get(struct sail_md_receive *receiver,
			      struct sail_md_receive_stats *stats);

#endif
