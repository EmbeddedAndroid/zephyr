/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "i2c_profile.h"

#include "sail_check.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define SE            UINT32_C(0xf1980000)
struct reg {
	uint32_t address, value;
};

/* Complete expected write trace, including zero-valued configuration. */
static const struct reg expected[] = {
	{0xf19c0120, 0x25},      {0xf19c0118, 1},         {0xf1981000, 0x303},
	{0xf1981004, 0x303},     {0xf1980000, 9},         {0xf1980004, 9},
	{0xf1980100, 0x90},      {0xf1980104, 0},         {0xf1980108, 0x90},
	{0xf198010c, 0},         {0xf1980110, 0x38028},   {0xf1980114, 0x84080},
	{0xf1980118, 0x343},     {0xf198011c, 0x10000},   {0xf1980120, 0},
	{0xf1980124, 0x1a00},    {0xf1980128, 0x100},     {0xf198012c, 0},
	{0xf1980130, 0},         {0xf1980134, 0},         {0xf1980138, 0x808008},
	{0xf198013c, 0x1c0020},  {0xf1980140, 0},         {0xf1980144, 0x20000},
	{0xf1980148, 0},         {0xf1980200, 0x201},     {0xf1980204, 0x1fc01},
	{0xf1980208, 0x36222},   {0xf198020c, 0x9c01ffc}, {0xf1980210, 0x100120},
	{0xf1980214, 0x2c00000}, {0xf1980218, 0},         {0xf198021c, 0},
	{0xf1980220, 0},         {0xf1980224, 0},         {0xf1980228, 0},
	{0xf198022c, 0},         {0xf1980230, 1},         {0xf1980234, 0},
	{0xf1980238, 0},         {0xf198023c, 0},         {0xf1980240, 0x409},
	{0xf1980244, 3},         {0xf1980248, 2},         {0xf198024c, 0},
	{0xf1980250, 0},         {0xf1980254, 0},         {0xf1980258, 0},
	{0xf198025c, 0},         {0xf1980260, 0x7f8fe},   {0xf1980264, 0xffefe},
	{0xf1980268, 0},         {0xf198026c, 0},         {0xf1980270, 0},
	{0xf1980274, 0},         {0xf1980278, 0},         {0xf198027c, 0},
	{0xf1980280, 1},         {0xf1980284, 0x7f807},   {0xf1980288, 0xffefe},
	{0xf198028c, 0},         {0xf1980290, 0},         {0xf1980294, 0},
	{0xf1980298, 0},         {0xf198029c, 0},         {0xf19802a0, 0},
	{0xf19802a4, 0},         {0xf19802a8, 0},         {0xf19802ac, 0},
	{0xf19802b0, 0xc00000},  {0xf19802b4, 0},         {0xf19802b8, 0},
	{0xf19802bc, 0},         {0xf19802c0, 0},         {0xf19802c4, 0x55},
};

struct access {
	uint32_t address, value;
	bool write;
	enum monaco_i2c_profile_stage stage;
};

struct fake {
	struct access accesses[90];
	unsigned int count, writes, fail_at;
	int error;
	uint32_t version, capability, protocol, wrapper;
	uint32_t state, tx_status, rx_status, tx_geometry, rx_geometry;
	uint32_t post_address, post_value;
	bool post_override;
	struct monaco_i2c_profile_status *status;
};

static const struct monaco_i2c_profile_qualification qualified = {
	.board = MONACO_I2C_PROFILE_VENTUNO_Q,
	.exclusive_ownership = true,
	.clocks_qualified = true,
	.reset_state_qualified = true,
	.board_state_qualified = true,
};

static struct fake fresh(void)
{
	return (struct fake){
		.version = 0x300b0000,
		.capability = 0x1000,
		.protocol = 0x303,
		.wrapper = 0xa5a5a5a5,
		.tx_geometry = 0x20100000,
		.rx_geometry = 0x20100000,
	};
}

