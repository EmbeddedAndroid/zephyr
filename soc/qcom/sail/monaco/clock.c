/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "clock.h"

#include <zephyr/sys/util.h>

#define PLL5          UINT32_C(0xf1105000)
#define PLL6          UINT32_C(0xf1106000)
#define DEMET_CMD     UINT32_C(0xf111d008)
#define DEMET_CBCR    UINT32_C(0xf111d020)
#define SE0_CMD       UINT32_C(0xf1115040)
#define SE0_DFS       UINT32_C(0xf1115054)
#define SE0_CBCR      UINT32_C(0xf1115038)
#define PLL_ON        UINT32_C(0x80000004)
#define RCG_FIELDS    UINT32_C(0x371f)
#define UPDATE_READS  UINT32_C(150000)
#define LOCK_READS    UINT32_C(1501)

static bool cbcr_allowed(uint32_t address)
{
	switch (address) {
	case 0xf1110008:
	case 0xf111000c:
	case 0xf1110010:
	case 0xf1110014:
	case 0xf1110018:
	case 0xf1110034:
	case 0xf1110050:
	case 0xf1111004:
	case 0xf111100c:
	case 0xf1111010:
	case 0xf1111014:
	case 0xf1111018:
	case 0xf111101c:
	case 0xf1111038:
	case 0xf1111054:
	case 0xf1111070:
	case 0xf1111090:
	case 0xf1111094:
	case 0xf1113004:
	case 0xf111300c:
	case 0xf1114030:
	case 0xf1115004:
	case 0xf1115008:
	case 0xf111500c:
	case 0xf1115018:
	case SE0_CBCR:
	case 0xf111b004:
	case 0xf111d020:
	case 0xf111e004:
	case 0xf1122004:
	case 0xf112300c:
	case 0xf1123014:
	case 0xf1124018:
	case 0xf1124020:
	case 0xf1124028:
	case 0xf112402c:
	case 0xf1124030:
	case 0xf1124034:
	case 0xf1124038:
	case 0xf1124040:
	case 0xf1124044:
	case 0xf1128008:
	case 0xf112a000:
	case 0xf112a004:
	case 0xf112a008:
	case 0xf112a00c:
	case 0xf112a010:
	case 0xf112a014:
	case 0xf112a018:
	case 0xf112a01c:
	case 0xf113201c:
	case 0xf1132038:
	case 0xf113203c:
	case 0xf113301c:
	case 0xf1133038:
	case 0xf113303c:
	case 0xf1134000:
	case 0xf1134008:
	case 0xf1134010:
	case 0xf1134018:
	case 0xf1134020:
	case 0xf1134028:
	case 0xf1136004:
	case 0xf1136008:
	case 0xf1137000:
		return true;
	default:
		return false;
	}
}

static bool rcg_allowed(uint32_t command)
{
	switch (command) {
	case 0xf111001c:
	case 0xf1110038:
	case 0xf1111020:
	case 0xf111103c:
	case 0xf1111058:
	case 0xf1111074:
	case 0xf1114054:
	case 0xf1114084:
	case 0xf1115020:
	case SE0_CMD:
	case 0xf111d008:
	case 0xf112301c:
	case 0xf1124000:
	case 0xf112a020:
	case 0xf1132004:
	case 0xf1132020:
	case 0xf1133004:
	case 0xf1133020:
	case 0xf1137008:
		return true;
	default:
		return false;
	}
}

static bool pll_offset_allowed(uint32_t offset)
{
	switch (offset) {
	case 0:
	case 4:
	case 0x10:
	case 0x14:
	case 0x18:
	case 0x1c:
	case 0x20:
	case 0x24:
	case 0x28:
	case 0x2c:
	case 0x30:
	case 0x34:
	case 0x38:
	case 0x3c:
		return true;
	default:
		return false;
	}
}

bool monaco_clock_write_allowed(uint32_t address)
{
	return cbcr_allowed(address) || rcg_allowed(address) ||
	       (address >= 4 && rcg_allowed(address - 4)) ||
	       (address >= PLL5 && pll_offset_allowed(address - PLL5)) ||
	       (address >= PLL6 && pll_offset_allowed(address - PLL6));
}

static int valid(const struct monaco_clock *clock)
{
	return clock && clock->io.read32 && clock->io.write32 && clock->timer &&
			       clock->timer->initialized && clock->max_poll_reads
		       ? 0
		       : MONACO_CLOCK_INVALID;
}

static int read32(struct monaco_clock *clock, uint32_t address, uint32_t *value)
{
	return clock->io.read32(clock->io.context, address, value) ? MONACO_CLOCK_IO : 0;
}

