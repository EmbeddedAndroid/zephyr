/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "prepare.h"
#include "sail_check.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define HW_VER UINT32_C(0xf0300028)
#define CFG63  UINT32_C(0xf013f000)
#define CFG66  UINT32_C(0xf0142000)

struct fake {
	uint32_t hw_ver, cfg63, cfg66;
	/* 1-based index of the access that fails; 0 for none. */
	unsigned int fail_at, accesses;
	/* Bits the pad register does not keep. */
	uint32_t stuck_clear;
	struct {
		char kind;
		uint32_t address, value;
	} log[8];
};

static uint32_t *cell(struct fake *f, uint32_t address)
{
	switch (address) {
	case HW_VER:
		return &f->hw_ver;
	case CFG63:
		return &f->cfg63;
	case CFG66:
		return &f->cfg66;
	default:
		return NULL;
	}
}

static int access(struct fake *f, char kind, uint32_t address, uint32_t value)
{
	unsigned int n = f->accesses++;

	sail_check(n < ARRAY_SIZE(f->log));
	f->log[n].kind = kind;
	f->log[n].address = address;
	f->log[n].value = value;
	return f->fail_at == n + 1U || cell(f, address) == NULL ? -1 : 0;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;
	int rc = access(f, 'R', address, 0);

	if (rc == 0) {
		*value = *cell(f, address);
	}
	return rc;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;
	int rc = access(f, 'W', address, value);

	sail_check(lemans_prepare_write_allowed(address));
	if (rc == 0) {
		*cell(f, address) = value & ~f->stuck_clear;
	}
	return rc;
}

static void reset(struct fake *f)
{
	memset(f, 0, sizeof(*f));
	f->hw_ver = UINT32_C(0x60190200);
	f->cfg63 = UINT32_C(0x5);
	f->cfg66 = UINT32_C(0x1);
}

static void release_sequence(void)
{
	struct fake f;
	const struct monaco_timer_io io = {&f, read32, write32};

	reset(&f);
	sail_check(lemans_prepare_run(&io) == 0);
	sail_check(f.accesses == 5);
	sail_check(f.log[0].kind == 'R' && f.log[0].address == HW_VER);
	sail_check(f.log[1].kind == 'W' && f.log[1].address == CFG63 && f.log[1].value == 0x3);
	sail_check(f.log[2].kind == 'R' && f.log[2].address == CFG63);
	sail_check(f.log[3].kind == 'W' && f.log[3].address == CFG66 && f.log[3].value == 0x204);
	sail_check(f.log[4].kind == 'R' && f.log[4].address == CFG66);
	sail_check(f.cfg63 == 0x3 && f.cfg66 == 0x204);
	/* Fields above the configuration (EGPIO) do not affect the readback. */
	reset(&f);
	f.cfg63 = UINT32_C(0x1805);
	sail_check(lemans_prepare_run(&io) == 0);
}

static void refusals(void)
{
	struct fake f;
	const struct monaco_timer_io io = {&f, read32, write32};
	const struct monaco_timer_io no_write = {&f, read32, NULL};

	sail_check(lemans_prepare_run(NULL) == LEMANS_PREPARE_INVALID);
	sail_check(lemans_prepare_run(&no_write) == LEMANS_PREPARE_INVALID);
	/* Monaco (0x601a) and any other SoC: no pad is touched. */
	for (unsigned int i = 0; i < 3; i++) {
		static const uint32_t ids[] = {0x601a0100, 0x60180000, 0};

		reset(&f);
		f.hw_ver = ids[i];
		sail_check(lemans_prepare_run(&io) == LEMANS_PREPARE_IDENTITY);
		sail_check(f.accesses == 1 && f.cfg63 == 0x5 && f.cfg66 == 0x1);
	}
	/* A failure at each access stops the sequence there. */
	for (unsigned int at = 1; at <= 5; at++) {
		reset(&f);
		f.fail_at = at;
		sail_check(lemans_prepare_run(&io) == LEMANS_PREPARE_IO);
		sail_check(f.accesses == at);
	}
	/* A pad that does not keep its configuration is reported. */
	reset(&f);
	f.stuck_clear = UINT32_C(0x200);
	sail_check(lemans_prepare_run(&io) == LEMANS_PREPARE_PAD && f.accesses == 5);
	sail_check(!lemans_prepare_write_allowed(HW_VER));
	sail_check(!lemans_prepare_write_allowed(CFG63 + 4U));
	sail_check(!lemans_prepare_write_allowed(UINT32_C(0xf0137000)));
}

int sail_model_test(void)
{
	release_sequence();
	refusals();
	puts("Lemans prepare: identity, PSAIL_ERR_N and SLP_EN pads, faults and refusals passed");
	return 0;
}
