/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic.h"

#include "sail_check.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>


#define TIMEOUT_US      5000U
#define TRANSPORT_ERROR (-77)

enum event_kind {
	TRANSFER,
	DELAY
};

struct event {
	enum event_kind kind;
	uint8_t address;
	uint8_t tx[3];
	size_t tx_size;
	size_t rx_size;
	uint32_t argument;
};

struct fake {
	uint8_t registers[3][256];
	struct event events[32];
	unsigned int count;
	unsigned int fail_at;
	unsigned int corrupt_at;
	int failure;
	bool crc_enabled;
	bool fail_after_enable;
	bool change_during_enable;
};

/* Independent whole-message polynomial division, not the driver's bit loop. */
static uint8_t reference_crc(const uint8_t *data, size_t size)
{
	uint64_t message = 0;
	unsigned int bits = (unsigned int)size * 8U;

	sail_check(size >= 1 && size <= 4);
	for (size_t i = 0; i < size; ++i) {
		message = (message << 8) | data[i];
	}
	message = (message << 8) ^ (UINT64_C(0xff) << bits);
	for (unsigned int bit = bits + 8U; bit-- > 8U;) {
		if (message & (UINT64_C(1) << bit)) {
			message ^= UINT64_C(0x107) << (bit - 8U);
		}
	}
	sail_check(message <= UINT8_MAX);
	return (uint8_t)message;
}

static struct event *record(struct fake *fake, enum event_kind kind)
{
	struct event *event;

	sail_check(fake->count < ARRAY_SIZE(fake->events));
	event = &fake->events[fake->count++];
	*event = (struct event){.kind = kind};
	return event;
}

static int bus_transfer(void *context, uint8_t address, const uint8_t *tx, size_t tx_size,
			uint8_t *rx, size_t rx_size, uint32_t timeout)
{
	struct fake *fake = context;
	struct event *event = record(fake, TRANSFER);
	unsigned int slave;
	uint8_t covered[4];

	sail_check(timeout == TIMEOUT_US);
	sail_check(tx && tx_size >= 1 && tx_size <= 3);
	sail_check((rx && rx_size >= 1 && rx_size <= 2) || (!rx && !rx_size));
	event->address = address;
	memcpy(event->tx, tx, tx_size);
	event->tx_size = tx_size;
	event->rx_size = rx_size;
	event->argument = timeout;
	sail_check(address == 0x28 || address == 0x29 || address == 0x12);
	slave = address == 0x28 ? 0U : (address == 0x29 ? 1U : 2U);
	if (fake->count == fake->fail_at && !fake->fail_after_enable) {
		/* Even a callback that changed rx before failing must not leak it. */
		if (rx) {
			memset(rx, 0xba, rx_size);
		}
		return fake->failure;
	}
	if (!fake->crc_enabled) {
		sail_check(slave == 0);
		if (rx_size) {
			sail_check(tx_size == 1 && rx_size == 1);
			sail_check(tx[0] == 0x01 || tx[0] == 0x02 || tx[0] == 0x03 ||
				   tx[0] == 0xa7);
			rx[0] = fake->registers[slave][tx[0]];
		} else {
			sail_check(tx_size == 2 && tx[0] == 0x85 && tx[1] == 2);
			fake->crc_enabled = true;
			if (fake->count == fake->fail_at && fake->fail_after_enable) {
				return fake->failure;
			}
		}
		return 0;
	}
	covered[0] = (uint8_t)(address << 1);
	covered[1] = tx[0];
	if (rx_size) {
		sail_check(tx_size == 1 && rx_size == 2);
		if (fake->change_during_enable && fake->count == 3) {
			/* Change unrelated bits before the first enable RMW read. */
			fake->registers[slave][tx[0]] = 0x68;
		}
		rx[0] = fake->registers[slave][tx[0]];
		covered[2] = (uint8_t)(covered[0] | 1U);
		covered[3] = rx[0];
		rx[1] = reference_crc(covered, 4);
		if (fake->count == fake->corrupt_at) {
			rx[1] ^= 1U;
		}
	} else {
		sail_check(tx_size == 3);
		covered[2] = tx[1];
		sail_check(tx[2] == reference_crc(covered, 3));
		fake->registers[slave][tx[0]] = tx[1];
	}
	return 0;
}

