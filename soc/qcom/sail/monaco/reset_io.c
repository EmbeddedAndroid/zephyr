/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "reset_io.h"
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stddef.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/sys_io.h>

int monaco_reset_read32(void *context, uint32_t address, uint32_t *value)
{
	ARG_UNUSED(context);
	if (value == NULL || (address != 0xf1149024U && address != 0xf1149020U &&
			      address != 0xf1149420U && address != 0xf0300044U)) {
		return -EINVAL;
	}
	barrier_dsync_fence_full();
	*value = sys_read32(address);
	barrier_dsync_fence_full();
	return 0;
}

int monaco_reset_write32(void *context, uint32_t address, uint32_t value)
{
	ARG_UNUSED(context);
	if (!((address == 0xf1149420U && value == 0x0000cff8U) ||
	      (address == 0xf0300044U && value == 0x00400ea0U))) {
		return -EPERM;
	}
	barrier_dsync_fence_full();
	sys_write32(value, address);
	barrier_dsync_fence_full();
	return 0;
}

int monaco_reset_reply(void *context, uint32_t answer, uint32_t index)
{
	ARG_UNUSED(context);
	if (index >= 4U || (answer != 0xaa030000U && answer != 0xaa0c0000U)) {
		return -EINVAL;
	}
	/* One byte per 32-bit register, low byte first; no doorbell write follows. */
	barrier_dsync_fence_full();
	sys_write32((answer >> (8U * index)) & 0xffU, 0xf0305094U + index * 4U);
	barrier_dsync_fence_full();
	return 0;
}
