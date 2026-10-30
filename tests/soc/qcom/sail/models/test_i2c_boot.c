/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "i2c_boot.h"

#include "sail_check.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define SE            UINT32_C(0xf1980000)
#define CMD           UINT32_C(0xf1115040)
#define CFG           UINT32_C(0xf1115044)
#define CBCR          UINT32_C(0xf1115038)
#define DFS           UINT32_C(0xf1115054)

struct reg {
	uint32_t address, value;
};
struct access {
	uint32_t address, value;
	enum monaco_i2c_boot_stage stage;
	bool write;
};
struct fake {
	struct reg registers[128];
	struct access accesses[1024];
	unsigned int register_count, count, fail_at, writes, high_reads;
	unsigned int profile_writes, clock_writes, fifo_writes, cbcr_reads;
	uint64_t ticks;
	int error;
	bool recording, hold_cmd, hold_cbcr, ignore_cfg, stop_timer, write_before_error;
	bool override_read;
	enum monaco_i2c_boot_stage override_stage;
	uint32_t override_address, override_value;
	struct monaco_i2c_boot_status *status;
};

static const struct monaco_i2c_boot_qualification qualified = {
	.board = MONACO_I2C_PROFILE_VENTUNO_Q,
	.exclusive_ownership = true,
	.parent_clocks_qualified = true,
	.reset_state_qualified = true,
	.board_state_qualified = true,
};

static uint32_t *reg(struct fake *f, uint32_t address)
{
	for (unsigned int i = 0; i < f->register_count; i++) {
		if (f->registers[i].address == address) {
			return &f->registers[i].value;
		}
	}
	sail_check(f->register_count < ARRAY_SIZE(f->registers));
	f->registers[f->register_count].address = address;
	return &f->registers[f->register_count++].value;
}

static bool record(struct fake *f, uint32_t address, uint32_t value, bool write)
{
	if (!f->recording) {
		return false;
	}
	sail_check(f->status && f->count < ARRAY_SIZE(f->accesses));
	if (f->count) {
		sail_check(f->accesses[f->count - 1].stage <= f->status->stage);
	}
	f->accesses[f->count++] = (struct access){address, value, f->status->stage, write};
	return f->count == f->fail_at;
}

static bool timer_address(uint32_t address)
{
	return address == 0xf1400000 || address == 0xf1400004 || address == 0xf1400010;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;
	uint32_t sampled;

	if (address == 0xf1400010) {
		sampled = 19200000;
	} else if (address == 0xf1400000) {
		sampled = (uint32_t)f->ticks;
	} else if (address == 0xf1400004) {
		sampled = (uint32_t)(f->ticks >> 32);
		if (++f->high_reads % 2 == 0 && !f->stop_timer) {
			f->ticks += 24;
		}
	} else {
		sail_check(address == CMD || address == CFG || address == CBCR || address == DFS ||
		       address == 0xf19c0004 || address == 0xf19c0120 || address == 0xf19c0118 ||
		       address == 0xf0100000 || address == 0xf0101000 ||
		       (address >= SE && address < SE + 0x3000));
		if (address == CMD && !f->hold_cmd) {
			*reg(f, address) &= ~1U;
		}
		if (address == CBCR && (*reg(f, address) & 1U)) {
			/* Make the real clock helper exercise its bounded timer path. */
			if (++f->cbcr_reads > 1 && !f->hold_cbcr) {
				*reg(f, address) &= ~UINT32_C(0xd0000000);
			}
		}
		if (address == 0xf19c0004) {
			sail_check((*reg(f, CBCR) & 0xd0000001U) == 1);
			sail_check((*reg(f, CFG) & 0x371fU) == 1);
		}
		sampled = *reg(f, address);
	}
	if (f->override_read && f->status && f->status->stage == f->override_stage &&
	    address == f->override_address) {
		sampled = f->override_value;
	}
	*value = sampled;
	if (record(f, address, sampled, false)) {
		*value = UINT32_MAX;
		return f->error;
	}
	return 0;
}