static bool record(struct fake *f, uint32_t address, uint32_t value, bool write)
{
	sail_check(f->count < ARRAY_SIZE(f->accesses));
	f->accesses[f->count++] = (struct access){
		.address = address,
		.value = value,
		.write = write,
		.stage = f->status->stage,
	};
	return f->count == f->fail_at;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;
	uint32_t sampled;

	switch (address) {
	case 0xf19c0004:
		sampled = f->version;
		break;
	case 0xf19c0120:
		sampled = f->wrapper;
		break;
	case 0xf19c0118:
		sampled = UINT32_MAX;
		break;
	case SE + 0xe2c:
		sampled = f->capability;
		break;
	case SE + 0x68:
		sampled = f->protocol;
		break;
	case SE + 0x40:
		sampled = f->state;
		break;
	case SE + 0x800:
		sampled = f->tx_status;
		break;
	case SE + 0x804:
		sampled = f->rx_status;
		break;
	case SE + 0xe24:
		sampled = f->tx_geometry;
		break;
	case SE + 0xe28:
		sampled = f->rx_geometry;
		break;
	default:
		sail_check(false);
		return -1;
	}
	if (f->post_override && f->writes == 75 && address == f->post_address) {
		sampled = f->post_value;
	}
	*value = sampled;
	if (record(f, address, sampled, false)) {
		/* A failing callback must not leak its output into valid status. */
		*value = UINT32_MAX;
		return f->error;
	}
	return 0;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;

	sail_check(f->writes < ARRAY_SIZE(expected));
	sail_check(address == expected[f->writes].address);
	uint32_t wanted = f->writes == 0 ? (f->wrapper & 0x7eU) | 1U : expected[f->writes].value;

	sail_check(value == wanted);
	sail_check(monaco_i2c_profile_write_allowed(address));
	if (record(f, address, value, true)) {
		return f->error;
	}
	f->writes++;
	return 0;
}

static int run(struct fake *f, struct monaco_i2c_profile_status *status)
{
	const struct monaco_timer_io io = {f, read32, write32};

	f->status = status;
	return monaco_i2c_profile_init(&io, &qualified, status);
}

static void test_trace(void)
{
	struct fake f = fresh();
	struct monaco_i2c_profile_status s;

	sail_check(run(&f, &s) == 0);
	sail_check(s.result == 0 && s.callback_result == 0);
	sail_check(s.stage == MONACO_I2C_PROFILE_COMPLETE && s.configuration_readback);
	sail_check(s.qup_version == 0x300b0000 && s.hardware_parameters == 0x1000);
	sail_check(s.protocol_revision == 0x303 && s.tx_depth == 16 && s.rx_depth == 16);
	sail_check(s.reads_attempted == 15 && s.reads_completed == 15);
	sail_check(s.writes_attempted == 75 && s.writes_completed == 75);
	sail_check(f.count == 90 && f.writes == 75 && s.value_valid);
	const uint32_t before[] = {
		0xf19c0004, SE + 0xe2c, SE + 0x40,  SE + 0x800,
		SE + 0x804, SE + 0xe24, SE + 0xe28, 0xf19c0120,
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(before); i++) {
		sail_check(f.accesses[i].address == before[i] && !f.accesses[i].write);
		sail_check(f.accesses[i].stage ==
		       (i == 7 ? MONACO_I2C_PROFILE_WRAPPER : MONACO_I2C_PROFILE_GUARDS));
	}
	sail_check(f.accesses[9].address == 0xf19c0118 && !f.accesses[9].write);
	const uint32_t after[] = {
		SE + 0x68, SE + 0xe24, SE + 0xe28, SE + 0x40, SE + 0x800, SE + 0x804,
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(after); i++) {
		sail_check(f.accesses[84 + i].address == after[i] && !f.accesses[84 + i].write);
		sail_check(f.accesses[84 + i].stage == MONACO_I2C_PROFILE_VERIFY);
	}
	unsigned int next = 0;

	for (unsigned int i = 0; i < f.count; i++) {
		if (!f.accesses[i].write) {
			continue;
		}
		sail_check(f.accesses[i].address == expected[next].address);
		sail_check(f.accesses[i].value == expected[next].value);
		enum monaco_i2c_profile_stage stage = next < 2    ? MONACO_I2C_PROFILE_WRAPPER
						      : next < 6  ? MONACO_I2C_PROFILE_METADATA
						      : next < 25 ? MONACO_I2C_PROFILE_CFG0
								  : MONACO_I2C_PROFILE_CFG64;
		sail_check(f.accesses[i].stage == stage);
		next++;
	}
	sail_check(next == 75);
}

