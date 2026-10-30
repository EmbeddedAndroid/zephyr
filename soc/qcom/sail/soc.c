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
#if defined(CONFIG_SAIL_RESET_SERVICE)
#include "reset_service_internal.h"
#include "monaco/reset_io.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#endif

/* The early hook runs on z_arm_sys_stack, before the main thread exists. */
_Static_assert(CONFIG_ARMV7_SYS_STACK_SIZE >= 8192,
	       "cold initialization runs on the SYS stack and needs 8 KiB");

/* Transport callbacks and PMIC bindings retain pointers into this object. */
static struct monaco_cold_boot boot;
static int boot_result = MONACO_COLD_STATE;
static enum sail_boot_mode boot_mode = SAIL_BOOT_STOPPED;

#if defined(CONFIG_SAIL_RESET_SERVICE)
/* Reset attempt and transport state must live as long as the boot state. */
static struct sail_reset_service reset_service;
#endif

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
#if defined(CONFIG_SAIL_RESET_SERVICE)
	int rc = sail_reset_timer_enter(&reset_service);

	if (rc != 0) {
		return rc;
	}
	rc = monaco_timer_time_us(&boot.timer, value);
	sail_reset_timer_leave(&reset_service);
	return rc;
#else
	return monaco_timer_time_us(&boot.timer, value);
#endif
}

int sail_delay_us(uint32_t value)
{
	if (!sail_boot_timer_qualified(&boot, boot_result, boot_mode)) {
		return boot_result ? boot_result : MONACO_COLD_STATE;
	}
#if defined(CONFIG_SAIL_RESET_SERVICE)
	int rc = sail_reset_timer_enter(&reset_service);

	if (rc != 0) {
		return rc;
	}
	rc = monaco_timer_delay_us(&boot.timer, value);
	sail_reset_timer_leave(&reset_service);
	return rc;
#else
	return monaco_timer_delay_us(&boot.timer, value);
#endif
}

#if defined(CONFIG_SAIL_RESET_SERVICE)
/* entry.ld independently binds this symbol to the ELF reset entry. */
extern void sail_reset(void);

int sail_reset_service_start(int (*retire)(void *context), void *context)
{
	static const struct monaco_timer_io controls = {
		.read32 = monaco_reset_read32,
		.write32 = monaco_reset_write32,
	};

	return sail_reset_start(&reset_service, &boot, &boot_result, &boot_mode,
				DEVICE_DT_GET(DT_NODELABEL(sail_mbox_rx)), &controls,
				monaco_reset_reply, (uint32_t)(uintptr_t)sail_reset, retire,
				context);
}

int sail_reset_service_poll(void)
{
	return sail_reset_poll(&reset_service);
}

int sail_reset_service_stop(void)
{
	return sail_reset_stop(&reset_service);
}
#endif