static int bus_delay(void *context, uint32_t us)
{
	struct fake *fake = context;
	struct event *event = record(fake, DELAY);

	sail_check(us == 2000 || us == 1000);
	event->argument = us;
	return fake->count == fake->fail_at ? fake->failure : 0;
}

static void reset_log(struct fake *fake)
{
	fake->count = 0;
	fake->fail_at = 0;
	fake->corrupt_at = 0;
	fake->failure = TRANSPORT_ERROR;
	fake->fail_after_enable = false;
	fake->change_during_enable = false;
}

static struct monaco_pmic_io bus_io(struct fake *fake)
{
	return (struct monaco_pmic_io){fake, bus_transfer, bus_delay};
}

static void prepare_raw(struct fake *fake, struct monaco_pmic *pmic)
{
	struct monaco_pmic_io io = bus_io(fake);

	*fake = (struct fake){.failure = TRANSPORT_ERROR};
	fake->registers[0][0x01] = 0x82;
	fake->registers[0][0x02] = 0xa5;
	fake->registers[0][0x03] = 0x01;
	fake->registers[0][0xa7] = 0x00;
	sail_check(monaco_pmic_init(pmic, &io, TIMEOUT_US) == 0);
}

static void prepare_crc(struct fake *fake, struct monaco_pmic *pmic)
{
	prepare_raw(fake, pmic);
	sail_check(monaco_pmic_identify(pmic) == 0);
	sail_check(monaco_pmic_enable_crc(pmic) == 0);
	sail_check(fake->crc_enabled && pmic->state == MONACO_PMIC_CRC_READY);
	reset_log(fake);
}

static void expect_read(const struct fake *fake, unsigned int index, uint8_t address, uint8_t reg,
			size_t rx_size)
{
	const struct event *event = &fake->events[index];

	sail_check(index < fake->count && event->kind == TRANSFER);
	sail_check(event->address == address && event->tx_size == 1);
	sail_check(event->tx[0] == reg && event->rx_size == rx_size);
}

static void expect_write(const struct fake *fake, unsigned int index, uint8_t address, uint8_t reg,
			 uint8_t value)
{
	const struct event *event = &fake->events[index];

	sail_check(index < fake->count && event->kind == TRANSFER);
	sail_check(event->address == address && event->tx_size == 3 && !event->rx_size);
	sail_check(event->tx[0] == reg && event->tx[1] == value);
}

