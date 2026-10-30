/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <limits.h>
#include <string.h>
#include "md_receive.h"
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>
#include <zephyr/irq_offload.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/ztest.h>

/* Build the driver itself, replacing only hardware I/O. */
#define FAKE_BASE 0x1000U
static uint32_t registers[4];
static unsigned int mmio_reads;
static bool real_enabled[2];

static uint32_t fake_read32(mem_addr_t address)
{
	zassert_true(k_is_in_isr());
	zassert_true(mmio_reads < ARRAY_SIZE(registers));
	zassert_equal(address, FAKE_BASE + mmio_reads * sizeof(uint32_t));
	return registers[mmio_reads++];
}

static void fake_irq_enable(unsigned int irq)
{
	zassert_true(irq == 37 || irq == 38);
	real_enabled[irq - 37] = true;
}

static void fake_irq_disable(unsigned int irq)
{
	zassert_true(irq == 37 || irq == 38);
	real_enabled[irq - 37] = false;
}

#pragma push_macro("irq_enable")
#pragma push_macro("irq_disable")
#undef irq_enable
#undef irq_disable
#define sys_read32  fake_read32
#define irq_enable  fake_irq_enable
#define irq_disable fake_irq_disable
#include "mbox_qcom_sail_rx.c"
#undef sys_read32
#undef irq_enable
#undef irq_disable
#pragma pop_macro("irq_disable")
#pragma pop_macro("irq_enable")

#ifdef DEVICE_MMIO_IS_IN_RAM
#error This fixture requires native_sim identity-mapped MMIO
#endif

static void configure_irqs(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static const struct sail_rx_config real_config = {
	._mmio = {.addr = FAKE_BASE},
	.irq = {37, 38},
	.configure_irqs = configure_irqs,
};
static struct sail_rx_data real_data;
DEVICE_DEFINE(md_real_fixture, "md_real_fixture", sail_rx_init, NULL, &real_data, &real_config,
	      POST_KERNEL, CONFIG_MBOX_INIT_PRIORITY, &sail_rx_api);
static const struct device *const real_device = DEVICE_GET(md_real_fixture);

struct fake_channel {
	mbox_callback_t callback;
	void *context;
	bool enabled;
};
struct fake_operation {
	char kind;
	uint32_t channel;
};
static struct {
	struct fake_channel channels[2];
	struct fake_operation trace[32];
	unsigned int operations, fail_at, second_fail_at, sends;
	int error, second_error, mtu;
	uint32_t channel_count;
	bool after_effect, inject_on_enable;
} fake;

static int operation(char kind, uint32_t channel)
{
	zassert_true(fake.operations < ARRAY_SIZE(fake.trace));
	fake.trace[fake.operations++] = (struct fake_operation){kind, channel};
	return fake.operations == fake.fail_at          ? fake.error
	       : fake.operations == fake.second_fail_at ? fake.second_error
							: 0;
}

static int fake_register(const struct device *dev, uint32_t channel, mbox_callback_t callback,
			 void *context)
{
	int error;

	ARG_UNUSED(dev);

	if (channel >= 2) {
		return -EINVAL;
	}
	error = operation(callback != NULL ? 'R' : 'U', channel);
	/* A failure leaves the registration unchanged, as in the driver. */
	if (error != 0) {
		return error;
	}
	if (fake.channels[channel].enabled) {
		return -EBUSY;
	}
	fake.channels[channel].callback = callback;
	fake.channels[channel].context = callback != NULL ? context : NULL;
	return 0;
}

static int fake_enable(const struct device *dev, uint32_t channel, bool enabled)
{
	uint8_t bytes[4] = {0x00, 0x00, 0x05, 0xcd};
	struct mbox_msg message = {.data = bytes, .size = sizeof(bytes)};
	struct fake_channel *c;
	int error;

	if (channel >= 2) {
		return -EINVAL;
	}
	c = &fake.channels[channel];
	error = operation(enabled ? 'E' : 'D', channel);
	if (error != 0 && !fake.after_effect) {
		return error;
	}
	if (c->enabled == enabled) {
		return error != 0 ? error : -EALREADY;
	}
	if (enabled && c->callback == NULL) {
		return -EINVAL;
	}
	c->enabled = enabled;
	if (enabled && fake.inject_on_enable) {
		/* Synthetic boundary callback, not a claim about IRQ-lock timing. */
		c->callback(dev, channel, c->context, &message);
		memset(bytes, 0xff, sizeof(bytes));
	}
	return error;
}

static int fake_send(const struct device *dev, uint32_t channel, const struct mbox_msg *message)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(message);
	fake.sends++;
	return -ENOSYS;
}

