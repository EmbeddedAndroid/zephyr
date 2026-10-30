/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "md_protocol.h"

#include <zephyr/ztest.h>
#include <stddef.h>

static const struct {
	uint32_t word;
	enum sail_md_command command;
} known[] = {
	{UINT32_C(0xcb000003), SAIL_MD_COMMAND_XBL_BOOT_SUCCESS},
	{UINT32_C(0xcb000007), SAIL_MD_COMMAND_SECURE_BOOT_NOTICE},
	{UINT32_C(0xcb00000f), SAIL_MD_COMMAND_HYP_BOOT_NOTICE},
	{UINT32_C(0xcc010000), SAIL_MD_COMMAND_DDR_READY},
	{UINT32_C(0xcc020000), SAIL_MD_COMMAND_CC020000},
	{UINT32_C(0xcc030000), SAIL_MD_COMMAND_CC030000},
	{UINT32_C(0xcc040000), SAIL_MD_COMMAND_CC040000},
	{UINT32_C(0xcc050000), SAIL_MD_COMMAND_CC050000},
	{UINT32_C(0xcd050000), SAIL_MD_COMMAND_WARM_RESET},
	{UINT32_C(0xcd060000), SAIL_MD_COMMAND_GRACEFUL_SHUTDOWN},
	{UINT32_C(0xcd070000), SAIL_MD_COMMAND_SOC_HR},
	{UINT32_C(0xcd0a0000), SAIL_MD_COMMAND_SDI_NOTICE},
};

static enum sail_md_command expected(uint32_t word)
{
	for (size_t i = 0; i < ARRAY_SIZE(known); ++i) {
		if (known[i].word == word) {
			return known[i].command;
		}
	}
	return SAIL_MD_COMMAND_UNKNOWN;
}

ZTEST(sail_md_protocol, test_exact_words_and_payloads)
{
	/* Words that this subset decodes as unknown. */
	static const uint32_t other[] = {
		0,
		UINT32_MAX,
		UINT32_C(0xcd000000),
		UINT32_C(0xcd010000),
		UINT32_C(0xcd020001),
		UINT32_C(0xcd030000),
		UINT32_C(0xcb000001),
		UINT32_C(0xcb00001f),
		UINT32_C(0xcb00003f),
		UINT32_C(0xd0030000),
		UINT32_C(0xd0000000),
		UINT32_C(0xd0020000),
		UINT32_C(0xaa010000),
		UINT32_C(0xaa020000),
		UINT32_C(0xaa030000),
		UINT32_C(0xaa040000),
		UINT32_C(0xaa050000),
		UINT32_C(0xaa060000),
		UINT32_C(0xaa070000),
		UINT32_C(0xaa080000),
		UINT32_C(0xaa0c0000),
		UINT32_C(0xcc070000),
		UINT32_C(0xcd080000),
		UINT32_C(0xcd090000),
		UINT32_C(0x000001cc),
		UINT32_C(0x000007cd),
		UINT32_C(0xcc060000),
	};

	for (size_t i = 0; i < ARRAY_SIZE(known); ++i) {
		zassert_true(sail_md_command_decode(known[i].word) == known[i].command);
		for (unsigned int bit = 0; bit < 32; ++bit) {
			uint32_t mutated = known[i].word ^ (UINT32_C(1) << bit);

			/* Some single-bit changes select a different known word. */
			zassert_true(sail_md_command_decode(mutated) == expected(mutated));
		}
		for (uint32_t low = 1; low <= UINT16_MAX; ++low) {
			uint32_t word = (known[i].word & UINT32_C(0xffff0000)) | low;

			zassert_true(sail_md_command_decode(word) == expected(word));
		}
	}
	for (uint32_t high = 0; high <= UINT16_MAX; ++high) {
		uint32_t word = high << 16;

		zassert_true(sail_md_command_decode(word) == expected(word));
		/* D001xxxx words are outside this API's subset. */
		zassert_true(sail_md_command_decode(UINT32_C(0xd0010000) | high) ==
			     SAIL_MD_COMMAND_UNKNOWN);
	}
	for (size_t i = 0; i < ARRAY_SIZE(other); ++i) {
		zassert_true(sail_md_command_decode(other[i]) == SAIL_MD_COMMAND_UNKNOWN);
	}
}

ZTEST(sail_md_protocol, test_xbl_boot_success_exact_word)
{
	zassert_equal(sail_md_command_decode(UINT32_C(0xcb000003)),
		      SAIL_MD_COMMAND_XBL_BOOT_SUCCESS);
	for (unsigned int bit = 0; bit < 32; ++bit) {
		uint32_t word = UINT32_C(0xcb000003) ^ (UINT32_C(1) << bit);

		/* Bit 2 selects the secure boot notice; every other mutation is unknown. */
		zassert_equal(sail_md_command_decode(word), expected(word));
	}
	zassert_equal(sail_md_command_decode(UINT32_C(0x030000cb)), SAIL_MD_COMMAND_UNKNOWN);
	zassert_equal(sail_md_command_decode(UINT32_C(0x070000cb)), SAIL_MD_COMMAND_UNKNOWN);
	zassert_equal(sail_md_command_decode(UINT32_C(0x0f0000cb)), SAIL_MD_COMMAND_UNKNOWN);
}

ZTEST_SUITE(sail_md_protocol, NULL, NULL, NULL, NULL, NULL);
