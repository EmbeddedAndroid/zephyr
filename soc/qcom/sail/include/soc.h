/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_QCOM_SAIL_SOC_H_
#define ZEPHYR_SOC_QCOM_SAIL_SOC_H_

#define __GIC_PRESENT 0
#define __TIM_PRESENT 0

#ifndef _ASMLANGUAGE
#include <stdint.h>

enum sail_boot_mode {
	SAIL_BOOT_STOPPED = 0,
	SAIL_BOOT_NORMAL = 1,
	/* Applications run; PCInit is not done and its error result is kept.
	 * The value is a distinct pattern that is easy to spot in an SRAM dump.
	 */
	SAIL_BOOT_PCINIT_DEFERRED = 0x4f425356,
};

/** @brief Return the boot mode selected by the cold initialization. */
enum sail_boot_mode sail_boot_mode_get(void);

/**
 * @brief Return the cold initialization result.
 *
 * @return 0, or a negative cold initialization code (not an errno). In mode
 *	   SAIL_BOOT_PCINIT_DEFERRED this is always the PCInit-required code.
 */
int sail_boot_status(void);

/**
 * @brief Read the boot timer in microseconds.
 *
 * @retval 0 @p value holds the time.
 * @return A negative cold initialization or timer code (not errno).
 */
int sail_time_us(uint64_t *value);

/**
 * @brief Busy-wait on the boot timer.
 *
 * @retval 0 At least @p value microseconds passed.
 * @return A negative cold initialization or timer code (not errno).
 */
int sail_delay_us(uint32_t value);

#endif
#endif
