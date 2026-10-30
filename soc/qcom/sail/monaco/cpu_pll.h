/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_CPU_PLL_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_CPU_PLL_H_

#include "clock.h"

bool monaco_cpu_pll_write_allowed(uint32_t address);
/* Checks, without writing, that PLL0 already runs at 600 MHz. */
int monaco_cpu_pll0_ready(struct monaco_clock *clock);
int monaco_cpu_pll0_enable(struct monaco_clock *clock);
int monaco_cpu_pll1_enable(struct monaco_clock *clock);
int monaco_cpu_pll2_enable(struct monaco_clock *clock);
int monaco_cpu_pll4_enable(struct monaco_clock *clock);

/*
 * Reapply PLL1's 1593.6 MHz configuration at the same rate. The CPU mux must
 * already select source 2 with divider encoding 2, and PLL1 must be locked at
 * that rate with outputs enabled; any other state is rejected without writes.
 */
int monaco_cpu_pll1_refresh(struct monaco_clock *clock);

/*
 * Raise PLL1 from 1593.6 to 1843.2 MHz while it drives the CPU. The PMIC
 * supplies must be programmed and settled, and the CPU mux must select
 * source 2 with divider encoding 2; any other PLL state is rejected without
 * writes. The caller reapplies the CPU mux on success. A failure partway
 * through is not rolled back.
 */
int monaco_cpu_pll1_raise(struct monaco_clock *clock);

#endif