static int write32(struct monaco_clock *clock, uint32_t address, uint32_t value)
{
	if (!monaco_clock_write_allowed(address)) {
		return MONACO_CLOCK_INVALID;
	}
	return clock->io.write32(clock->io.context, address, value) ? MONACO_CLOCK_IO : 0;
}

static int update(struct monaco_clock *clock, uint32_t address, uint32_t mask, uint32_t value)
{
	int rc;
	uint32_t old;

	rc = read32(clock, address, &old);
	if (rc != 0) {
		return rc;
	}
	return write32(clock, address, (old & ~mask) | value);
}

static int delay(struct monaco_clock *clock)
{
	return monaco_timer_delay_us(clock->timer, 1) ? MONACO_CLOCK_TIMEBASE : 0;
}

static int wait_bits(struct monaco_clock *clock, uint32_t address, uint32_t mask, uint32_t expected,
		     uint32_t limit, bool microsecond_delay)
{
	int rc;
	uint32_t value;

	if (limit > clock->max_poll_reads) {
		limit = clock->max_poll_reads;
	}
	for (uint32_t i = 0; i < limit; i++) {
		rc = read32(clock, address, &value);
		if (rc != 0) {
			return rc;
		}
		if ((value & mask) == expected) {
			return 0;
		}
		if (microsecond_delay) {
			rc = delay(clock);
			if (rc != 0) {
				return rc;
			}
		}
	}
	return MONACO_CLOCK_POLL_LIMIT;
}

int monaco_clock_cbcr_enable(struct monaco_clock *clock, uint32_t address)
{
	int rc;
	uint32_t value;

	rc = valid(clock);
	if (rc != 0) {
		return rc;
	}
	if (!cbcr_allowed(address)) {
		return MONACO_CLOCK_INVALID;
	}
	rc = update(clock, address, 1, 1);
	if (rc != 0) {
		return rc;
	}
	for (uint32_t i = 0; i < clock->max_poll_reads; i++) {
		rc = read32(clock, address, &value);
		if (rc != 0) {
			return rc;
		}
		if ((value & 2) || !(value & UINT32_C(0xd0000000))) {
			return 0;
		}
		rc = delay(clock);
		if (rc != 0) {
			return rc;
		}
	}
	return MONACO_CLOCK_POLL_LIMIT;
}

int monaco_clock_rcg_update(struct monaco_clock *clock, uint32_t command, uint8_t source,
			    uint8_t divider_encoding)
{
	int rc;
	uint32_t value, fields;

	rc = valid(clock);
	if (rc != 0) {
		return rc;
	}
	if (!rcg_allowed(command) || source > 7 || !divider_encoding || divider_encoding > 32) {
		return MONACO_CLOCK_INVALID;
	}
	if (command == SE0_CMD) {
		rc = read32(clock, SE0_DFS, &value);
		if (rc != 0) {
			return rc;
		}
		if (value & 1U) {
			return MONACO_CLOCK_STATE;
		}
	}
	fields = ((uint32_t)source << 8) | ((uint32_t)divider_encoding - 1);
	rc = wait_bits(clock, command, 1, 0, UPDATE_READS, false);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, command + 4, &value);
	if (rc != 0) {
		return rc;
	}
	rc = write32(clock, command + 4, (value & ~RCG_FIELDS) | fields);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, command, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = wait_bits(clock, command, 1, 0, UPDATE_READS, false);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, command + 4, &value);
	if (rc != 0) {
		return rc;
	}
	return (value & RCG_FIELDS) == fields ? 0 : MONACO_CLOCK_STATE;
}

int monaco_clock_se0_enable(struct monaco_clock *clock)
{
	int rc;

	rc = monaco_clock_rcg_update(clock, SE0_CMD, 0, 2);
	if (rc != 0) {
		return rc;
	}
	return monaco_clock_cbcr_enable(clock, SE0_CBCR);
}

struct pll_profile {
	uint32_t base;
	uint32_t trim_address;
	uint32_t fuse_address;
	uint8_t trim_shift;
	uint8_t first_fuse_shift;
	uint8_t second_fuse_shift;
};

static const struct pll_profile pll5 = {
	.base = PLL5,
	.trim_address = 0xf870418c,
	.fuse_address = 0xf8704190,
	.trim_shift = 5,
	.first_fuse_shift = 17,
	.second_fuse_shift = 9,
};

static const struct pll_profile pll6 = {
	.base = PLL6,
	.trim_address = 0xf8704194,
	.fuse_address = 0xf8704198,
	.trim_shift = 0,
	.first_fuse_shift = 12,
	.second_fuse_shift = 4,
};