static void test_identity(void)
{
	struct fake fake;
	struct monaco_pmic pmic;
	static const uint8_t regs[] = {0x01, 0x02, 0x03, 0xa7};

	prepare_raw(&fake, &pmic);
	sail_check(monaco_pmic_identify(&pmic) == 0);
	sail_check(fake.count == 4 && pmic.state == MONACO_PMIC_IDENTIFIED);
	sail_check(pmic.version.device == 0x82 && pmic.version.revision == 0xa5);
	sail_check(pmic.version.firmware == 1 && pmic.version.firmware_revision == 0);
	for (unsigned int i = 0; i < 4; ++i) {
		expect_read(&fake, i, 0x28, regs[i], 1);
	}
	sail_check(monaco_pmic_identify(&pmic) == 0 && fake.count == 4);
	sail_check(monaco_pmic_enable_crc(&pmic) == 0);
	sail_check(fake.count == 6 && pmic.state == MONACO_PMIC_CRC_READY);
	sail_check(fake.events[4].address == 0x28 && fake.events[4].tx_size == 2);
	sail_check(fake.events[4].rx_size == 0 && fake.events[4].tx[0] == 0x85);
	sail_check(fake.events[4].tx[1] == 2 && fake.events[5].kind == DELAY);
	sail_check(fake.events[5].argument == 2000);
	sail_check(monaco_pmic_identify(&pmic) == 0);
	sail_check(monaco_pmic_enable_crc(&pmic) == 0 && fake.count == 6);

	/* No acceptance-range restriction on the other three version bytes. */
	prepare_raw(&fake, &pmic);
	fake.registers[0][0x02] = 0;
	fake.registers[0][0x03] = 0xff;
	fake.registers[0][0xa7] = 0xff;
	sail_check(monaco_pmic_identify(&pmic) == 0);
	sail_check(pmic.version.revision == 0 && pmic.version.firmware == 0xff);
	sail_check(pmic.version.firmware_revision == 0xff);

	for (unsigned int device = 0; device <= UINT8_MAX; ++device) {
		if (device == 0x82) {
			continue;
		}
		prepare_raw(&fake, &pmic);
		fake.registers[0][0x01] = (uint8_t)device;
		sail_check(monaco_pmic_identify(&pmic) == MONACO_PMIC_UNSUPPORTED);
		sail_check(fake.count == 4 && pmic.state == MONACO_PMIC_RAW);
		sail_check(!pmic.version.device && !pmic.version.revision);
		sail_check(!pmic.version.firmware && !pmic.version.firmware_revision);
		sail_check(monaco_pmic_enable_crc(&pmic) == MONACO_PMIC_STATE);
		sail_check(fake.count == 4);
	}
	for (unsigned int fail = 1; fail <= 4; ++fail) {
		for (unsigned int positive = 0; positive <= 1; ++positive) {
			prepare_raw(&fake, &pmic);
			fake.fail_at = fail;
			fake.failure = positive ? 0x16 : TRANSPORT_ERROR;
			sail_check(monaco_pmic_identify(&pmic) ==
			       (positive ? MONACO_PMIC_CALLBACK : TRANSPORT_ERROR));
			sail_check(fake.count == fail && pmic.state == MONACO_PMIC_RAW);
			sail_check(!pmic.version.device && !pmic.version.revision);
			sail_check(!pmic.version.firmware && !pmic.version.firmware_revision);
			sail_check(monaco_pmic_enable_crc(&pmic) == MONACO_PMIC_STATE);
			sail_check(fake.count == fail);
			reset_log(&fake);
			sail_check(monaco_pmic_identify(&pmic) == 0 && fake.count == 4);
		}
	}
}

static void test_crc_transition_failures(void)
{
	struct fake fake;
	struct monaco_pmic pmic;
	uint8_t value = 0x5a;

	for (unsigned int fail = 1; fail <= 2; ++fail) {
		for (unsigned int positive = 0; positive <= 1; ++positive) {
			prepare_raw(&fake, &pmic);
			sail_check(monaco_pmic_identify(&pmic) == 0);
			reset_log(&fake);
			fake.fail_at = fail;
			fake.failure = positive ? 1 : TRANSPORT_ERROR;
			sail_check(monaco_pmic_enable_crc(&pmic) ==
			       (positive ? MONACO_PMIC_CALLBACK : TRANSPORT_ERROR));
			sail_check(fake.count == fail && pmic.state == MONACO_PMIC_TRANSITION);
			sail_check(monaco_pmic_enable_crc(&pmic) == MONACO_PMIC_STATE);
			sail_check(monaco_pmic_identify(&pmic) == MONACO_PMIC_STATE);
			sail_check(monaco_pmic_read(&pmic, 0, 0x01, &value) == MONACO_PMIC_STATE);
			sail_check(monaco_pmic_write(&pmic, 0, 0xcd, 0x3b) == MONACO_PMIC_STATE);
			sail_check(value == 0x5a && fake.count == fail);
		}
	}
	prepare_raw(&fake, &pmic);
	sail_check(monaco_pmic_identify(&pmic) == 0);
	reset_log(&fake);
	fake.fail_at = 1;
	fake.fail_after_enable = true;
	sail_check(monaco_pmic_enable_crc(&pmic) == TRANSPORT_ERROR);
	sail_check(fake.crc_enabled && pmic.state == MONACO_PMIC_TRANSITION);
	sail_check(monaco_pmic_enable_crc(&pmic) == MONACO_PMIC_STATE && fake.count == 1);
}

