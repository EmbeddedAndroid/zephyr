/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "md_protocol.h"

enum sail_md_command sail_md_command_decode(uint32_t raw)
{
	switch (raw) {
	case UINT32_C(0xcc010000):
		return SAIL_MD_COMMAND_DDR_READY;
	case UINT32_C(0xcd050000):
		return SAIL_MD_COMMAND_WARM_RESET;
	case UINT32_C(0xcd060000):
		return SAIL_MD_COMMAND_GRACEFUL_SHUTDOWN;
	case UINT32_C(0xcd070000):
		return SAIL_MD_COMMAND_SOC_HR;
	case UINT32_C(0xcd0a0000):
		return SAIL_MD_COMMAND_SDI_NOTICE;
	case UINT32_C(0xcc020000):
		return SAIL_MD_COMMAND_CC020000;
	case UINT32_C(0xcc030000):
		return SAIL_MD_COMMAND_CC030000;
	case UINT32_C(0xcc040000):
		return SAIL_MD_COMMAND_CC040000;
	case UINT32_C(0xcc050000):
		return SAIL_MD_COMMAND_CC050000;
	case UINT32_C(0xcb000003):
		return SAIL_MD_COMMAND_XBL_BOOT_SUCCESS;
	case UINT32_C(0xcb000007):
		return SAIL_MD_COMMAND_SECURE_BOOT_NOTICE;
	case UINT32_C(0xcb00000f):
		return SAIL_MD_COMMAND_HYP_BOOT_NOTICE;
	default:
		return SAIL_MD_COMMAND_UNKNOWN;
	}
}