static void apply_write(struct fake *f, uint32_t address, uint32_t value)
{
	f->writes++;
	switch (f->status->stage) {
	case MONACO_I2C_BOOT_CLOCK:
		sail_check(address == CMD || address == CFG || address == CBCR);
		sail_check(!f->profile_writes && !f->fifo_writes);
		f->clock_writes++;
		break;
	case MONACO_I2C_BOOT_PROFILE:
		sail_check(f->clock_writes == 3 && f->fifo_writes == 0);
		sail_check(monaco_i2c_profile_write_allowed(address));
		f->profile_writes++;
		break;
	case MONACO_I2C_BOOT_FIFO:
		sail_check(f->clock_writes == 3 && f->profile_writes == 75);
		sail_check(monaco_i2c_write_allowed(address));
		f->fifo_writes++;
		break;
	default:
		sail_check(false);
	}
	if (address != CFG || !f->ignore_cfg) {
		*reg(f, address) = value;
	}
	/* Synthetic device model of the publication. */
	if (address == SE + 0x2c4 && f->profile_writes == 75) {
		sail_check(*reg(f, SE + 0x1000) == 0x303 && *reg(f, SE + 0x1004) == 0x303);
		sail_check(*reg(f, SE) == 9 && *reg(f, SE + 4) == 9);
		*reg(f, SE + 0x68) = 0x303;
	}
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;
	bool failed = record(f, address, value, true);

	if (!failed || f->write_before_error) {
		apply_write(f, address, value);
	}
	return failed ? f->error : 0;
}

static struct monaco_clock prepare(struct fake *f, struct monaco_timer *timer,
				   struct monaco_i2c_boot_status *status)
{
	*f = (struct fake){.status = status};
	struct monaco_timer_io io = {f, read32, write32};

	sail_check(monaco_timer_init(timer, &io, 8) == 0);
	*reg(f, CFG) = 0xa5a0c302;
	*reg(f, CBCR) = 0x80000000;
	*reg(f, 0xf19c0004) = 0x300b0000;
	*reg(f, SE + 0xe2c) = 0x1000;
	*reg(f, SE + 0xe24) = 0x20100000;
	*reg(f, SE + 0xe28) = 0x20100000;
	*reg(f, SE + 0x68) = 0xffff;
	f->recording = true;
	return (struct monaco_clock){io, timer, 4};
}

static int run(struct fake *f, struct monaco_clock *clock, struct monaco_i2c *i2c,
	       struct monaco_i2c_boot_status *status)
{
	/* A previous caller's usable context must never survive a failed attempt. */
	*i2c = (struct monaco_i2c){.initialized = true, .busy = true, .tx_depth = 47};
	memset(status, 0xa5, sizeof(*status));
	f->status = status;
	return monaco_i2c_boot_init(i2c, clock, &qualified, status);
}

static void invalid_transport(struct fake *f, struct monaco_i2c *i2c)
{
	unsigned int count = f->count;

	sail_check(!i2c->initialized && !i2c->busy);
	sail_check(monaco_i2c_probe(i2c, 0x28, 100) == MONACO_I2C_STATE);
	sail_check(f->count == count);
}