static int fake_mtu(const struct device *dev)
{
	ARG_UNUSED(dev);
	return fake.mtu;
}

static uint32_t fake_channels(const struct device *dev)
{
	ARG_UNUSED(dev);
	return fake.channel_count;
}

static DEVICE_API(mbox, fake_api) = {
	.register_callback = fake_register,
	.set_enabled = fake_enable,
	.mtu_get = fake_mtu,
	.max_channels_get = fake_channels,
	.send = fake_send,
};
DEVICE_DEFINE(md_fault_fixture, "md_fault_fixture", NULL, NULL, NULL, NULL, POST_KERNEL,
	      CONFIG_MBOX_INIT_PRIORITY, &fake_api);
static const struct device *const fault_device = DEVICE_GET(md_fault_fixture);

static int unavailable_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENODEV;
}
DEVICE_DEFINE(md_unavailable_fixture, "md_unavailable_fixture", unavailable_init, NULL, NULL, NULL,
	      POST_KERNEL, CONFIG_MBOX_INIT_PRIORITY, &fake_api);

static struct sail_md_receive real_receiver, receiver, init_receiver;

static void clear_trace(void)
{
	fake.operations = 0;
	fake.fail_at = 0;
	fake.second_fail_at = 0;
	fake.error = -EIO;
	fake.second_error = -EFAULT;
	fake.after_effect = false;
	fake.inject_on_enable = false;
}

static struct sail_md_receive_stats stats(struct sail_md_receive *r)
{
	struct sail_md_receive_stats result;

	zassert_ok(sail_md_receive_stats_get(r, &result));

	return result;
}

static void drain(struct sail_md_receive *r)
{
	struct sail_md_receive_event event;
	int error;

	do {

		error = sail_md_receive_get(r, &event);
	} while (error == 0);
	zassert_equal(error, -ENOMSG);
}

struct callback_delivery {
	mbox_callback_t callback;
	const struct device *device;
	uint32_t channel;
	void *context;
	struct mbox_msg *message;
};

static void invoke(const void *parameter)
{
	const struct callback_delivery *d = parameter;

	zassert_true(k_is_in_isr());

	d->callback(d->device, d->channel, d->context, d->message);
}

static void deliver(uint32_t channel, uint8_t value)
{
	uint8_t bytes[4] = {value, (uint8_t)(value + 1), 0x07, 0xcd};
	struct mbox_msg message = {.data = bytes, .size = sizeof(bytes)};
	struct callback_delivery d = {fake.channels[channel].callback, fault_device, channel,
				      fake.channels[channel].context, &message};
	zassert_not_null(d.callback);
	irq_offload(invoke, &d);
	memset(bytes, 0xff, sizeof(bytes));
}

static void real_deliver(const void *parameter)
{
	uint32_t channel = *(const uint32_t *)parameter;

	if (channel == 0) {

		sail_rx_nonsecure_isr(real_device);
	} else {
		sail_rx_secure_isr(real_device);
	}
}

static void *setup(void)
{
	fake.mtu = 4;
	fake.channel_count = 2;
	clear_trace();
	zassert_true(device_is_ready(real_device));
	zassert_true(device_is_ready(fault_device));
	zassert_ok(sail_md_receive_init(&real_receiver, real_device));
	zassert_ok(sail_md_receive_init(&receiver, fault_device));
	return NULL;
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	clear_trace();
	zassert_ok(sail_md_receive_stop(&receiver));
	zassert_ok(sail_md_receive_stop(&real_receiver));
	drain(&receiver);
	drain(&real_receiver);
	zassert_is_null(fake.channels[0].callback);
	zassert_is_null(fake.channels[1].callback);
	zassert_false(fake.channels[0].enabled);
	zassert_false(fake.channels[1].enabled);
	clear_trace();
}