static void test_failures(void)
{
	struct fake good = fresh();
	struct monaco_i2c_profile_status s;
	const int errors[] = {-1, -45, INT_MIN, 1, INT_MAX};

	sail_check(run(&good, &s) == 0);
	for (unsigned int n = 1; n <= good.count; n++) {
		for (unsigned int e = 0; e < ARRAY_SIZE(errors); e++) {
			struct fake f = fresh();

			f.fail_at = n;
			f.error = errors[e];
			sail_check(run(&f, &s) == MONACO_I2C_PROFILE_IO);
			sail_check(s.result == MONACO_I2C_PROFILE_IO);
			sail_check(s.callback_result == errors[e] && f.count == n);
			sail_check(!s.configuration_readback &&
				   s.stage == good.accesses[n - 1].stage);
			sail_check(s.address == good.accesses[n - 1].address);
			sail_check(s.value_valid == good.accesses[n - 1].write);
			unsigned int writes = 0, reads = 0;

			for (unsigned int i = 0; i < n; i++) {
				sail_check(f.accesses[i].address == good.accesses[i].address);
				sail_check(f.accesses[i].write == good.accesses[i].write);
				if (f.accesses[i].write) {
					writes++;
				} else {
					reads++;
				}
			}
			sail_check(s.writes_attempted == writes && s.reads_attempted == reads);
			sail_check(s.writes_completed ==
			       writes - (good.accesses[n - 1].write ? 1U : 0U));
			sail_check(s.reads_completed ==
				   reads - (good.accesses[n - 1].write ? 0U : 1U));
			sail_check(f.writes == s.writes_completed);
			if (!good.accesses[n - 1].write) {
				sail_check(s.value == 0);
			}
			if (n == 1) {
				sail_check(s.qup_version == 0);
			}
			if (n == 2) {
				sail_check(s.hardware_parameters == 0);
			}
			if (n == 85) {
				sail_check(s.protocol_revision == 0);
			}
		}
	}
}

static void test_inputs(void)
{
	struct fake f = fresh();
	struct monaco_i2c_profile_status s;
	struct monaco_timer_io io = {&f, read32, write32};
	struct monaco_i2c_profile_qualification q;

	f.status = &s;
	sail_check(monaco_i2c_profile_init(&io, &qualified, NULL) == MONACO_I2C_PROFILE_INVALID);
	for (unsigned int i = 0; i < 5; i++) {
		memset(&s, 0xff, sizeof(s));
		struct monaco_timer_io invalid = io;

		if (i == 2) {
			invalid.read32 = NULL;
		}
		if (i == 3) {
			invalid.write32 = NULL;
		}
		int rc = monaco_i2c_profile_init(i == 0 ? NULL : &invalid,
						 i == 1 ? NULL : &qualified, &s);
		if (i == 4) {
			sail_check(rc == 0);
			f = fresh();
			f.status = &s;
			continue;
		}
		sail_check(rc == MONACO_I2C_PROFILE_INVALID && f.count == 0);
		sail_check(s.result == rc && s.stage == MONACO_I2C_PROFILE_INPUT);
		sail_check(!s.configuration_readback && !s.value_valid);
		sail_check(s.reads_attempted == 0 && s.writes_attempted == 0);
	}
	for (unsigned int i = 0; i < 6; i++) {
		q = qualified;
		s.configuration_readback = true;
		s.writes_attempted = 75;
		if (i == 0) {
			q.board = MONACO_I2C_PROFILE_BOARD_UNKNOWN;
		}
		if (i == 1) {
			q.board = (enum monaco_i2c_profile_board)99;
		}
		if (i == 2) {
			q.exclusive_ownership = false;
		}
		if (i == 3) {
			q.clocks_qualified = false;
		}
		if (i == 4) {
			q.reset_state_qualified = false;
		}
		if (i == 5) {
			q.board_state_qualified = false;
		}
		sail_check(monaco_i2c_profile_init(&io, &q, &s) == MONACO_I2C_PROFILE_UNQUALIFIED);
		sail_check(f.count == 0 && !s.configuration_readback && s.writes_attempted == 0);
	}
}

static void expect_guard(struct fake *f, int result)
{
	struct monaco_i2c_profile_status s;

	sail_check(run(f, &s) == result);
	sail_check(!s.configuration_readback && s.stage == MONACO_I2C_PROFILE_GUARDS);
	sail_check(s.result == result && s.callback_result == 0);
	sail_check(f->writes == 0 && s.writes_attempted == 0 && s.writes_completed == 0);
}

static void test_guards(void)
{
	struct monaco_i2c_profile_status s;

	for (unsigned int bit = 0; bit < 32; bit++) {
		struct fake f = fresh();

		f.version ^= UINT32_C(1) << bit;
		expect_guard(&f, MONACO_I2C_PROFILE_VERSION);
		sail_check(f.count == 1);
	}
	for (uint32_t low = 0; low <= UINT16_MAX; low++) {
		struct fake f = fresh();

		f.capability = low;
		if (low & 0x1000U) {
			sail_check(run(&f, &s) == 0);
		} else {
			expect_guard(&f, MONACO_I2C_PROFILE_CAPABILITY);
		}
	}
	struct fake f = fresh();

	f.capability = UINT32_MAX;
	expect_guard(&f, MONACO_I2C_PROFILE_CAPABILITY);
	for (unsigned int field = 0; field < 3; field++) {
		for (unsigned int bit = 0; bit < 32; bit++) {
			f = fresh();
			uint32_t value = UINT32_C(1) << bit;
			uint32_t mask = field == 0   ? 0x1001U
					: field == 1 ? 0x0fffffffU
						     : 0x01ffffffU;
			if (field == 0) {
				f.state = value;
			}
			if (field == 1) {
				f.tx_status = value;
			}
			if (field == 2) {
				f.rx_status = value;
			}
			if (value & mask) {
				expect_guard(&f, field == 0 ? MONACO_I2C_PROFILE_BUSY
							    : MONACO_I2C_PROFILE_FIFO);
			} else {
				sail_check(run(&f, &s) == 0);
			}
		}
	}
}

