/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cpu_pll.h"

#include <zephyr/sys/util.h>

#define PLL0          UINT32_C(0xf1100000)
#define PLL1          UINT32_C(0xf1101000)
#define PLL2          UINT32_C(0xf1102000)
#define PLL4          UINT32_C(0xf1104000)
#define LOCK          UINT32_C(0x80000000)

struct setting {
	uint32_t offset, value;
};
struct match {
	uint32_t offset, mask, value;
};
struct pll0_profile {
	uint32_t base, l, fraction, divider_bits;
};

static const struct pll0_profile pll0 = {PLL0, 31, 0x4000, 0x400};
static const struct pll0_profile pll2 = {PLL2, 34, 0x9555, 0};
static const struct pll0_profile pll4 = {PLL4, 26, 0x0aaa, 0};

bool monaco_cpu_pll_write_allowed(uint32_t address)
{
	switch (address) {
	case PLL0:
	case PLL0 + 4:
	case PLL0 + 0x10:
	case PLL0 + 0x14:
	case PLL0 + 0x18:
	case PLL0 + 0x1c:
	case PLL0 + 0x20:
	case PLL0 + 0x24:
	case PLL0 + 0x28:
	case PLL0 + 0x2c:
	case PLL0 + 0x30:
	case PLL0 + 0x34:
	case PLL1:
	case PLL1 + 4:
	case PLL1 + 8:
	case PLL1 + 0xc:
	case PLL1 + 0x10:
	case PLL1 + 0x14:
	case PLL1 + 0x18:
	case PLL1 + 0x1c:
	case PLL1 + 0x20:
	case PLL1 + 0x24:
	case PLL1 + 0x28:
	case PLL1 + 0x2c:
	case PLL2:
	case PLL2 + 4:
	case PLL2 + 0x10:
	case PLL2 + 0x14:
	case PLL2 + 0x18:
	case PLL2 + 0x1c:
	case PLL2 + 0x20:
	case PLL2 + 0x24:
	case PLL2 + 0x28:
	case PLL2 + 0x2c:
	case PLL2 + 0x30:
	case PLL2 + 0x34:
	case PLL4:
	case PLL4 + 4:
	case PLL4 + 0x10:
	case PLL4 + 0x14:
	case PLL4 + 0x18:
	case PLL4 + 0x1c:
	case PLL4 + 0x20:
	case PLL4 + 0x24:
	case PLL4 + 0x28:
	case PLL4 + 0x2c:
	case PLL4 + 0x30:
	case PLL4 + 0x34:
		return true;
	default:
		return false;
	}
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
	if (!monaco_cpu_pll_write_allowed(address)) {
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

static int match_fields(struct monaco_clock *clock, uint32_t base, const struct match *fields,
			unsigned int count)
{
	int rc;
	uint32_t value;

	for (unsigned int i = 0; i < count; i++) {
		rc = read32(clock, base + fields[i].offset, &value);
		if (rc != 0) {
			return rc;
		}
		if ((value & fields[i].mask) != fields[i].value) {
			return MONACO_CLOCK_STATE;
		}
	}
	return 0;
}

static int matches_pll0(struct monaco_clock *clock, const struct pll0_profile *profile,
			uint32_t mode)
{
	const struct match fields[] = {
		{4, UINT32_MAX, 1},
		{0x10, 0xffff, profile->l},
		{0x14, 0xffff, profile->fraction},
		{0x18, 0x11c3fc01, profile->divider_bits | 1U},
		{0x1c, 0x10000, 0},
	};
	if ((mode & (LOCK | 5U)) != (LOCK | 5U)) {
		return MONACO_CLOCK_STATE;
	}
	return match_fields(clock, profile->base, fields, ARRAY_SIZE(fields));
}

static int matches_pll1(struct monaco_clock *clock, uint32_t mode, uint32_t l)
{
	const struct match fields[] = {
		{4, 0xff, l},
		{8, 0xffff, 0},
		{0xc, 1, 1},
		{0x10, 0x12023000, 0x02000000},
	};
	if ((mode & (LOCK | 7U)) != (LOCK | 7U)) {
		return MONACO_CLOCK_STATE;
	}
	return match_fields(clock, PLL1, fields, ARRAY_SIZE(fields));
}

static int initialize(struct monaco_clock *clock, uint32_t base, const struct setting *settings,
		      unsigned int count)
{
	int rc;

	for (unsigned int i = 0; i < count; i++) {
		rc = write32(clock, base + settings[i].offset, settings[i].value);
		if (rc != 0) {
			return rc;
		}
	}
	return 0;
}

static int wait_lock(struct monaco_clock *clock, uint32_t base, uint32_t count)
{
	int rc;
	uint32_t value;

	if (count > clock->max_poll_reads) {
		count = clock->max_poll_reads;
	}
	for (uint32_t i = 0; i < count; i++) {
		rc = read32(clock, base, &value);
		if (rc != 0) {
			return rc;
		}
		if (value & LOCK) {
			return 0;
		}
		if (monaco_timer_delay_us(clock->timer, 1)) {
			return MONACO_CLOCK_TIMEBASE;
		}
	}
	return MONACO_CLOCK_POLL_LIMIT;
}

static int pll0_family_enable(struct monaco_clock *clock, const struct pll0_profile *profile)
{
	int rc;
	static const struct setting settings[] = {
		{0x20, 0x20485699}, {0x24, 0x00182261}, {0x28, 0x32aa299c}, {0x2c, 0},
		{0x30, 0},          {0x34, 0},          {0x18, 0},          {0x1c, 0x00400805},
	};
	uint32_t mode, opmode, base = profile->base;

	rc = valid(clock);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, base, &mode);
	if (rc != 0) {
		return rc;
	}
	if ((mode & (LOCK | 4U)) == (LOCK | 4U)) {
		return matches_pll0(clock, profile, mode);
	}
	rc = read32(clock, base + 4, &opmode);
	if (rc != 0) {
		return rc;
	}
	if ((mode & (LOCK | 7U)) || opmode) {
		return MONACO_CLOCK_STATE;
	}
	rc = initialize(clock, base, settings, ARRAY_SIZE(settings));
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base + 0x10, 0xffff, profile->l);
	if (rc != 0) {
		return rc;
	}
	rc = write32(clock, base + 0x14, profile->fraction);
	if (rc != 0) {
		return rc;
	}
	/* Clears format bit 28 along with the divider and predivider fields. */
	rc = update(clock, base + 0x18, ~UINT32_C(0xee3c03ff), profile->divider_bits);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base + 0x1c, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base + 0x18, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, base, &mode);
	if (rc != 0) {
		return rc;
	}
	if (mode & (LOCK | 7U)) {
		return MONACO_CLOCK_STATE;
	}
	if (mode & 0x100U) {
		rc = update(clock, base + 0x10, 0xffff0000, 0x00440000);
		if (rc != 0) {
			return rc;
		}
	}
	rc = write32(clock, base + 4, 1);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base, 4, 4);
	if (rc != 0) {
		return rc;
	}
	rc = wait_lock(clock, base, 1001);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, base, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, base, &mode);
	if (rc != 0) {
		return rc;
	}
	return matches_pll0(clock, profile, mode);
}