static void test_crc_vectors_and_frames(void)
{
	static const uint8_t vectors[][4] = {
		{0x50, 0xcd, 0x3b, 0xaa},
		{0x50, 0xce, 0x3b, 0x95},
		{0x50, 0xcf, 0x49, 0xd9},
		{0x50, 0xd0, 0x49, 0x4d},
	};
	static const uint8_t read_vector[] = {0x50, 0x66, 0x51, 0x01};
	static const uint8_t addresses[] = {0x28, 0x29, 0x12};
	struct fake fake;
	struct monaco_pmic pmic;
	uint8_t value;

	sail_check(reference_crc(read_vector, 4) == 0x8b);
	prepare_crc(&fake, &pmic);
	for (unsigned int i = 0; i < ARRAY_SIZE(vectors); ++i) {
		sail_check(reference_crc(vectors[i], 3) == vectors[i][3]);
		sail_check(monaco_pmic_write(&pmic, 0, vectors[i][1], vectors[i][2]) == 0);
		sail_check(fake.events[i].tx[2] == vectors[i][3]);
	}
	fake.registers[0][0x66] = 1;
	sail_check(monaco_pmic_read(&pmic, 0, 0x66, &value) == 0 && value == 1);

	/* Every data byte at both register boundaries and every physical address. */
	for (unsigned int slave = 0; slave < 3; ++slave) {
		for (unsigned int reg = 0; reg <= UINT8_MAX; reg += UINT8_MAX) {
			for (unsigned int byte = 0; byte <= UINT8_MAX; ++byte) {
				reset_log(&fake);
				sail_check(monaco_pmic_write(&pmic, slave, (uint8_t)reg,
							 (uint8_t)byte) == 0);
				expect_write(&fake, 0, addresses[slave], (uint8_t)reg,
					     (uint8_t)byte);
				sail_check(monaco_pmic_read(&pmic, slave, (uint8_t)reg, &value) ==
					   0);
				sail_check(value == byte && fake.count == 2);
				expect_read(&fake, 1, addresses[slave], (uint8_t)reg, 2);
			}
		}
	}
	for (unsigned int slave = 0; slave < 3; ++slave) {
		prepare_crc(&fake, &pmic);
		fake.registers[slave][0x66] = 0x39;
		fake.corrupt_at = 1;
		value = 0x5a;
		sail_check(monaco_pmic_read(&pmic, slave, 0x66, &value) == MONACO_PMIC_CRC);
		sail_check(value == 0x5a && fake.count == 1);
		reset_log(&fake);
		fake.corrupt_at = 1;
		sail_check(monaco_pmic_update_bits(&pmic, slave, 0x66, 1, 1) == MONACO_PMIC_CRC);
		sail_check(fake.count == 1 && fake.registers[slave][0x66] == 0x39);
	}
}

static void test_register_errors_and_rmw(void)
{
	struct fake fake;
	struct monaco_pmic pmic;
	uint8_t value;

	prepare_crc(&fake, &pmic);
	fake.registers[0][0xca] = 0xa5;
	sail_check(monaco_pmic_update_bits(&pmic, 0, 0xca, 0x0f, 0xf3) == 0);
	sail_check(fake.registers[0][0xca] == 0xa3 && fake.count == 2);
	expect_read(&fake, 0, 0x28, 0xca, 2);
	expect_write(&fake, 1, 0x28, 0xca, 0xa3);
	reset_log(&fake);
	sail_check(monaco_pmic_update_bits(&pmic, 0, 0xca, 0, 0xff) == 0);
	sail_check(fake.count == 2 && fake.registers[0][0xca] == 0xa3);
	reset_log(&fake);
	sail_check(monaco_pmic_update_bits(&pmic, 0, 0xca, 0xff, 0x5a) == 0);
	sail_check(fake.count == 2 && fake.registers[0][0xca] == 0x5a);

	for (unsigned int positive = 0; positive <= 1; ++positive) {
		int expected = positive ? MONACO_PMIC_CALLBACK : TRANSPORT_ERROR;

		prepare_crc(&fake, &pmic);
		fake.fail_at = 1;
		fake.failure = positive ? 2 : TRANSPORT_ERROR;
		value = 0x5a;
		sail_check(monaco_pmic_read(&pmic, 0, 0x66, &value) == expected);
		sail_check(fake.count == 1 && value == 0x5a);
		reset_log(&fake);
		fake.fail_at = 1;
		fake.failure = positive ? 2 : TRANSPORT_ERROR;
		sail_check(monaco_pmic_write(&pmic, 0, 0x66, 1) == expected);
		sail_check(fake.count == 1);
		for (unsigned int fail = 1; fail <= 2; ++fail) {
			reset_log(&fake);
			fake.fail_at = fail;
			fake.failure = positive ? 2 : TRANSPORT_ERROR;
			sail_check(monaco_pmic_update_bits(&pmic, 0, 0x66, 1, 1) == expected);
			sail_check(fake.count == fail);
		}
	}
}

