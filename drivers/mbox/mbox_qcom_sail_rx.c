/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT qcom_sail_mbox_rx

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/dt-bindings/interrupt-controller/arm-gic.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define SAIL_RX_CHANNELS        2U
#define SAIL_RX_BYTES           4U

struct sail_rx_config {
	DEVICE_MMIO_ROM;
	unsigned int irq[SAIL_RX_CHANNELS];
	void (*configure_irqs)(const struct device *dev);
};

struct sail_rx_channel {
	mbox_callback_t callback;
	void *user_data;
	bool enabled;
};

struct sail_rx_data {
	DEVICE_MMIO_RAM;
	struct k_spinlock lock;
	struct sail_rx_channel channels[SAIL_RX_CHANNELS];
};

static void sail_rx_receive(const struct device *dev, uint32_t channel)
{
	mm_reg_t base = DEVICE_MMIO_GET(dev);
	struct sail_rx_data *data = dev->data;
	struct sail_rx_channel *state = &data->channels[channel];
	uint8_t bytes[SAIL_RX_BYTES];
	struct mbox_msg message = {.data = bytes, .size = sizeof(bytes)};
	mbox_callback_t callback;
	void *user_data;
	k_spinlock_key_t key = k_spin_lock(&data->lock);

	if (!state->enabled || state->callback == NULL) {
		k_spin_unlock(&data->lock, key);
		return;
	}
	callback = state->callback;
	user_data = state->user_data;
	/* Both doorbells share four registers, each carrying one low byte. */
	for (size_t i = 0; i < sizeof(bytes); i++) {
		bytes[i] = (uint8_t)sys_read32(base + i * sizeof(uint32_t));
	}
	k_spin_unlock(&data->lock, key);

	/* The client must copy this snapshot before returning from the ISR. */
	callback(dev, channel, user_data, &message);
}

static void sail_rx_nonsecure_isr(const struct device *dev)
{
	sail_rx_receive(dev, 0U);
}

static void sail_rx_secure_isr(const struct device *dev)
{
	sail_rx_receive(dev, 1U);
}

static int sail_rx_register_callback(const struct device *dev, uint32_t channel,
				     mbox_callback_t callback, void *user_data)
{
	struct sail_rx_data *data = dev->data;
	k_spinlock_key_t key;

	if (channel >= SAIL_RX_CHANNELS) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	if (data->channels[channel].enabled) {
		k_spin_unlock(&data->lock, key);
		return -EBUSY;
	}
	data->channels[channel].callback = callback;
	data->channels[channel].user_data = callback != NULL ? user_data : NULL;
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int sail_rx_set_enabled(const struct device *dev, uint32_t channel, bool enabled)
{
	const struct sail_rx_config *config = dev->config;
	struct sail_rx_data *data = dev->data;
	struct sail_rx_channel *state;
	k_spinlock_key_t key;

	if (channel >= SAIL_RX_CHANNELS) {
		return -EINVAL;
	}
	key = k_spin_lock(&data->lock);
	state = &data->channels[channel];
	if (state->enabled == enabled) {
		k_spin_unlock(&data->lock, key);
		return -EALREADY;
	}
	if (enabled && state->callback == NULL) {
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}
	if (enabled) {
		state->enabled = true;
		irq_enable(config->irq[channel]);
	} else {
		irq_disable(config->irq[channel]);
		state->enabled = false;
	}
	k_spin_unlock(&data->lock, key);
	return 0;
}

static int sail_rx_mtu_get(const struct device *dev)
{
	ARG_UNUSED(dev);
	return SAIL_RX_BYTES;
}

static uint32_t sail_rx_max_channels_get(const struct device *dev)
{
	ARG_UNUSED(dev);
	return SAIL_RX_CHANNELS;
}

static DEVICE_API(mbox, sail_rx_api) = {
	.register_callback = sail_rx_register_callback,
	.set_enabled = sail_rx_set_enabled,
	.mtu_get = sail_rx_mtu_get,
	.max_channels_get = sail_rx_max_channels_get,
};

static int sail_rx_init(const struct device *dev)
{
	const struct sail_rx_config *config = dev->config;

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	config->configure_irqs(dev);
	for (size_t i = 0; i < SAIL_RX_CHANNELS; i++) {
		irq_disable(config->irq[i]);
	}
	return 0;
}

#define SAIL_RX_DEFINE(inst)                                                                       \
	BUILD_ASSERT(DT_INST_REG_SIZE(inst) == SAIL_RX_BYTES * sizeof(uint32_t));                  \
	BUILD_ASSERT(DT_INST_REG_ADDR(inst) % sizeof(uint32_t) == 0U);                             \
	BUILD_ASSERT(DT_NUM_IRQS(DT_DRV_INST(inst)) == SAIL_RX_CHANNELS);                          \
	BUILD_ASSERT(DT_INST_IRQN_BY_NAME(inst, nonsecure) !=                                      \
		     DT_INST_IRQN_BY_NAME(inst, secure));                                          \
	BUILD_ASSERT(DT_INST_IRQ_BY_NAME(inst, nonsecure, flags) == IRQ_TYPE_EDGE);                \
	BUILD_ASSERT(DT_INST_IRQ_BY_NAME(inst, secure, flags) == IRQ_TYPE_EDGE);                   \
	static void sail_rx_configure_##inst(const struct device *dev)                             \
	{                                                                                          \
		ARG_UNUSED(dev);                                                                   \
		IRQ_CONNECT(DT_INST_IRQN_BY_NAME(inst, nonsecure),                                 \
			    DT_INST_IRQ_BY_NAME(inst, nonsecure, priority), sail_rx_nonsecure_isr, \
			    DEVICE_DT_INST_GET(inst),                                              \
			    DT_INST_IRQ_BY_NAME(inst, nonsecure, flags));                          \
		IRQ_CONNECT(DT_INST_IRQN_BY_NAME(inst, secure),                                    \
			    DT_INST_IRQ_BY_NAME(inst, secure, priority), sail_rx_secure_isr,       \
			    DEVICE_DT_INST_GET(inst), DT_INST_IRQ_BY_NAME(inst, secure, flags));   \
	}                                                                                          \
	static const struct sail_rx_config sail_rx_config_##inst = {                               \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(inst)),                                           \
		.irq = {DT_INST_IRQN_BY_NAME(inst, nonsecure),                                     \
			DT_INST_IRQN_BY_NAME(inst, secure)},                                       \
		.configure_irqs = sail_rx_configure_##inst,                                        \
	};                                                                                         \
	static struct sail_rx_data sail_rx_data_##inst;                                            \
	DEVICE_DT_INST_DEFINE(inst, sail_rx_init, NULL, &sail_rx_data_##inst,                      \
			      &sail_rx_config_##inst, POST_KERNEL, CONFIG_MBOX_INIT_PRIORITY,      \
			      &sail_rx_api);

DT_INST_FOREACH_STATUS_OKAY(SAIL_RX_DEFINE)
