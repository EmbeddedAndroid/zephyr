/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SAIL_TEST_PLATFORM_H_
#define SAIL_TEST_PLATFORM_H_

#include "chip.h"
#include "platform.h"
#include "pmic_boot.h"

/* SAIL_TEST_LEMANS builds the chip-dependent models for Lemans. */
#if defined(SAIL_TEST_LEMANS)
#define SAIL_TEST_CHIP (&sail_chip_lemans)
#else
#define SAIL_TEST_CHIP (&sail_chip_monaco)
#endif

/* The VENTUNO Q rails, as ventuno_q_sail.dts describes them. */
extern const struct monaco_pmic_regulator_policy
	sail_test_ventuno_q_rails[MONACO_PMIC_BOOT_REGULATORS];

/* The rail arguments of monaco_pmic_policy_select() for the VENTUNO Q. */
#define SAIL_TEST_RAILS sail_test_ventuno_q_rails, MONACO_PMIC_BOOT_REGULATORS

/* VENTUNO Q (Monaco, SAIL PMIC) and IQ-9075 EVK (Lemans, no SAIL PMIC). */
extern const struct sail_platforms sail_test_platforms;

#endif