static struct monaco_pmic_regulator_policy regulator_policy(enum monaco_pmic_regulator_group group,
							    unsigned int index)
{
	return (struct monaco_pmic_regulator_policy){
		.group = group,
		.index = index,
		.write_permitted = true,
		.enable_required = true,
		.required_mode = group == MONACO_PMIC_LDO ? 2 : 0,
		.minimum_uv =
			group == MONACO_PMIC_LDO ? (index == 3 ? 1200000U : 600000U) : 300000U,
		.maximum_uv = group == MONACO_PMIC_LDO ? 3300000U : 3340000U,
	};
}

static uint8_t prepare_rail(struct fake *fake, struct monaco_pmic *pmic,
			    const struct monaco_pmic_regulator_policy *policy)
{
	uint8_t control;

	prepare_crc(fake, pmic);
	if (policy->group == MONACO_PMIC_LDO) {
		control = (uint8_t)(0x1dU + policy->index);
		fake->registers[0][0x23U + policy->index] = policy->index == 3 ? 32 : 8;
	} else {
		control = (uint8_t)(4U + 2U * policy->index);
		fake->registers[0][control + 0x0aU] = 15;
	}
	fake->registers[0][control] = 0x80;
	return control;
}

static void test_regulator_enable_group(unsigned int group)
{
	struct fake fake;
	struct monaco_pmic pmic;
	struct monaco_pmic_regulator_result result;

	unsigned int count = group == 0 ? 4U : 5U;

	for (unsigned int index = 0; index < count; ++index) {
		struct monaco_pmic_regulator_policy policy =
			regulator_policy((enum monaco_pmic_regulator_group)group, index);
		uint8_t control = prepare_rail(&fake, &pmic, &policy);
		unsigned int before_enable = group == 0 ? 2U : 4U;
		uint8_t enabled = group == 0 ? 0x91 : 0xb1;

		sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) == 0);
		sail_check(result.stage == MONACO_PMIC_REGULATOR_COMPLETE);
		sail_check(result.voltage_valid && result.mode_valid && result.enable_valid);
		sail_check(!result.observed_enabled && result.enable_sequence_complete);
		sail_check(!result.enable_skipped && fake.registers[0][control] == enabled);
		sail_check(fake.count == before_enable + 5U);
		expect_read(&fake, before_enable, 0x28, control, 2);
		expect_write(&fake, before_enable + 1U, 0x28, control, 0x81);
		expect_read(&fake, before_enable + 2U, 0x28, control, 2);
		expect_write(&fake, before_enable + 3U, 0x28, control, enabled);
		sail_check(fake.events[before_enable + 4U].kind == DELAY);
		sail_check(fake.events[before_enable + 4U].argument == 1000);
		if (group == 0) {
			expect_read(&fake, 0, 0x28, (uint8_t)(0x23U + index), 2);
			expect_read(&fake, 1, 0x28, control, 2);
		} else {
			expect_read(&fake, 0, 0x28, control, 2);
			expect_read(&fake, 1, 0x28, (uint8_t)(control + 0x0aU), 2);
			expect_read(&fake, 2, 0x28, control, 2);
			expect_read(&fake, 3, 0x28, control, 2);
		}

		for (unsigned int permission = 0; permission < 2; ++permission) {
			for (unsigned int required = 0; required < 2; ++required) {
				for (unsigned int already = 0; already < 2; ++already) {
					policy.write_permitted = permission != 0;
					policy.enable_required = required != 0;
					control = prepare_rail(&fake, &pmic, &policy);
					fake.registers[0][control] |= (uint8_t)already;
					sail_check(monaco_pmic_regulator_init(&pmic, &policy,
									  &result) == 0);
					sail_check(result.observed_enabled == (already != 0));
					sail_check(result.enable_skipped ==
					       (!permission && required && !already));
					sail_check(result.enable_sequence_complete ==
					       (permission && required && !already));
					sail_check(fake.count ==
					       before_enable + ((permission && required && !already)
									? 5U
									: 0U));
					if (!result.enable_sequence_complete) {
						sail_check(fake.registers[0][control] ==
						       (0x80U | already));
					}
				}
			}
		}
	}
}

