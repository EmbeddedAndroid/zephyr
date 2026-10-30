/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "reset_prepare.h"

#include "sail_check.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define RESET_ENTRY       0x0801d400U
#define IO_ERROR          (-77)
#define GUARD_ERROR       (-78)

struct event {
	char kind;
	uint32_t address;
	uint32_t value;
};

struct fixture {
	struct monaco_timer_io io;
	struct monaco_reset_prepare_result result;
	struct event events[32];
	uint32_t words[4];
	uint8_t calls;
	uint8_t reads;
	uint8_t writes;
	uint8_t fail_at;
	uint8_t retire_at;
	uint8_t drop_at;
	uint8_t corrupt_at;
	int error;
	bool after_effect;
	bool live;
	bool reenter;
};

static int guard(void *context);

static int configure(struct fixture *fixture)
{
	return monaco_reset_prepare(&fixture->io, RESET_ENTRY, &fixture->result, guard, fixture);
}

static void enter(struct fixture *fixture, char kind, uint32_t address, uint32_t value)
{
	unsigned char saved[sizeof(fixture->result)];

	sail_check(fixture->calls < ARRAY_SIZE(fixture->events));
	fixture->events[fixture->calls++] = (struct event){kind, address, value};
	if (fixture->reenter) {
		memcpy(saved, &fixture->result, sizeof(saved));
		sail_check(configure(fixture) == -EALREADY);
		sail_check(memcmp(saved, &fixture->result, sizeof(saved)) == 0);
	}
	if (fixture->calls == fixture->retire_at) {
		fixture->live = false;
	}
}

static int guard(void *context)
{
	struct fixture *fixture = context;

	enter(fixture, 'G', 0U, 0U);
	if (!fixture->live) {
		return GUARD_ERROR;
	}
	return fixture->calls == fixture->fail_at ? fixture->error : 0;
}

static unsigned int word_index(uint32_t address)
{
	static const uint32_t addresses[] = {0xf1149024U, 0xf1149020U, 0xf1149420U, 0xf0300044U};

	for (unsigned int i = 0; i < ARRAY_SIZE(addresses); ++i) {
		if (address == addresses[i]) {
			return i;
		}
	}
	sail_check(false);
	return 0;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fixture *fixture = context;
	unsigned int index = word_index(address);

	enter(fixture, 'R', address, fixture->words[index]);
	fixture->reads++;
	if (fixture->calls == fixture->fail_at && !fixture->after_effect) {
		return fixture->error;
	}
	*value = fixture->words[index];
	if (fixture->reads == fixture->corrupt_at) {
		*value ^= index == 1U ? 0x1000U : 1U;
	}
	return fixture->calls == fixture->fail_at ? fixture->error : 0;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct fixture *fixture = context;
	unsigned int index = word_index(address);

	sail_check(fixture->result.valid == 15U || fixture->result.valid == 31U);
	sail_check(index == 2U || index == 3U);
	sail_check(value == (index == 2U ? 0x0000cff8U : 0x00400ea0U));
	enter(fixture, 'W', address, value);
	fixture->writes++;
	if (fixture->calls == fixture->fail_at && !fixture->after_effect) {
		return fixture->error;
	}
	if (fixture->writes != fixture->drop_at) {
		fixture->words[index] = value;
	}
	return fixture->calls == fixture->fail_at ? fixture->error : 0;
}

static void initialize(struct fixture *fixture, unsigned int targets)
{
	memset(fixture, 0, sizeof(*fixture));
	fixture->io =
		(struct monaco_timer_io){.read32 = read32, .write32 = write32, .context = fixture};
	fixture->words[0] = 0x00020101U;
	fixture->words[1] = 0x00001002U;
	fixture->words[2] = (targets & 1U) != 0U ? 0x0000cff8U : 0x0000ffffU;
	fixture->words[3] = (targets & 2U) != 0U ? 0x00400ea0U : 0U;
	fixture->live = true;
	fixture->error = IO_ERROR;
}

static void no_retry(struct fixture *fixture)
{
	unsigned char saved[sizeof(fixture->result)];
	unsigned int calls = fixture->calls;

	memcpy(saved, &fixture->result, sizeof(saved));
	sail_check(configure(fixture) == -EALREADY);
	sail_check(fixture->calls == calls);
	sail_check(memcmp(saved, &fixture->result, sizeof(saved)) == 0);
}