int monaco_cpu_pll0_ready(struct monaco_clock *clock)
{
	int rc;
	uint32_t mode;

	rc = valid(clock);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, PLL0, &mode);
	if (rc != 0) {
		return rc;
	}
	return matches_pll0(clock, &pll0, mode);
}

int monaco_cpu_pll0_enable(struct monaco_clock *clock)
{
	return pll0_family_enable(clock, &pll0);
}

int monaco_cpu_pll2_enable(struct monaco_clock *clock)
{
	return pll0_family_enable(clock, &pll2);
}

int monaco_cpu_pll4_enable(struct monaco_clock *clock)
{
	return pll0_family_enable(clock, &pll4);
}

int monaco_cpu_pll1_enable(struct monaco_clock *clock)
{
	int rc;
	static const struct setting settings[] = {
		{0x14, 0x08200800}, {0x18, 0x05028001}, {0x1c, 0}, {0x20, 0},
		{0x24, 0},          {0x28, 0},          {0xc, 0},  {0x10, 0},
	};
	uint32_t mode, user;

	rc = valid(clock);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, PLL1, &mode);
	if (rc != 0) {
		return rc;
	}
	if ((mode & (LOCK | 6U)) == (LOCK | 6U)) {
		return matches_pll1(clock, mode, 83);
	}
	if (mode & (LOCK | 7U)) {
		return MONACO_CLOCK_STATE;
	}
	rc = initialize(clock, PLL1, settings, ARRAY_SIZE(settings));
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, PLL1 + 4, 0xff, 0x53);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, PLL1 + 0x10, &user);
	if (rc != 0) {
		return rc;
	}
	user = (user & UINT32_C(0xedfdcfff)) | UINT32_C(0x02000000);
	rc = update(clock, PLL1 + 8, 0xffff, 0);
	if (rc != 0) {
		return rc;
	}
	rc = write32(clock, PLL1 + 0x10, user);
	if (rc != 0) {
		return rc;
	}
	rc = update(clock, PLL1 + 0xc, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, PLL1, &mode);
	if (rc != 0) {
		return rc;
	}
	if (mode & (LOCK | 7U)) {
		return MONACO_CLOCK_STATE;
	}
	/* All three writes derive from the MODE value read above. */
	rc = write32(clock, PLL1, mode | 2U);
	if (rc != 0) {
		return rc;
	}
	rc = write32(clock, PLL1, mode | 6U);
	if (rc != 0) {
		return rc;
	}
	rc = write32(clock, PLL1 + 0x2c, 1);
	if (rc != 0) {
		return rc;
	}
	rc = wait_lock(clock, PLL1, 51);
	if (rc != 0) {
		return rc;
	}
	rc = write32(clock, PLL1, mode | 7U);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, PLL1, &mode);
	if (rc != 0) {
		return rc;
	}
	return matches_pll1(clock, mode, 83);
}