static void test_regulator_enable_and_permission(void)
{
	struct fake fake;
	struct monaco_pmic pmic;
	struct monaco_pmic_regulator_result result;

	for (unsigned int group = 0; group < 2; ++group) {
		test_regulator_enable_group(group);
	}
	/* RMW must reread current bits, not write a stale enable snapshot. */
	struct monaco_pmic_regulator_policy policy = regulator_policy(MONACO_PMIC_LDO, 0);
	uint8_t control = prepare_rail(&fake, &pmic, &policy);

	fake.change_during_enable = true;
	sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) == 0);
	expect_write(&fake, 3, 0x28, control, 0x69);
	expect_write(&fake, 5, 0x28, control, 0x79);

	/* Required SMPS mode 0 never lowers a current mode 1. */
	policy = regulator_policy(MONACO_PMIC_SMPS, 2);
	control = prepare_rail(&fake, &pmic, &policy);
	fake.registers[0][control] |= 3U;
	sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) == 0);
	sail_check(result.observed_mode == 1 && result.observed_enabled);
	sail_check(fake.count == 4 && fake.registers[0][control] == 0x83);
}

static void test_voltage_boundaries(void)
{
	static const struct {
		uint8_t code;
		uint32_t uv;
	} smps_cases[] = {
		{0, 300000},    {14, 580000},   {15, 600000},   {114, 1095000},
		{115, 1100000}, {170, 1650000}, {171, 1660000}, {255, 3340000},
	};
	static const struct {
		unsigned int index;
		uint8_t raw;
		uint32_t uv;
	} ldo_cases[] = {
		{0, 8, 600000},    {0, 116, 3300000},  {1, 0x89, 600000},
		{2, 126, 3550000}, {2, 6, 13350000},   {3, 32, 1200000},
		{3, 116, 3300000}, {3, 0xa0, 1200000}, {3, 31, 7575000},
	};
	struct fake fake;
	struct monaco_pmic pmic;
	struct monaco_pmic_regulator_result result;
	struct monaco_pmic_regulator_policy policy;
	uint8_t control;

	for (unsigned int i = 0; i < ARRAY_SIZE(smps_cases); ++i) {
		for (unsigned int select = 0; select < 2; ++select) {
			policy = regulator_policy(MONACO_PMIC_SMPS, 4);
			policy.enable_required = false;
			control = prepare_rail(&fake, &pmic, &policy);
			fake.registers[0][control] = select ? 8 : 0;
			fake.registers[0][control + (select ? 0x0bU : 0x0aU)] = smps_cases[i].code;
			sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) == 0);
			sail_check(result.voltage_uv == smps_cases[i].uv && fake.count == 4);
			expect_read(&fake, 1, 0x28, (uint8_t)(control + (select ? 0x0bU : 0x0aU)),
				    2);
		}
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(ldo_cases); ++i) {
		policy = regulator_policy(MONACO_PMIC_LDO, ldo_cases[i].index);
		policy.enable_required = false;
		prepare_rail(&fake, &pmic, &policy);
		fake.registers[0][0x23U + policy.index] = ldo_cases[i].raw;
		int expected = ldo_cases[i].uv > policy.maximum_uv ? MONACO_PMIC_RANGE : 0;

		sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) == expected);
		sail_check(result.voltage_valid && result.voltage_uv == ldo_cases[i].uv);
		sail_check(fake.count == (expected ? 1U : 2U));
	}
	/* Exact equality and one-microvolt exclusions test inclusive comparison. */
	for (unsigned int group = 0; group < 2; ++group) {
		for (unsigned int bound = 0; bound < 3; ++bound) {
			policy = regulator_policy((enum monaco_pmic_regulator_group)group, 0);
			policy.minimum_uv = 600000U + (bound == 1 ? 1U : 0U);
			policy.maximum_uv = 600000U - (bound == 2 ? 1U : 0U);
			if (bound == 1) {
				policy.maximum_uv = 700000;
			} else if (bound == 2) {
				policy.minimum_uv = 500000;
			}
			policy.enable_required = false;
			prepare_rail(&fake, &pmic, &policy);
			sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) ==
			       (bound ? MONACO_PMIC_RANGE : 0));
			if (bound) {
				sail_check(fake.count == (group ? 2U : 1U));
				sail_check(result.stage == MONACO_PMIC_REGULATOR_VOLTAGE);
				sail_check(!result.mode_valid && !result.enable_valid);
			}
		}
	}
}

