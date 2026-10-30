/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/platform/hooks.h>
#include <zephyr/sys/util_macro.h>
#include <soc.h>
#include "boot_mode.h"
#include "cold_boot.h"
#include "native_io.h"

/* The early hook runs on z_arm_sys_stack, before the main thread exists. */
_Static_assert(CONFIG_ARMV7_SYS_STACK_SIZE >= 8192,
	       "cold initialization runs on the SYS stack and needs 8 KiB");

/* Transport callbacks and PMIC bindings retain pointers into this object. */
static struct monaco_cold_boot boot;
static int boot_result = MONACO_COLD_STATE;
static enum sail_boot_mode boot_mode = SAIL_BOOT_STOPPED;

void soc_early_init_hook(void)
{
	static const struct monaco_cold_boot_io io = {
		.read32 = monaco_native_read32,
		.write32 = monaco_native_write32,
		.read_mpidr = monaco_native_read_mpidr,
		.read_cpsr = monaco_native_read_cpsr,
		/* Keep the access protection left by the boot ROM; XPU and SID
		 * settings are not changed.
		 */
		.md_access_contract = MD_BIST_ACCESS_PBL_COLD_INHERITED_DEVICE,
		.md_publication = {
				.write32 = monaco_native_md_status_write32,
			},
	};

	boot_result = monaco_cold_boot_run(&boot, &io, &monaco_cold_boot_defaults);
	boot_mode = sail_boot_mode_select(&boot, boot_result,
					  IS_ENABLED(CONFIG_SAIL_START_WITHOUT_PCINIT));
	if (boot_mode == SAIL_BOOT_STOPPED) {
		/* Halt; the boot state stays in SRAM. */
		for (;;) {
			__asm__ volatile("dsb sy\n\twfi" ::: "memory");
		}
	}
}

enum sail_boot_mode sail_boot_mode_get(void)
{
	return boot_mode;
}

int sail_boot_status(void)
{
	return boot_result;
}

int sail_time_us(uint64_t *value)
{
	if (!sail_boot_timer_qualified(&boot, boot_result, boot_mode)) {
		return boot_result ? boot_result : MONACO_COLD_STATE;
	}
	return monaco_timer_time_us(&boot.timer, value);
}

int sail_delay_us(uint32_t value)
{
	if (!sail_boot_timer_qualified(&boot, boot_result, boot_mode)) {
		return boot_result ? boot_result : MONACO_COLD_STATE;
	}
	return monaco_timer_delay_us(&boot.timer, value);
}