static int pll1_update_rate(struct monaco_clock *clock, uint32_t l)
{
	int rc;
	uint32_t user;

	rc = update(clock, PLL1 + 4, 0xff, l);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, PLL1 + 0x10, &user);
	if (rc != 0) {
		return rc;
	}
	user = (user & ~UINT32_C(0x10000000)) | UINT32_C(0x02000000);
	rc = update(clock, PLL1 + 8, 0xffff, 0);
	if (rc != 0) {
		return rc;
	}
	rc = write32(clock, PLL1 + 0x10, user);
	if (rc != 0) {
		return rc;
	}
	if (monaco_timer_delay_us(clock->timer, 1)) {
		return MONACO_CLOCK_TIMEBASE;
	}
	return wait_lock(clock, PLL1, 51);
}

int monaco_cpu_pll1_refresh(struct monaco_clock *clock)
{
	int rc;
	uint32_t mode;

	rc = valid(clock);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, PLL1, &mode);
	if (rc != 0) {
		return rc;
	}
	rc = matches_pll1(clock, mode, 83);
	if (rc != 0) {
		return rc;
	}
	/* The caller has already updated the CPU mux, so the same-rate update is safe. */
	return pll1_update_rate(clock, 83);
}

int monaco_cpu_pll1_raise(struct monaco_clock *clock)
{
	int rc;
	uint32_t mode;

	rc = valid(clock);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, PLL1, &mode);
	if (rc != 0) {
		return rc;
	}
	rc = matches_pll1(clock, mode, 83);
	if (rc != 0) {
		return rc;
	}
	rc = pll1_update_rate(clock, 96);
	if (rc != 0) {
		return rc;
	}
	rc = read32(clock, PLL1, &mode);
	if (rc != 0) {
		return rc;
	}
	return matches_pll1(clock, mode, 96);
}