ZTEST(sail_md_receive, test_real_driver_copy_and_channels)
{
	zassert_ok(sail_md_receive_start(&real_receiver));
	zassert_true(real_enabled[0] && real_enabled[1]);
	for (uint32_t i = 0; i < 64; ++i) {
		uint32_t channel = i & 1U;
		uint8_t expected[4];
		struct sail_md_receive_event event;

		for (unsigned int n = 0; n < 4; ++n) {
			expected[n] = (uint8_t)(i * 7U + n);
			registers[n] = UINT32_C(0xabcde000) | expected[n];
		}
		mmio_reads = 0;
		irq_offload(real_deliver, &channel);
		zassert_equal(mmio_reads, 4);
		memset(registers, 0, sizeof(registers));
		zassert_ok(sail_md_receive_get(&real_receiver, &event));
		zassert_equal(event.channel, channel);
		zassert_mem_equal(event.bytes, expected, 4);
	}
	zassert_ok(sail_md_receive_stop(&real_receiver));
	zassert_false(real_enabled[0] || real_enabled[1]);
	uint32_t channel = 0;

	mmio_reads = 0;

	irq_offload(real_deliver, &channel);
	zassert_equal(mmio_reads, 0);
}

ZTEST(sail_md_receive, test_queue_overflow_and_retained_order)
{
	struct sail_md_receive_stats before = stats(&receiver);
	struct sail_md_receive_event event;

	zassert_ok(sail_md_receive_start(&receiver));
	for (uint32_t i = 0; i < SAIL_MD_RECEIVE_DEPTH + 5; ++i) {
		deliver(i & 1U, (uint8_t)i);
	}
	struct sail_md_receive_stats after = stats(&receiver);

	zassert_equal(after.pending, SAIL_MD_RECEIVE_DEPTH);

	zassert_equal(after.overflow[0] - before.overflow[0], 3);
	zassert_equal(after.overflow[1] - before.overflow[1], 2);
	zassert_ok(sail_md_receive_stop(&receiver));
	zassert_equal(sail_md_receive_start(&receiver), -EBUSY);
	for (uint32_t i = 0; i < SAIL_MD_RECEIVE_DEPTH; ++i) {
		zassert_ok(sail_md_receive_get(&receiver, &event));
		zassert_equal(event.channel, i & 1U);
		zassert_equal(event.bytes[0], i);
		zassert_equal(event.bytes[1], i + 1);
		zassert_equal(event.bytes[2], 0x07);
		zassert_equal(event.bytes[3], 0xcd);
	}
	zassert_equal(sail_md_receive_get(&receiver, &event), -ENOMSG);
	zassert_ok(sail_md_receive_start(&receiver));
	zassert_equal(sail_md_receive_start(&receiver), -EALREADY);
	zassert_equal(fake.sends, 0);
}

ZTEST(sail_md_receive, test_start_failures_and_partial_events)
{
	for (unsigned int fail = 1; fail <= 4; ++fail) {
		for (unsigned int after = 0; after < 2; ++after) {
			clear_trace();
			fake.fail_at = fail;
			fake.after_effect = after != 0;
			fake.inject_on_enable = true;
			zassert_equal(sail_md_receive_start(&receiver), -EIO);
			struct sail_md_receive_stats s = stats(&receiver);

			zassert_equal(s.state, SAIL_MD_RECEIVE_STOPPED);

			zassert_equal(s.last_start_error, -EIO);
			zassert_equal(s.cleanup_error, 0);
			zassert_equal(s.registered_mask, 0);
			zassert_equal(s.possibly_enabled_mask, 0);
			unsigned int pending = fail < 3 ? 0 : fail == 3 ? after : 1U + after;

			zassert_equal(s.pending, pending);

			if (pending != 0U) {
				struct sail_md_receive_event event;

				zassert_equal(sail_md_receive_start(&receiver), -EBUSY);

				for (unsigned int i = 0; i < pending; ++i) {
					zassert_ok(sail_md_receive_get(&receiver, &event));
					const uint8_t expected[4] = {0, 0, 5, 0xcd};

					zassert_equal(event.channel, i);

					zassert_mem_equal(event.bytes, expected, 4);
				}
			}
			clear_trace();
			zassert_ok(sail_md_receive_start(&receiver));
			zassert_ok(sail_md_receive_stop(&receiver));
		}
	}
}

