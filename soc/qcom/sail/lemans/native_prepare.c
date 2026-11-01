/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "prepare.h"
#include "native_io.h"
#include <zephyr/sys/util.h>

static int write32(void *context, uint32_t address, uint32_t value)
{
	ARG_UNUSED(context);
	if (!lemans_prepare_write_allowed(address)) {
		return -1;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)address = value;
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

int lemans_prepare(void)
{
	static const struct monaco_timer_io io = {
		.read32 = monaco_native_read32,
		.write32 = write32,
	};

	return lemans_prepare_run(&io);
}