static void test_success_and_faults(void)
{
	struct fake good, f;
	struct monaco_timer timer;
	struct monaco_i2c_boot_status status;
	struct monaco_i2c i2c;
	struct monaco_clock clock = prepare(&good, &timer, &status);

	sail_check(run(&good, &clock, &i2c, &status) == 0);
	sail_check(status.stage == MONACO_I2C_BOOT_COMPLETE && status.completed_stages == 3);
	sail_check(status.result == 0 && status.profile.configuration_readback);
	sail_check(status.profile.stage == MONACO_I2C_PROFILE_COMPLETE);
	sail_check(status.profile.writes_completed == 75 && status.fifo.result == 0);
	sail_check(i2c.initialized && !i2c.busy && i2c.tx_depth == 16 && i2c.rx_depth == 16);
	sail_check(i2c.io.context == &good && i2c.io.read32 == read32 && i2c.io.write32 == write32);
	sail_check(i2c.timer == &timer && i2c.max_poll_reads == 4);
	sail_check(good.clock_writes == 3 && good.profile_writes == 75 && good.fifo_writes == 25);
	sail_check((*reg(&good, CFG) & 0x371fU) == 1 && (*reg(&good, CBCR) & 0xd0000001U) == 1);
	sail_check(*reg(&good, SE + 0x284) == 0x7f8fe && *reg(&good, SE + 0x278) == 0x302412);
	sail_check(*reg(&good, 0xf0100000) == 0x207 && *reg(&good, 0xf0101000) == 0x207);
	sail_check(*reg(&good, SE + 0x2000) == 1 && *reg(&good, SE + 0x618) == UINT32_MAX);
	unsigned int starts[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};

	for (unsigned int n = 0; n < good.count; n++) {
		unsigned int stage = (unsigned int)good.accesses[n].stage - 1U;

		sail_check(stage < 3);
		if (starts[stage] == UINT32_MAX) {
			starts[stage] = n;
		}
	}
	sail_check(good.accesses[starts[0]].address == DFS);
	sail_check(good.accesses[starts[1]].address == 0xf19c0004);
	sail_check(good.accesses[starts[2]].address == SE + 0x68);
	sail_check(starts[0] < starts[1] && starts[1] + 90 == starts[2]);
	const int errors[] = {-1, INT_MIN, 1};

	for (unsigned int n = 1; n <= good.count; n++) {
		for (unsigned int error = 0; error < ARRAY_SIZE(errors); error++) {
			for (unsigned int post_effect = 0; post_effect < 2; post_effect++) {
				clock = prepare(&f, &timer, &status);
				f.fail_at = n;
				f.error = errors[error];
				f.write_before_error = post_effect != 0;
				int rc = run(&f, &clock, &i2c, &status);
				enum monaco_i2c_boot_stage stage = good.accesses[n - 1].stage;
				int wanted = stage == MONACO_I2C_BOOT_CLOCK
						     ? (timer_address(good.accesses[n - 1].address)
								? MONACO_CLOCK_TIMEBASE
								: MONACO_CLOCK_IO)
					     : stage == MONACO_I2C_BOOT_PROFILE
						     ? MONACO_I2C_PROFILE_IO
						     : MONACO_I2C_IO;

				sail_check(rc == wanted && status.result == wanted && f.count == n);
				sail_check(status.stage == stage &&
				       status.completed_stages == (unsigned int)stage - 1U);
				for (unsigned int i = 0; i < n; i++) {
					sail_check(f.accesses[i].address ==
						   good.accesses[i].address);
					sail_check(f.accesses[i].write == good.accesses[i].write);
				}
				if (stage == MONACO_I2C_BOOT_PROFILE) {
					sail_check(status.profile.callback_result == errors[error]);
				}
				if (stage == MONACO_I2C_BOOT_FIFO) {
					sail_check(status.fifo.result == MONACO_I2C_IO);
				}
				invalid_transport(&f, &i2c);
			}
		}
	}
	printf("I2C boot real-component trace and %u access faults passed\n", good.count);
}

static void test_guards_and_partial_state(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_i2c_boot_status status;
	struct monaco_i2c i2c;
	struct monaco_clock clock;

	for (unsigned int fault = 0; fault < 5; fault++) {
		clock = prepare(&f, &timer, &status);
		if (fault == 0) {
			*reg(&f, DFS) = 1;
		}
		if (fault == 1) {
			*reg(&f, CMD) = 1;
			f.hold_cmd = true;
		}
		if (fault == 2) {
			f.ignore_cfg = true;
		}
		if (fault == 3) {
			f.hold_cbcr = true;
		}
		if (fault == 4) {
			f.hold_cbcr = true;
			f.stop_timer = true;
		}
		int wanted = fault == 0 || fault == 2 ? MONACO_CLOCK_STATE
			     : fault == 4             ? MONACO_CLOCK_TIMEBASE
						      : MONACO_CLOCK_POLL_LIMIT;
		sail_check(run(&f, &clock, &i2c, &status) == wanted);
		sail_check(status.stage == MONACO_I2C_BOOT_CLOCK && status.completed_stages == 0);
		sail_check(f.profile_writes == 0 && f.fifo_writes == 0);
		if (fault < 2) {
			sail_check(f.writes == 0);
		}
		invalid_transport(&f, &i2c);
	}
	const struct {
		enum monaco_i2c_boot_stage stage;
		uint32_t address, value;
		int error;
	} faults[] = {
		{MONACO_I2C_BOOT_PROFILE, 0xf19c0004, 0, MONACO_I2C_PROFILE_VERSION},
		{MONACO_I2C_BOOT_PROFILE, SE + 0xe2c, 0, MONACO_I2C_PROFILE_CAPABILITY},
		{MONACO_I2C_BOOT_PROFILE, SE + 0xe2c, UINT32_MAX, MONACO_I2C_PROFILE_CAPABILITY},
		{MONACO_I2C_BOOT_PROFILE, SE + 0x40, 0x1000, MONACO_I2C_PROFILE_BUSY},
		{MONACO_I2C_BOOT_PROFILE, SE + 0xe28, 0x20020000, MONACO_I2C_PROFILE_FIFO},
		{MONACO_I2C_BOOT_PROFILE, SE + 0x68, 0xffff, MONACO_I2C_PROFILE_READBACK},
		{MONACO_I2C_BOOT_FIFO, SE + 0x68, 0xffff, MONACO_I2C_FIRMWARE},
		{MONACO_I2C_BOOT_FIFO, SE + 0x40, 1, MONACO_I2C_STATE},
		{MONACO_I2C_BOOT_FIFO, SE + 0xe24, 0x20000000, MONACO_I2C_FIFO},
		{MONACO_I2C_BOOT_FIFO, 0xf0100000, 0x206, MONACO_I2C_STATE},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(faults); i++) {
		clock = prepare(&f, &timer, &status);
		f.override_read = true;
		f.override_stage = faults[i].stage;
		f.override_address = faults[i].address;
		f.override_value = faults[i].value;
		sail_check(run(&f, &clock, &i2c, &status) == faults[i].error);
		sail_check(status.stage == faults[i].stage);
		sail_check(status.completed_stages == (unsigned int)faults[i].stage - 1U);
		sail_check(f.clock_writes == 3);
		if (i < 5) {
			sail_check(f.profile_writes == 0);
		}
		if (i == 5) {
			sail_check(f.profile_writes == 75 &&
				   !status.profile.configuration_readback);
		}
		if (faults[i].stage == MONACO_I2C_BOOT_FIFO) {
			sail_check(status.profile.configuration_readback);
		}
		if (i < 9) {
			sail_check(f.fifo_writes == 0);
		}
		if (i == 9) {
			sail_check(f.fifo_writes == 1);
		}
		invalid_transport(&f, &i2c);
	}
}