int sail_model_test(void)
{
	struct fixture fixture;
	struct event default_events[32];
	unsigned int default_calls;

	for (unsigned int targets = 0; targets < 4U; ++targets) {
		initialize(&fixture, targets);
		fixture.reenter = true;
		sail_check(configure(&fixture) == 0);
		sail_check(fixture.result.stage == MONACO_RESET_PREPARE_COMPLETE);
		sail_check(fixture.result.valid == 63U);
		sail_check(fixture.result.stores_attempted == (targets ^ 3U));
		sail_check(fixture.result.stores_completed == (targets ^ 3U));
		sail_check(fixture.result.mask_readback == 0x0000cff8U);
		sail_check(fixture.result.vector_readback == 0x00400ea0U);
		sail_check(fixture.result.guards_attempted ==
		       2U * (fixture.reads + fixture.writes) + 1U);
		sail_check(fixture.result.guards_attempted == fixture.result.guards_completed);
		sail_check(fixture.calls == 3U * (fixture.reads + fixture.writes) + 1U);
		for (unsigned int i = 0; i + 1U < fixture.calls; i += 3U) {
			sail_check(fixture.events[i].kind == 'G');
			sail_check(fixture.events[i + 1U].kind != 'G');
			sail_check(fixture.events[i + 2U].kind == 'G');
		}
		sail_check(fixture.events[fixture.calls - 1U].kind == 'G');
		no_retry(&fixture);
	}

	initialize(&fixture, 0U);
	sail_check(configure(&fixture) == 0);
	default_calls = fixture.calls;
	memcpy(default_events, fixture.events, sizeof(default_events));
	sail_check(default_calls == 31U && fixture.reads == 8U && fixture.writes == 2U);
	/* Both full controls are read before the first store. */
	sail_check(default_events[7].address == 0xf1149420U && default_events[7].kind == 'R');
	sail_check(default_events[10].address == 0xf0300044U && default_events[10].kind == 'R');
	sail_check(default_events[13].address == 0xf1149420U && default_events[13].kind == 'W');

	for (unsigned int at = 1; at <= default_calls; ++at) {
		for (unsigned int after = 0; after < 2U; ++after) {
			for (unsigned int positive = 0; positive < 2U; ++positive) {
				initialize(&fixture, 0U);
				fixture.fail_at = at;
				fixture.after_effect = after != 0U;
				fixture.error = positive != 0U ? 1 : IO_ERROR;
				sail_check(configure(&fixture) ==
					   (positive != 0U ? -EIO : IO_ERROR));
				sail_check(fixture.result.stage != MONACO_RESET_PREPARE_COMPLETE);
				sail_check(fixture.calls ==
					   at + (default_events[at - 1U].kind != 'G'));
				no_retry(&fixture);
			}
		}
		initialize(&fixture, 0U);
		fixture.retire_at = at;
		sail_check(configure(&fixture) == GUARD_ERROR);
		sail_check(fixture.calls == at + (default_events[at - 1U].kind != 'G'));
		no_retry(&fixture);
	}
	/* A guard failure after the access cannot replace the first I/O error. */
	initialize(&fixture, 0U);
	fixture.fail_at = 2U;
	fixture.retire_at = 2U;
	sail_check(configure(&fixture) == IO_ERROR);
	sail_check(fixture.result.callback_status == IO_ERROR &&
	       fixture.result.guard_status == GUARD_ERROR);
	sail_check(fixture.result.valid == 0U);

	for (unsigned int index = 0; index < 4U; ++index) {
		initialize(&fixture, 0U);
		fixture.words[index] ^= index == 1U ? 0x1000U : 0x80000000U;
		sail_check(configure(&fixture) == -ESTALE && fixture.writes == 0U);
		sail_check(fixture.reads == 4U);
	}
	for (unsigned int read = 1; read <= 8U; ++read) {
		initialize(&fixture, 0U);
		fixture.corrupt_at = read;
		sail_check(configure(&fixture) != 0);
		no_retry(&fixture);
	}
	for (unsigned int write = 1; write <= 2U; ++write) {
		initialize(&fixture, 0U);
		fixture.drop_at = write;
		sail_check(configure(&fixture) == -EIO && fixture.writes == write);
		no_retry(&fixture);
	}

	sail_check(monaco_reset_prepare(NULL, RESET_ENTRY, NULL, NULL, NULL) == -EINVAL);
	for (unsigned int invalid = 0; invalid < 5U; ++invalid) {
		initialize(&fixture, 0U);
		if (invalid == 0U) {
			fixture.io.read32 = NULL;
		} else if (invalid == 1U) {
			fixture.io.write32 = NULL;
		}
		sail_check(monaco_reset_prepare(invalid == 2U ? NULL : &fixture.io,
					    invalid == 3U ? RESET_ENTRY + 32U : RESET_ENTRY,
					    &fixture.result, invalid == 4U ? NULL : guard,
					    &fixture) == -EINVAL);
		sail_check(fixture.calls == 0U);
		no_retry(&fixture);
	}
	puts("PASS reset preparation: dual preflight, conditional stores, final verification, "
	     "ordered guards, callback faults, retirement, reentry and one attempt");
	return 0;
}
