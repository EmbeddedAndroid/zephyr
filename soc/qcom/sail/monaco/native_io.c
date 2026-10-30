/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "native_io.h"
#include <zephyr/sys/util.h>
#include "clock.h"
#include "cpu_pll.h"
#include "hbcu.h"
#include "i2c.h"
#include "i2c_profile.h"
#include "md_bist.h"
#include "md_publication.h"

#define QTMR_FREQUENCY UINT32_C(0xf140c000)
#define XO_HZ          UINT32_C(19200000)

static int device_address(uint32_t address)
{
	return address >= UINT32_C(0xc0000000) && (address & 3U) == 0;
}

static void ordered_access(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}

int monaco_native_read_mpidr(void *context, uint32_t *value)
{
	ARG_UNUSED(context);
	if (!value) {
		return -1;
	}
	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(*value));
	return 0;
}

int monaco_native_read_cpsr(void *context, uint32_t *value)
{
	ARG_UNUSED(context);
	if (!value) {
		return -1;
	}
	__asm__ volatile("mrs %0, cpsr" : "=r"(*value));
	return 0;
}

int monaco_native_read32(void *context, uint32_t address, uint32_t *value)
{
	ARG_UNUSED(context);
	if (!value || !device_address(address)) {
		return -1;
	}
	/* A synchronous bus fault is not recoverable through this return value. */
	ordered_access();
	*value = *(volatile uint32_t *)(uintptr_t)address;
	ordered_access();
	return 0;
}

int monaco_native_write32(void *context, uint32_t address, uint32_t value)
{
	ARG_UNUSED(context);
	if (!device_address(address)) {
		return -1;
	}
	if (address == QTMR_FREQUENCY) {
		if (value != XO_HZ) {
			return -1;
		}
	} else if (!md_bist_write_allowed(address) && !monaco_clock_write_allowed(address) &&
		   !monaco_cpu_pll_write_allowed(address) && !monaco_hbcu_write_allowed(address) &&
		   !monaco_i2c_write_allowed(address) &&
		   !monaco_i2c_profile_write_allowed(address)) {
		return -1;
	}
	ordered_access();
	*(volatile uint32_t *)(uintptr_t)address = value;
	ordered_access();
	return 0;
}

int monaco_native_md_status_write32(void *context, uint32_t address, uint32_t value)
{
	ARG_UNUSED(context);
	/* Status words are written only through the publisher, which orders them. */
	if (!md_publication_write_allowed(address, value)) {
		return -1;
	}
	ordered_access();
	*(volatile uint32_t *)(uintptr_t)address = value;
	ordered_access();
	return 0;
}
