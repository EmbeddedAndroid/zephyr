/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/sys/util.h>
#include <soc.h>

#define DT_DRV_COMPAT qcom_sail_platform

#define HAS_DEVICES(n) || DT_INST_NODE_HAS_PROP(n, qcom_devices)
#define SAIL_PLATFORM_DEVICES (0 DT_INST_FOREACH_STATUS_OKAY(HAS_DEVICES))

#if SAIL_PLATFORM_DEVICES

/*
 * Devices are found by name at run time: a listed device whose driver is not
 * built has no device object, and a reference to it would not link.
 */
#define DEVICE_NAME(node, prop, idx) DEVICE_DT_NAME(DT_PHANDLE_BY_IDX(node, prop, idx)),

#define PLATFORM_DEVICES(n)                                                                        \
	IF_ENABLED(DT_INST_NODE_HAS_PROP(n, qcom_devices),                                         \
		   (static const char *const devices_##n[] = {                                     \
			    DT_INST_FOREACH_PROP_ELEM(n, qcom_devices, DEVICE_NAME)};))

DT_INST_FOREACH_STATUS_OKAY(PLATFORM_DEVICES)

static void init_devices(uint16_t chip, uint16_t board_chip, const char *const *names,
			 size_t count)
{
	if (chip != board_chip) {
		return;
	}
	for (size_t i = 0; i < count; i++) {
		STRUCT_SECTION_FOREACH(device, dev) {
			if (strcmp(dev->name, names[i]) == 0) {
				(void)device_init(dev);
			}
		}
	}
}

#define INIT_PLATFORM(n)                                                                           \
	IF_ENABLED(DT_INST_NODE_HAS_PROP(n, qcom_devices),                                         \
		   (init_devices(chip, DT_INST_PROP(n, qcom_chip_id), devices_##n,                 \
				 ARRAY_SIZE(devices_##n));))

static int sail_platform_devices_init(void)
{
	uint16_t chip = sail_chip_id_get();

	DT_INST_FOREACH_STATUS_OKAY(INIT_PLATFORM)
	return 0;
}

SYS_INIT(sail_platform_devices_init, PRE_KERNEL_1, CONFIG_SAIL_PLATFORM_DEVICES_INIT_PRIORITY);

#endif
