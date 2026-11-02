/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cores.h"

#include <errno.h>
#include <zephyr/sys/util.h>

int sail_cores_native_read32(void *context, uint32_t address, uint32_t *value)
{
	ARG_UNUSED(context);
	if (value == NULL || address < UINT32_C(0xc0000000) || (address & 3U) != 0U) {
		return -EINVAL;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*value = *(volatile uint32_t *)(uintptr_t)address;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

int sail_cores_native_write32(void *context, uint32_t address, uint32_t value)
{
	ARG_UNUSED(context);
	if (!sail_cores_write_allowed(address)) {
		return -EPERM;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)address = value;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}