ZTEST(sail_md_receive, test_failed_cleanup_and_retry)
{
	for (unsigned int fail = 1; fail <= 4; ++fail) {
		for (unsigned int after = 0; after < 2; ++after) {
			clear_trace();
			zassert_ok(sail_md_receive_start(&receiver));
			clear_trace();
			fake.fail_at = fail;
			fake.after_effect = after != 0;
			zassert_equal(sail_md_receive_stop(&receiver), -EIO);
			struct sail_md_receive_stats s = stats(&receiver);

			zassert_equal(s.state, SAIL_MD_RECEIVE_FAULTED);

			zassert_not_equal(s.registered_mask, 0);
			zassert_equal(s.last_stop_error, -EIO);
			zassert_equal(s.cleanup_error, -EIO);
			zassert_equal(sail_md_receive_start(&receiver), -EBUSY);
			clear_trace();
			zassert_ok(sail_md_receive_stop(&receiver));
			s = stats(&receiver);
			zassert_equal(s.state, SAIL_MD_RECEIVE_STOPPED);
			zassert_equal(s.registered_mask | s.possibly_enabled_mask, 0);
		}
	}
	clear_trace();
	fake.fail_at = 4; /* Second enable fails, then cleanup of that channel fails. */
	fake.second_fail_at = 5;
	zassert_equal(sail_md_receive_start(&receiver), -EIO);
	struct sail_md_receive_stats s = stats(&receiver);

	zassert_equal(s.last_start_error, -EIO);

	zassert_equal(s.cleanup_error, -EFAULT);
	zassert_equal(s.state, SAIL_MD_RECEIVE_FAULTED);
	zassert_equal(s.registered_mask, 2);
	zassert_equal(s.possibly_enabled_mask, 2);
	clear_trace();
	zassert_ok(sail_md_receive_stop(&receiver));
}

ZTEST(sail_md_receive, test_malformed_callbacks_and_late_delivery)
{
	uint8_t bytes[5] = {0};
	struct mbox_msg message = {.data = bytes, .size = 4};
	struct sail_md_receive_stats before = stats(&receiver);

	zassert_ok(sail_md_receive_start(&receiver));

	struct callback_delivery saved = {fake.channels[0].callback, fault_device, 0,
					  fake.channels[0].context, &message};
	for (unsigned int fault = 0; fault < 7; ++fault) {
		struct callback_delivery d = saved;

		message.data = bytes;

		message.size = 4;
		switch (fault) {
		case 0:
			d.device = real_device;
			break;
		case 1:
			d.channel = 2;
			break;
		case 2:
			d.channel = UINT32_MAX;
			break;
		case 3:
			d.message = NULL;
			break;
		case 4:
			message.data = NULL;
			break;
		case 5:
			message.size = 3;
			break;
		case 6:
			message.size = 5;
			break;
		}
		irq_offload(invoke, &d);
	}
	struct sail_md_receive_stats s = stats(&receiver);

	zassert_equal(s.malformed - before.malformed, 7);

	zassert_equal(s.pending, 0);
	message.data = bytes;
	message.size = 4;
	zassert_ok(sail_md_receive_stop(&receiver));
	/* Simulate a stale callback; on a single core, stop drains the ISR. */
	irq_offload(invoke, &saved);
	s = stats(&receiver);
	zassert_equal(s.inactive[0] - before.inactive[0], 1);
	zassert_equal(s.pending, 0);
	saved.context = NULL;
	irq_offload(invoke, &saved);
}