static int active_pll_matches(struct monaco_clock *clock, uint32_t base, uint32_t mode)
{
	int rc;
	static const struct {
		uint32_t offset, mask, expected;
	} checks[] = {
		{4, UINT32_MAX, 1},
		{0x10, UINT32_MAX, 2},
		{0x14, 0x7c3, 0x403},
		{0x18, 0x300000, 0x200000},
	};
	uint32_t value;

	if ((mode & (PLL_ON | 0x1201U)) != (PLL_ON | 0x1001U)) {
		return MONACO_CLOCK_STATE;
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(checks); i++) {
		rc = read32(clock, base + checks[i].offset, &value);
		if (rc != 0) {
			return rc;
		}
		if ((value & checks[i].mask) != checks[i].expected) {
			return MONACO_CLOCK_STATE;
		}
	}
	return 0;
}

static int pll_enable(struct monaco_clock *clock, const struct pll_profile *profile)
{
	int rc;
	static const struct {
		uint32_t offset, value;
	} initial[] = {
		{0x1c, 0x60004f61}, {0x20, 0x0001c808}, {0x24, 0},          {0x28, 0x040001ac},
		{0x2c, 0},          {0x30, 0x0081c6de}, {0x34, 0x40100000}, {0x38, 0x440001b2},
		{0x3c, 0x3d8},      {0x14, 0},          {0x18, 0x00250002},
	};
	const uint32_t base = profile->base;
	uint32_t mode, opmode, trim, fuse = 0;

	rc = valid(clock);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, base, &mode);
	if (rc != 0) {
		return rc;
	}
	if ((mode & PLL_ON) == PLL_ON) {
		return active_pll_matches(clock, base, mode);
	}
	rc = read32(clock, base + 4, &opmode);
	if (rc != 0) {
		return rc;
	}
	/* A partly enabled or transitioning PLL cannot be reprogrammed. */
	if ((mode & UINT32_C(0x80001205)) || opmode) {
		return MONACO_CLOCK_STATE;
	}
	rc = read32(clock, profile->trim_address, &trim);
	if (rc != 0) {
		return rc;
	}
	trim = (trim >> profile->trim_shift) & 0xffffU;
	if (trim) {
		/* Read the fuse word once, before any PLL write. */
		rc = read32(clock, profile->fuse_address, &fuse);
		if (rc != 0) {
			return rc;
		}
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(initial); i++) {
		rc = write32(clock, base + initial[i].offset, initial[i].value);
		if (rc != 0) {
			return rc;
		}
	}
	if (trim) {
		rc = update(clock, base + 0x28, 0xffff, trim);
		if (rc != 0) {
			return rc;
		}
		rc = update(clock, base + 0x38, 1, 1);
		if (rc != 0) {
			return rc;
		}
		rc = update(clock, base + 0x34, 0x100, 0x100);
		if (rc != 0) {
			return rc;
		}
		rc = update(clock, base + 0x34, 0x3e00,
			    (fuse >> profile->first_fuse_shift) & 0x3800);
		if (rc != 0) {
			return rc;
		}
		rc = update(clock, base + 0x34, 0x7c000,
			    (fuse >> profile->second_fuse_shift) & 0x70000);
		if (rc != 0) {
			return rc;
		}
	}
	rc = update(clock, base + 0x18, 0x300000, 0x200000);
	if (rc != 0) {
		return rc;
	}
	rc = write32(clock, base + 0x10, 2);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base, 1, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base, 0x400, 0x400);
	if (rc != 0) {
		return rc;
	}
	rc = wait_bits(clock, base, 0x200, 0, UPDATE_READS, false);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base + 0x14, 0x7c1, 0x401);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base + 0x14, 2, 2);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base + 0x14, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = write32(clock, base + 4, 1);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base, 4, 4);
	if (rc != 0) {
		return rc;
	}
	rc = wait_bits(clock, base, 0x80000000, 0x80000000, LOCK_READS, true);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base + 0x18, 0x400, 0x400);
	if (rc != 0) {
		return rc;
	}
	rc = delay(clock);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base + 0x18, 0x400, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base, 0x1000, 0x1000);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, base, &mode);
	if (rc != 0) {
		return rc;
	}
	return active_pll_matches(clock, base, mode);
}

int monaco_clock_pll5_enable(struct monaco_clock *clock)
{
	return pll_enable(clock, &pll5);
}

int monaco_clock_pll6_enable(struct monaco_clock *clock)
{
	return pll_enable(clock, &pll6);
}

int monaco_clock_demet_enable(struct monaco_clock *clock)
{
	int rc;

	rc = monaco_clock_pll5_enable(clock);
	if (rc != 0) {
		return rc;
	}
	rc = monaco_clock_rcg_update(clock, DEMET_CMD, 3, 2);
	if (rc != 0) {
		return rc;
	}
	return monaco_clock_cbcr_enable(clock, DEMET_CBCR);
}