static void test_regulator_failures(void)
{
	struct fake fake;
	struct monaco_pmic pmic;
	struct monaco_pmic_regulator_result result;
	static const enum monaco_pmic_regulator_stage ldo_stages[] = {
		MONACO_PMIC_REGULATOR_VOLTAGE,        MONACO_PMIC_REGULATOR_ENABLE,
		MONACO_PMIC_REGULATOR_ENABLE_BIT,     MONACO_PMIC_REGULATOR_ENABLE_BIT,
		MONACO_PMIC_REGULATOR_ENABLE_CONTROL, MONACO_PMIC_REGULATOR_ENABLE_CONTROL,
		MONACO_PMIC_REGULATOR_SETTLE,
	};
	static const enum monaco_pmic_regulator_stage smps_stages[] = {
		MONACO_PMIC_REGULATOR_VOLTAGE,        MONACO_PMIC_REGULATOR_VOLTAGE,
		MONACO_PMIC_REGULATOR_MODE,           MONACO_PMIC_REGULATOR_ENABLE,
		MONACO_PMIC_REGULATOR_ENABLE_BIT,     MONACO_PMIC_REGULATOR_ENABLE_BIT,
		MONACO_PMIC_REGULATOR_ENABLE_CONTROL, MONACO_PMIC_REGULATOR_ENABLE_CONTROL,
		MONACO_PMIC_REGULATOR_SETTLE,
	};

	for (unsigned int group = 0; group < 2; ++group) {
		struct monaco_pmic_regulator_policy policy =
			regulator_policy((enum monaco_pmic_regulator_group)group, 0);
		unsigned int calls = group ? 9U : 7U;
		const enum monaco_pmic_regulator_stage *stages = group ? smps_stages : ldo_stages;

		for (unsigned int fail = 1; fail <= calls; ++fail) {
			for (unsigned int positive = 0; positive < 2; ++positive) {
				prepare_rail(&fake, &pmic, &policy);
				fake.fail_at = fail;
				fake.failure = positive ? 0x16 : TRANSPORT_ERROR;
				sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) ==
				       (positive ? MONACO_PMIC_CALLBACK : TRANSPORT_ERROR));
				sail_check(fake.count == fail && result.stage == stages[fail - 1U]);
				sail_check(!result.enable_sequence_complete);
			}
		}
		/* Every read can fail CRC, including both separate enable RMW reads. */
		for (unsigned int corrupt = 1; corrupt <= calls; ++corrupt) {
			prepare_rail(&fake, &pmic, &policy);
			sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) == 0);
			if (fake.events[corrupt - 1U].rx_size == 0) {
				continue;
			}
			prepare_rail(&fake, &pmic, &policy);
			fake.corrupt_at = corrupt;
			sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) ==
			       MONACO_PMIC_CRC);
			sail_check(fake.count == corrupt && result.stage == stages[corrupt - 1U]);
			sail_check(!result.enable_sequence_complete);
		}
	}
}