ZTEST(sail_md_receive, test_saturating_diagnostics)
{
	zassert_ok(sail_md_receive_start(&receiver));
	for (unsigned int i = 0; i < SAIL_MD_RECEIVE_DEPTH; ++i) {
		deliver(0, 1);
	}
	unsigned int key = irq_lock();

	receiver.stats.callbacks[0] = UINT32_MAX - 1;

	receiver.stats.overflow[0] = UINT32_MAX - 1;
	irq_unlock(key);
	deliver(0, 1);
	deliver(0, 1);
	struct sail_md_receive_stats s = stats(&receiver);

	zassert_equal(s.callbacks[0], UINT32_MAX);

	zassert_equal(s.overflow[0], UINT32_MAX);
	drain(&receiver);
	key = irq_lock();
	receiver.stats.queued[0] = UINT32_MAX - 1;
	irq_unlock(key);
	deliver(0, 1);
	deliver(0, 1);
	zassert_equal(stats(&receiver).queued[0], UINT32_MAX);
	/* Restore counters for the remaining tests. */
	key = irq_lock();
	receiver.stats.callbacks[0] = 0;
	receiver.stats.overflow[0] = 0;
	receiver.stats.queued[0] = 0;
	irq_unlock(key);
}

static void forbidden_public_calls(const void *unused)
{
	struct sail_md_receive_event event;
	struct sail_md_receive_stats s;

	ARG_UNUSED(unused);

	zassert_equal(sail_md_receive_init(&init_receiver, fault_device), -EWOULDBLOCK);
	zassert_equal(sail_md_receive_start(&receiver), -EWOULDBLOCK);
	zassert_equal(sail_md_receive_stop(&receiver), -EWOULDBLOCK);
	zassert_equal(sail_md_receive_get(&receiver, &event), -EWOULDBLOCK);
	zassert_equal(sail_md_receive_stats_get(&receiver, &s), -EWOULDBLOCK);
}

ZTEST(sail_md_receive, test_api_guards_and_existing_owner)
{
	struct sail_md_receive_event event;

	zassert_equal(sail_md_receive_init(NULL, fault_device), -EINVAL);

	zassert_equal(sail_md_receive_init(&init_receiver, NULL), -EINVAL);
	zassert_equal(sail_md_receive_init(&init_receiver, DEVICE_GET(md_unavailable_fixture)),
		      -ENODEV);
	fake.mtu = 3;
	zassert_equal(sail_md_receive_init(&init_receiver, fault_device), -ENOTSUP);
	fake.mtu = 4;
	fake.channel_count = 3;
	zassert_equal(sail_md_receive_init(&init_receiver, fault_device), -ENOTSUP);
	fake.channel_count = 2;
	zassert_equal(sail_md_receive_init(&receiver, fault_device), -EALREADY);
	zassert_equal(sail_md_receive_start(&init_receiver), -EINVAL);
	zassert_equal(sail_md_receive_stop(NULL), -EINVAL);
	zassert_equal(sail_md_receive_get(NULL, &event), -EINVAL);
	zassert_equal(sail_md_receive_get(&receiver, NULL), -EINVAL);
	zassert_equal(sail_md_receive_stats_get(&receiver, NULL), -EINVAL);
	irq_offload(forbidden_public_calls, NULL);
	/* An existing enabled channel is refused, never disabled/adopted. */
	fake.channels[0].enabled = true;
	zassert_equal(sail_md_receive_start(&receiver), -EBUSY);
	zassert_true(fake.channels[0].enabled);
	zassert_equal(fake.operations, 1);
	fake.channels[0].enabled = false;
	clear_trace();
	fake.channels[1].enabled = true;
	zassert_equal(sail_md_receive_start(&receiver), -EBUSY);
	zassert_true(fake.channels[1].enabled);
	zassert_is_null(fake.channels[0].callback);
	zassert_equal(fake.operations, 3); /* R0, refused R1, U0 only. */
	fake.channels[1].enabled = false;
	zassert_equal(fake.sends, 0);
}

ZTEST_SUITE(sail_md_receive, NULL, setup, before, NULL, NULL);
