/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MD_PROTOCOL_H_
#define ZEPHYR_SOC_QCOM_SAIL_MD_PROTOCOL_H_

#include <stdint.h>

/* Recognized messages; these are not the wire values. */
enum sail_md_command {
	SAIL_MD_COMMAND_UNKNOWN = 0,
	SAIL_MD_COMMAND_DDR_READY,
	SAIL_MD_COMMAND_WARM_RESET,
	SAIL_MD_COMMAND_GRACEFUL_SHUTDOWN,
	SAIL_MD_COMMAND_SOC_HR,
	SAIL_MD_COMMAND_SDI_NOTICE,
	SAIL_MD_COMMAND_CC020000,
	SAIL_MD_COMMAND_CC030000,
	SAIL_MD_COMMAND_CC040000,
	SAIL_MD_COMMAND_CC050000,
	SAIL_MD_COMMAND_XBL_BOOT_SUCCESS,
	SAIL_MD_COMMAND_SECURE_BOOT_NOTICE,
	SAIL_MD_COMMAND_HYP_BOOT_NOTICE,
};

/*
 * Classify one received word by exact 32-bit match. The caller assembles the
 * four low register bytes little-endian. Other words, including zero, return
 * SAIL_MD_COMMAND_UNKNOWN. Both channels decode the same way; the receiver
 * tracks which channel a word came from. Decoding does not acknowledge.
 */
enum sail_md_command sail_md_command_decode(uint32_t raw);

#endif
