/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/irq.h>
#include <zephyr/irq_offload.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/ztest.h>

#define FAKE_BASE      0x1000U
#define FAKE_IRQ_FIRST 37U

static uint32_t registers[4];
static uintptr_t reads[4];
static unsigned int read_count;
static unsigned int configure_count;
static unsigned int enable_count;
static unsigned int disable_count;
static bool enabled[2];
static bool read_in_isr;

static uint32_t fake_read32(mem_addr_t address)
{
	zassert_true(read_count < ARRAY_SIZE(reads));
	zassert_equal(address, FAKE_BASE + read_count * sizeof(uint32_t));
	reads[read_count] = address;
	read_in_isr = read_in_isr && k_is_in_isr();
	return registers[read_count++];
}

static void fake_irq_enable(unsigned int irq)
{
	zassert_true(irq >= FAKE_IRQ_FIRST && irq < FAKE_IRQ_FIRST + 2U);
	enabled[irq - FAKE_IRQ_FIRST] = true;
	enable_count++;
}

static void fake_irq_disable(unsigned int irq)
{
	zassert_true(irq >= FAKE_IRQ_FIRST && irq < FAKE_IRQ_FIRST + 2U);
	enabled[irq - FAKE_IRQ_FIRST] = false;
	disable_count++;
}

/* Keep the device API, spinlock and ISR context; substitute only I/O. */
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
#error This native_sim fixture requires the identity-mapped MMIO configuration
#endif

static void fake_configure_irqs(const struct device *dev)
{
	ARG_UNUSED(dev);
	configure_count++;
}

static const struct sail_rx_config config = {
	._mmio = {.addr = FAKE_BASE},
	.irq = {FAKE_IRQ_FIRST, FAKE_IRQ_FIRST + 1U},
	.configure_irqs = fake_configure_irqs,
};
static struct sail_rx_data data;

DEVICE_DEFINE(sail_rx_fixture, "sail_rx_fixture", sail_rx_init, NULL, &data, &config, POST_KERNEL,
	      CONFIG_MBOX_INIT_PRIORITY, &sail_rx_api);

static const struct device *const fixture = DEVICE_GET(sail_rx_fixture);

struct capture {
	unsigned int calls;
	uint32_t channel;
	uint8_t bytes[4];
	bool self_disable;
};

static void receive(const struct device *dev, uint32_t channel, void *context,
		    struct mbox_msg *message)
{
	struct capture *capture = context;

	zassert_equal_ptr(dev, fixture);
	zassert_true(k_is_in_isr());
	zassert_not_null(capture);
	zassert_equal(message->size, sizeof(capture->bytes));
	memcpy(capture->bytes, message->data, sizeof(capture->bytes));
	capture->channel = channel;
	capture->calls++;
	if (capture->self_disable) {
		zassert_ok(mbox_set_enabled(dev, channel, false));
	}
}

static void deliver(const void *channel_pointer)
{
	uint32_t channel = *(const uint32_t *)channel_pointer;

	if (channel == 0U) {
		sail_rx_nonsecure_isr(fixture);
	} else {
		sail_rx_secure_isr(fixture);
	}
}

static void dispatch(uint32_t channel)
{
	read_count = 0;
	read_in_isr = true;
	irq_offload(deliver, &channel);
}

static void before(void *unused)
{
	ARG_UNUSED(unused);
	for (uint32_t channel = 0; channel < 2U; channel++) {
		int status = mbox_set_enabled(fixture, channel, false);

		zassert_true(status == 0 || status == -EALREADY);
		zassert_ok(mbox_register_callback(fixture, channel, NULL, NULL));
	}
	enable_count = 0;
	disable_count = 0;
	read_count = 0;
}

static void *setup(void)
{
	zassert_true(device_is_ready(fixture));
	zassert_equal(configure_count, 1U);
	zassert_equal(disable_count, 2U);
	zassert_equal(enable_count, 0U);
	zassert_equal(read_count, 0U);
	zassert_false(enabled[0]);
	zassert_false(enabled[1]);
	return NULL;
}