static void test_invalid_inputs(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_i2c_boot_status status;
	struct monaco_i2c i2c;
	struct monaco_clock clock;

	for (unsigned int fault = 0; fault < 13; fault++) {
		clock = prepare(&f, &timer, &status);
		i2c = (struct monaco_i2c){.initialized = true, .busy = true};
		memset(&status, 0xff, sizeof(status));
		if (fault == 4) {
			clock.io.read32 = NULL;
		}
		if (fault == 5) {
			clock.io.write32 = NULL;
		}
		if (fault == 6) {
			clock.timer = NULL;
		}
		if (fault == 7) {
			timer.initialized = false;
		}
		if (fault == 8) {
			clock.max_poll_reads = 0;
		}
		if (fault == 9) {
			timer.io.read32 = NULL;
		}
		if (fault == 10) {
			timer.io.write32 = NULL;
		}
		if (fault == 11) {
			timer.frequency = 0;
		}
		if (fault == 12) {
			timer.read_budget = 0;
		}
		sail_check(monaco_i2c_boot_init(
				   fault == 0 ? NULL : &i2c, fault == 1 ? NULL : &clock,
				   fault == 2 ? NULL : &qualified,
				   fault == 3 ? NULL : &status) == MONACO_I2C_BOOT_INVALID);
		sail_check(f.count == 0);
		if (fault != 0) {
			invalid_transport(&f, &i2c);
		}
		if (fault != 3) {
			sail_check(status.result == MONACO_I2C_BOOT_INVALID &&
			       status.completed_stages == 0);
			sail_check(status.stage == MONACO_I2C_BOOT_INPUT &&
			       !status.profile.configuration_readback);
		}
	}
	for (unsigned int fault = 0; fault < 6; fault++) {
		clock = prepare(&f, &timer, &status);
		struct monaco_i2c_boot_qualification q = qualified;

		if (fault == 0) {
			q.board = MONACO_I2C_PROFILE_BOARD_UNKNOWN;
		}
		if (fault == 1) {
			q.board = (enum monaco_i2c_profile_board)99;
		}
		if (fault == 2) {
			q.exclusive_ownership = false;
		}
		if (fault == 3) {
			q.parent_clocks_qualified = false;
		}
		if (fault == 4) {
			q.reset_state_qualified = false;
		}
		if (fault == 5) {
			q.board_state_qualified = false;
		}
		i2c = (struct monaco_i2c){.initialized = true, .busy = true};
		sail_check(monaco_i2c_boot_init(&i2c, &clock, &q, &status) ==
		       MONACO_I2C_BOOT_UNQUALIFIED);
		sail_check(status.completed_stages == 0 && status.stage == MONACO_I2C_BOOT_INPUT);
		sail_check(f.count == 0);
		invalid_transport(&f, &i2c);
	}
}

int sail_model_test(void)
{
	test_success_and_faults();
	test_guards_and_partial_state();
	test_invalid_inputs();
	puts("I2C boot guards, partial state and stale transport retirement passed");
	return 0;
}
