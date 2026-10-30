/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_BOOT_MODE_H_
#define ZEPHYR_SOC_QCOM_SAIL_BOOT_MODE_H_

#include <stdbool.h>
#include <stdint.h>
#include <soc.h>

struct monaco_cold_boot;

/* Select the boot mode from this boot's cold boot result. @p boot must be the
 * object the cold boot ran on. SAIL_BOOT_PCINIT_DEFERRED is selected only when
 * every stage before PCInit completed; services that touch power or reset state
 * must check for it themselves. Without @p allow_pcinit_deferred any error
 * selects SAIL_BOOT_STOPPED.
 */
enum sail_boot_mode sail_boot_mode_select(const struct monaco_cold_boot *boot, int result,
					  bool allow_pcinit_deferred);

/* Return true when the boot timer may be used. @p mode must be the mode
 * selected for this boot; the deferred mode rechecks completion and timer state.
 */
bool sail_boot_timer_qualified(const struct monaco_cold_boot *boot, int result,
			       enum sail_boot_mode mode);

#endif
