/*
 * Qualcomm SAIL R52 stub SoC.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <cmsis_core.h>
#include <zephyr/sys/barrier.h>

void soc_reset_hook(void)
{
	/* Enable the instruction cache as early as possible. The data
	 * cache is left to the MPU configuration / kernel.
	 */
	if (IS_ENABLED(CONFIG_ICACHE)) {
		if (!(__get_SCTLR() & SCTLR_I_Msk)) {
			L1C_InvalidateICacheAll();
			__set_SCTLR(__get_SCTLR() | SCTLR_I_Msk);
			barrier_isync_fence_full();
		}
	}
}
