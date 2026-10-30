/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "md_receive.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP), "SAIL receive lifecycle requires one CPU");

static void increment(uint32_t *counter)
{
	if (*counter != UINT32_MAX) {
		++*counter;
	}
}

static void receive(const struct device *device, uint32_t channel, void *user_data,
		    struct mbox_msg *message)
{
	struct sail_md_receive *r = user_data;
	struct sail_md_receive_event event;
	unsigned int key;
	int error;

	if (r == NULL) {
		return;
	}
	key = irq_lock();
	if (device != r->device || channel >= SAIL_MD_RECEIVE_CHANNELS || message == NULL ||
	    message->data == NULL || message->size != SAIL_MD_RECEIVE_BYTES) {
		increment(&r->stats.malformed);
		irq_unlock(key);
		return;
	}
	increment(&r->stats.callbacks[channel]);
	if (r->stats.state != SAIL_MD_RECEIVE_RUNNING &&
	    r->stats.state != SAIL_MD_RECEIVE_STARTING) {
		increment(&r->stats.inactive[channel]);
		irq_unlock(key);
		return;
	}
	event.channel = channel;
	memcpy(event.bytes, message->data, sizeof(event.bytes));
	error = k_msgq_put(&r->queue, &event, K_NO_WAIT);
	if (error == 0) {
		increment(&r->stats.queued[channel]);
	} else if (error == -ENOMSG) {
		increment(&r->stats.overflow[channel]);
	} else {
		increment(&r->stats.queue_errors);
		if (r->stats.first_queue_error == 0) {
			r->stats.first_queue_error = error;
		}
	}
	irq_unlock(key);
}

static int usable(const struct sail_md_receive *r)
{
	if (r == NULL || r->device == NULL || r->stats.state == SAIL_MD_RECEIVE_NEW) {
		return -EINVAL;
	}
	return k_is_in_isr() ? -EWOULDBLOCK : 0;
}

int sail_md_receive_init(struct sail_md_receive *r, const struct device *device)
{
	unsigned int key;

	if (r == NULL || device == NULL) {
		return -EINVAL;
	}
	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	key = irq_lock();
	if (r->stats.state != SAIL_MD_RECEIVE_NEW || r->device != NULL) {
		irq_unlock(key);
		return -EALREADY;
	}
	if (!device_is_ready(device)) {
		irq_unlock(key);
		return -ENODEV;
	}
	if (mbox_mtu_get(device) != SAIL_MD_RECEIVE_BYTES ||
	    mbox_max_channels_get(device) != SAIL_MD_RECEIVE_CHANNELS) {
		irq_unlock(key);
		return -ENOTSUP;
	}
	k_msgq_init(&r->queue, r->storage, sizeof(struct sail_md_receive_event),
		    SAIL_MD_RECEIVE_DEPTH);
	r->device = device;
	r->stats = (struct sail_md_receive_stats){.state = SAIL_MD_RECEIVE_STOPPED};
	irq_unlock(key);
	return 0;
}

/* Called with local IRQs locked, after revoking callback acceptance. */
static int cleanup(struct sail_md_receive *r)
{
	int first_error = 0;

	for (unsigned int channel = SAIL_MD_RECEIVE_CHANNELS; channel-- > 0;) {
		uint8_t bit = (uint8_t)BIT(channel);
		int error;

		if ((r->stats.registered_mask & bit) == 0U) {
			continue;
		}
		if ((r->stats.possibly_enabled_mask & bit) != 0U) {
			error = mbox_set_enabled(r->device, channel, false);
			if (error != 0 && error != -EALREADY) {
				if (first_error == 0) {
					first_error = error;
				}
				continue;
			}
			r->stats.possibly_enabled_mask &= (uint8_t)~bit;
		}
		error = mbox_register_callback(r->device, channel, NULL, NULL);
		if (error != 0) {
			if (first_error == 0) {
				first_error = error;
			}
		} else {
			r->stats.registered_mask &= (uint8_t)~bit;
		}
	}
	r->stats.cleanup_error = first_error;
	r->stats.state =
		r->stats.registered_mask != 0U ? SAIL_MD_RECEIVE_FAULTED : SAIL_MD_RECEIVE_STOPPED;
	return first_error;
}

int sail_md_receive_start(struct sail_md_receive *r)
{
	unsigned int key;
	int error = usable(r);

	if (error != 0) {
		return error;
	}
	key = irq_lock();
	if (r->stats.state == SAIL_MD_RECEIVE_RUNNING) {
		irq_unlock(key);
		return -EALREADY;
	}
	if (r->stats.state != SAIL_MD_RECEIVE_STOPPED || r->stats.registered_mask != 0U ||
	    r->stats.possibly_enabled_mask != 0U || k_msgq_num_used_get(&r->queue) != 0U) {
		irq_unlock(key);
		return -EBUSY;
	}
	r->stats.state = SAIL_MD_RECEIVE_STARTING;
	r->stats.last_start_error = 0;
	r->stats.cleanup_error = 0;
	for (uint32_t channel = 0; channel < SAIL_MD_RECEIVE_CHANNELS; ++channel) {
		error = mbox_register_callback(r->device, channel, receive, r);
		if (error != 0) {
			goto failed;
		}
		r->stats.registered_mask |= (uint8_t)BIT(channel);
	}
	for (uint32_t channel = 0; channel < SAIL_MD_RECEIVE_CHANNELS; ++channel) {
		/* An error may follow an effect; cleanup must attempt to disable it. */
		r->stats.possibly_enabled_mask |= (uint8_t)BIT(channel);
		error = mbox_set_enabled(r->device, channel, true);
		if (error != 0) {
			goto failed;
		}
	}
	r->stats.state = SAIL_MD_RECEIVE_RUNNING;
	irq_unlock(key);
	return 0;

failed:
	r->stats.last_start_error = error;
	r->stats.state = SAIL_MD_RECEIVE_STOPPING;
	(void)cleanup(r);
	irq_unlock(key);
	return error;
}

int sail_md_receive_stop(struct sail_md_receive *r)
{
	unsigned int key;
	int error = usable(r);

	if (error != 0) {
		return error;
	}
	key = irq_lock();
	r->stats.state = SAIL_MD_RECEIVE_STOPPING;
	error = cleanup(r);
	r->stats.last_stop_error = error;
	irq_unlock(key);
	return error;
}

int sail_md_receive_get(struct sail_md_receive *r, struct sail_md_receive_event *event)
{
	int error = usable(r);

	if (error != 0 || event == NULL) {
		return error != 0 ? error : -EINVAL;
	}
	return k_msgq_get(&r->queue, event, K_NO_WAIT);
}

int sail_md_receive_stats_get(struct sail_md_receive *r, struct sail_md_receive_stats *stats)
{
	unsigned int key;
	int error = usable(r);

	if (error != 0 || stats == NULL) {
		return error != 0 ? error : -EINVAL;
	}
	key = irq_lock();
	*stats = r->stats;
	stats->pending = k_msgq_num_used_get(&r->queue);
	irq_unlock(key);
	return 0;
}