ZTEST(sail_mbox_rx, test_api_and_activation_guards)
{
	zassert_equal(mbox_mtu_get(fixture), 4);
	zassert_equal(mbox_max_channels_get(fixture), 2U);
	zassert_equal(mbox_send(fixture, 0U, NULL), -ENOSYS);
	for (uint32_t channel = 0; channel < 2U; channel++) {
		zassert_equal(mbox_set_enabled(fixture, channel, true), -EINVAL);
		zassert_equal(mbox_set_enabled(fixture, channel, false), -EALREADY);
		dispatch(channel);
		zassert_equal(read_count, 0U);
	}
	const uint32_t invalid[] = {2U, UINT32_MAX};

	for (size_t i = 0; i < ARRAY_SIZE(invalid); i++) {
		zassert_equal(mbox_set_enabled(fixture, invalid[i], true), -EINVAL);
		zassert_equal(mbox_register_callback(fixture, invalid[i], receive, NULL), -EINVAL);
	}
	zassert_equal(enable_count, 0U);
	zassert_equal(disable_count, 0U);
}

ZTEST(sail_mbox_rx, test_both_channels_and_snapshot_lifetime)
{
	struct capture captures[2] = {0};

	for (uint32_t channel = 0; channel < 2U; channel++) {
		zassert_ok(mbox_register_callback(fixture, channel, receive, &captures[channel]));
		zassert_ok(mbox_set_enabled(fixture, channel, true));
		zassert_equal(mbox_set_enabled(fixture, channel, true), -EALREADY);
	}
	for (uint32_t iteration = 0; iteration < 64U; iteration++) {
		uint32_t channel = iteration % 2U;
		uint8_t expected[4];

		for (size_t i = 0; i < ARRAY_SIZE(registers); i++) {
			expected[i] = (uint8_t)(iteration * 13U + i);
			registers[i] = 0xa5c30000U | expected[i];
		}
		dispatch(channel);
		zassert_equal(read_count, 4U);
		zassert_true(read_in_isr);
		zassert_equal(captures[channel].channel, channel);
		memset(registers, 0, sizeof(registers));
		zassert_mem_equal(captures[channel].bytes, expected, sizeof(expected));
	}
	zassert_equal(captures[0].calls, 32U);
	zassert_equal(captures[1].calls, 32U);
	zassert_ok(mbox_set_enabled(fixture, 0U, false));
	dispatch(0U);
	zassert_equal(read_count, 0U);
	zassert_equal(captures[0].calls, 32U);
	zassert_true(enabled[1]);
	dispatch(1U);
	zassert_equal(captures[1].calls, 33U);
}

ZTEST(sail_mbox_rx, test_callback_replacement_and_self_disable)
{
	struct capture first = {0};
	struct capture replacement = {.self_disable = true};

	zassert_ok(mbox_register_callback(fixture, 1U, receive, &first));
	zassert_ok(mbox_set_enabled(fixture, 1U, true));
	zassert_equal(mbox_register_callback(fixture, 1U, receive, &replacement), -EBUSY);
	zassert_equal(mbox_register_callback(fixture, 1U, NULL, NULL), -EBUSY);
	dispatch(1U);
	zassert_equal(first.calls, 1U);
	zassert_equal(replacement.calls, 0U);
	zassert_ok(mbox_set_enabled(fixture, 1U, false));
	zassert_ok(mbox_register_callback(fixture, 1U, receive, &replacement));
	zassert_ok(mbox_set_enabled(fixture, 1U, true));
	dispatch(1U);
	zassert_equal(replacement.calls, 1U);
	zassert_false(enabled[1]);
	dispatch(1U);
	zassert_equal(read_count, 0U);
	zassert_equal(replacement.calls, 1U);
	zassert_ok(mbox_register_callback(fixture, 1U, NULL, &first));
	zassert_equal(mbox_set_enabled(fixture, 1U, true), -EINVAL);
}

ZTEST_SUITE(sail_mbox_rx, NULL, setup, before, NULL, NULL);