static void test_geometry_and_readback(void)
{
	struct monaco_i2c_profile_status s;

	for (unsigned int side = 0; side < 2; side++) {
		for (uint32_t width = 0; width < 64; width++) {
			for (uint32_t depth = 0; depth < 64; depth++) {
				struct fake f = fresh();
				uint32_t geometry = width << 24 | depth << 16;

				if (side == 0) {
					f.tx_geometry = geometry;
				} else {
					f.rx_geometry = geometry;
				}
				if (width == 32 && depth >= (side == 0 ? 1U : 3U)) {
					sail_check(run(&f, &s) == 0);
				} else {
					expect_guard(&f, MONACO_I2C_PROFILE_FIFO);
				}
			}
		}
	}
	for (uint32_t protocol = 0; protocol < 256; protocol++) {
		struct fake f = fresh();

		f.protocol = protocol << 8 | 0xab;
		int rc = run(&f, &s);

		sail_check(rc == (protocol == 3 ? 0 : MONACO_I2C_PROFILE_READBACK));
		sail_check(f.writes == 75 && s.configuration_readback == (protocol == 3));
		sail_check(s.protocol_revision == f.protocol);
	}
	const struct {
		uint32_t address, value;
		int result;
	} faults[] = {
		{SE + 0xe24, 0x1f100000, MONACO_I2C_PROFILE_FIFO},
		{SE + 0xe24, 0x20000000, MONACO_I2C_PROFILE_FIFO},
		{SE + 0xe28, 0x21100000, MONACO_I2C_PROFILE_FIFO},
		{SE + 0xe28, 0x20020000, MONACO_I2C_PROFILE_FIFO},
		{SE + 0x40, 1, MONACO_I2C_PROFILE_BUSY},
		{SE + 0x40, 0x1000, MONACO_I2C_PROFILE_BUSY},
		{SE + 0x800, 1, MONACO_I2C_PROFILE_FIFO},
		{SE + 0x804, 1, MONACO_I2C_PROFILE_FIFO},
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(faults); i++) {
		struct fake f = fresh();

		f.post_override = true;
		f.post_address = faults[i].address;
		f.post_value = faults[i].value;
		sail_check(run(&f, &s) == faults[i].result);
		sail_check(f.writes == 75 && !s.configuration_readback);
		uint32_t last = faults[i].address == SE + 0xe24 ? SE + 0xe28 : faults[i].address;

		sail_check(s.stage == MONACO_I2C_PROFILE_VERIFY && s.address == last);
	}
	for (unsigned int bit = 0; bit < 32; bit++) {
		struct fake f = fresh();

		f.wrapper = UINT32_C(1) << bit;
		sail_check(run(&f, &s) == 0);
		sail_check(f.accesses[8].value == ((f.wrapper & 0x7eU) | 1U));
	}
}

static void test_allowlist(void)
{
	unsigned int allowed = 0;

	for (uint32_t address = SE - 4; address < SE + 0x3004; address++) {
		bool wanted = false;

		for (unsigned int i = 2; i < ARRAY_SIZE(expected); i++) {
			wanted |= expected[i].address == address;
		}
		sail_check(monaco_i2c_profile_write_allowed(address) == wanted);
		if (wanted) {
			allowed++;
		}
	}
	sail_check(allowed == 73);
	for (uint32_t address = 0xf19c0000; address < 0xf19c0200; address++) {
		sail_check(monaco_i2c_profile_write_allowed(address) ==
		       (address == 0xf19c0120 || address == 0xf19c0118));
	}
	const uint32_t forbidden[] = {
		0,          UINT32_MAX, 0xf100000c, 0xf0305084, 0xf0305088, 0xf030508c, 0xf0305090,
		0xf0100000, 0xf0101000, 0xf1980050, 0xf1981010, 0xf1982000, 0xf19c0004,
	};
	for (unsigned int i = 0; i < ARRAY_SIZE(forbidden); i++) {
		sail_check(!monaco_i2c_profile_write_allowed(forbidden[i]));
	}
}

int sail_model_test(void)
{
	test_trace();
	test_failures();
	test_inputs();
	test_guards();
	test_geometry_and_readback();
	test_allowlist();
	puts("I2C minicore profile: 75-write trace, 90 access faults, guards and readback passed");
	return 0;
}