static void test_invalid_arguments(void)
{
	struct fake fake;
	struct monaco_pmic pmic;
	struct monaco_pmic_io io;
	struct monaco_pmic_regulator_policy policy = regulator_policy(MONACO_PMIC_LDO, 0);
	struct monaco_pmic_regulator_result result;
	uint8_t value = 0x5a;

	prepare_raw(&fake, &pmic);
	io = bus_io(&fake);
	sail_check(monaco_pmic_init(NULL, &io, TIMEOUT_US) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_init(&pmic, NULL, TIMEOUT_US) == MONACO_PMIC_INVALID);
	sail_check(pmic.state == MONACO_PMIC_UNINITIALIZED && !pmic.io.transfer);
	sail_check(monaco_pmic_init(&pmic, &io, 0) == MONACO_PMIC_INVALID);
	io.transfer = NULL;
	sail_check(monaco_pmic_init(&pmic, &io, TIMEOUT_US) == MONACO_PMIC_INVALID);
	io = bus_io(&fake);
	io.delay_us = NULL;
	sail_check(monaco_pmic_init(&pmic, &io, TIMEOUT_US) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_identify(&pmic) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_enable_crc(&pmic) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_identify(NULL) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_enable_crc(NULL) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_read(NULL, 0, 0, &value) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_write(NULL, 0, 0, 0) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_update_bits(NULL, 0, 0, 0, 0) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_regulator_init(NULL, &policy, &result) == MONACO_PMIC_INVALID);
	sail_check(result.stage == MONACO_PMIC_REGULATOR_INVALID && fake.count == 0);

	prepare_raw(&fake, &pmic);
	sail_check(monaco_pmic_enable_crc(&pmic) == MONACO_PMIC_STATE);
	sail_check(monaco_pmic_read(&pmic, 0, 0, &value) == MONACO_PMIC_STATE);
	sail_check(monaco_pmic_write(&pmic, 0, 0, 0) == MONACO_PMIC_STATE);
	sail_check(monaco_pmic_update_bits(&pmic, 0, 0, 0, 0) == MONACO_PMIC_STATE);
	sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) == MONACO_PMIC_STATE);
	sail_check(fake.count == 0 && value == 0x5a);
	sail_check(monaco_pmic_identify(&pmic) == 0);
	reset_log(&fake);
	sail_check(monaco_pmic_read(&pmic, 0, 0, &value) == MONACO_PMIC_STATE);
	sail_check(monaco_pmic_write(&pmic, 0, 0, 0) == MONACO_PMIC_STATE);
	sail_check(fake.count == 0);

	prepare_crc(&fake, &pmic);
	sail_check(monaco_pmic_read(&pmic, 0, 0, NULL) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_read(&pmic, 3, 0, &value) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_write(&pmic, UINT_MAX, 0, 0) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_update_bits(&pmic, 3, 0, 0, 0) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_regulator_init(&pmic, NULL, &result) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_regulator_init(&pmic, &policy, NULL) == MONACO_PMIC_INVALID);
	for (unsigned int group = 0; group < 2; ++group) {
		policy = regulator_policy((enum monaco_pmic_regulator_group)group, group ? 5U : 4U);
		sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) ==
			   MONACO_PMIC_INVALID);
		policy.index = UINT_MAX;
		sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) ==
			   MONACO_PMIC_INVALID);
		policy.index = 0;
		policy.required_mode = group ? 1 : 0;
		sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) ==
		       MONACO_PMIC_UNSUPPORTED);
	}
	policy = regulator_policy(MONACO_PMIC_LDO, 0);
	policy.group = (enum monaco_pmic_regulator_group)9;
	sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) == MONACO_PMIC_INVALID);
	policy = regulator_policy(MONACO_PMIC_LDO, 0);
	policy.minimum_uv = UINT32_MAX;
	sail_check(monaco_pmic_regulator_init(&pmic, &policy, &result) == MONACO_PMIC_INVALID);
	sail_check(fake.count == 0 && value == 0x5a);
	sail_check(result.stage == MONACO_PMIC_REGULATOR_INVALID && !result.voltage_valid);

	/* Invalid reinitialization cannot leave a previous CRC-ready context. */
	sail_check(monaco_pmic_init(&pmic, NULL, TIMEOUT_US) == MONACO_PMIC_INVALID);
	sail_check(monaco_pmic_write(&pmic, 0, 0, 0) == MONACO_PMIC_INVALID);
	sail_check(!pmic.version.device && fake.count == 0);
}

int sail_model_test(void)
{
	test_identity();
	test_crc_transition_failures();
	test_crc_vectors_and_frames();
	test_register_errors_and_rmw();
	test_regulator_enable_and_permission();
	test_voltage_boundaries();
	test_regulator_failures();
	test_invalid_arguments();
	puts("PMIC protocol and regulator tests passed");
	return 0;
}
