/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "post_pmic_clock.h"
#include "cpu_pll.h"
#include "sail_check.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

static const uint32_t commands[] = {0xf1124000, 0xf1114054, 0xf111001c, 0xf1111058};
static const uint32_t before[] = {0x201, 0x104, 0x105, 0x102};
static const uint32_t after[] = {0x201, 0x103, 0x104, 0x601};
static const struct monaco_pmic_regulator_policy policies[] = {
	{MONACO_PMIC_LDO, 0, true, true, 2, 600000, 3300000},
	{MONACO_PMIC_LDO, 1, true, false, 2, 600000, 3300000},
	{MONACO_PMIC_LDO, 2, false, false, 2, 600000, 3300000},
	{MONACO_PMIC_LDO, 3, true, true, 2, 1200000, 3300000},
	{MONACO_PMIC_SMPS, 0, false, false, 0, 300000, 3340000},
	{MONACO_PMIC_SMPS, 1, false, true, 0, 300000, 3340000},
	{MONACO_PMIC_SMPS, 2, true, true, 0, 300000, 3340000},
	{MONACO_PMIC_SMPS, 3, true, true, 0, 300000, 3340000},
	{MONACO_PMIC_SMPS, 4, true, true, 0, 300000, 3340000},
};
struct reg {
	uint32_t address, value;
};
struct fake {
	struct reg regs[80], writes[80];
	unsigned int count, written, accesses, fail_at, high_reads;
	uint64_t ticks;
	uint32_t hold_command, ignore_config;
	bool timer_failed, hold_pll4;
};

static uint32_t *reg(struct fake *f, uint32_t address)
{
	for (unsigned int i = 0; i < f->count; i++) {
		if (f->regs[i].address == address) {
			return &f->regs[i].value;
		}
	}
	sail_check(f->count < ARRAY_SIZE(f->regs));
	f->regs[f->count].address = address;
	return &f->regs[f->count++].value;
}

static bool fail(struct fake *f, uint32_t address)
{
	if (++f->accesses != f->fail_at) {
		return false;
	}
	f->timer_failed = address == 0xf1400000 || address == 0xf1400004;
	return true;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;

	if (fail(f, address)) {
		return -1;
	}
	if (address == 0xf1400010) {
		*value = 19200000;
		return 0;
	}
	if (address == 0xf1400000) {
		*value = (uint32_t)f->ticks;
		return 0;
	}
	if (address == 0xf1400004) {
		*value = (uint32_t)(f->ticks >> 32);
		if (++f->high_reads % 2 == 0) {
			f->ticks += 20;
		}
		return 0;
	}
	sail_check(monaco_clock_write_allowed(address) || monaco_cpu_pll_write_allowed(address));
	uint32_t *r = reg(f, address);

	for (unsigned int i = 0; i < ARRAY_SIZE(commands); i++) {
		if (address == commands[i] && address != f->hold_command) {
			*r &= ~1U;
		}
	}
	if (address == 0xf1104000 && (*r & 4U) && !f->hold_pll4) {
		*r |= 0x80000000U;
	}
	*value = *r;
	return 0;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;
	bool allowed = address == 0xf1101004 || address == 0xf1101008 || address == 0xf1101010 ||
		       (address >= 0xf1104000 && address <= 0xf1104034 &&
			monaco_cpu_pll_write_allowed(address));
	for (unsigned int i = 0; i < ARRAY_SIZE(commands); i++) {
		allowed |= address == commands[i] || address == commands[i] + 4;
	}
	sail_check(allowed);
	if (fail(f, address)) {
		return -1;
	}
	sail_check(f->written < ARRAY_SIZE(f->writes));
	f->writes[f->written++] = (struct reg){address, value};
	if (address != f->ignore_config) {
		*reg(f, address) = value;
	}
	return 0;
}

static struct monaco_clock prepare(struct fake *f, struct monaco_timer *timer,
				   struct monaco_pmic *pmic, struct monaco_pmic_boot_result *boot)
{
	*f = (struct fake){0};
	struct monaco_timer_io io = {f, read32, write32};

	sail_check(monaco_timer_init(timer, &io, 8) == 0);
	f->accesses = 0;
	*pmic = (struct monaco_pmic){.state = MONACO_PMIC_CRC_READY, .version.device = 0x82};
	*boot = (struct monaco_pmic_boot_result){.stage = MONACO_PMIC_BOOT_COMPLETE,
						 .regulators_completed = 9,
						 .last_operation = MONACO_PMIC_BOOT_DELAY,
						 .last_delay_us = 3000,
						 .delay_us_completed = 5000};
	for (unsigned int i = 0; i < MONACO_PMIC_BOOT_REGULATORS; i++) {
		boot->regulators[i] = (struct monaco_pmic_regulator_result){
			.stage = MONACO_PMIC_REGULATOR_COMPLETE,
			.voltage_valid = true,
			.mode_valid = true,
			.enable_valid = true,
			.observed_enabled = true,
			.voltage_uv = policies[i].minimum_uv,
			.observed_mode = policies[i].required_mode};
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(commands); i++) {
		*reg(f, commands[i] + 4) = 0xa5a0c000U | before[i];
	}
	*reg(f, 0xf1100000) = 0x80000005;
	*reg(f, 0xf1100004) = 1;
	*reg(f, 0xf1100010) = 31;
	*reg(f, 0xf1100014) = 0x4000;
	*reg(f, 0xf1100018) = 0x401;
	*reg(f, 0xf1101000) = 0x80000007;
	*reg(f, 0xf1101004) = 83;
	*reg(f, 0xf110100c) = 1;
	*reg(f, 0xf1101010) = 0x02000000;
	return (struct monaco_clock){io, timer, 4};
}

static unsigned int first_write(const struct fake *f, uint32_t address)
{
	for (unsigned int i = 0; i < f->written; i++) {
		if (f->writes[i].address == address) {
			return i;
		}
	}
	return UINT32_MAX;
}

struct pmic_model {
	uint8_t regs[256];
	unsigned int writes[256], transfers, raw_reads, time_reads;
	uint32_t delays[16];
	unsigned int delay_count;
	uint64_t elapsed_us;
	bool crc_enabled;
};

static uint8_t frame_crc(const uint8_t *data, size_t size)
{
	unsigned int remainder = 0xff;

	for (size_t i = 0; i < size; i++) {
		remainder ^= data[i];
		for (unsigned int bit = 0; bit < 8; bit++) {
			remainder <<= 1;
			if (remainder & 0x100U) {
				remainder ^= 0x107U;
			}
		}
	}
	return (uint8_t)remainder;
}

static int pmic_transfer(void *context, uint8_t address, const uint8_t *tx, size_t tx_size,
			 uint8_t *rx, size_t rx_size, uint32_t timeout)
{
	static const uint8_t identity[] = {1, 2, 3, 0xa7};
	struct pmic_model *m = context;
	uint8_t covered[4] = {0x50, tx[0], 0x51, 0};

	sail_check(address == 0x28 && timeout == 1000);
	m->transfers++;
	if (!m->crc_enabled) {
		if (rx_size) {
			sail_check(tx_size == 1 && rx_size == 1 && rx);
			sail_check(m->raw_reads < ARRAY_SIZE(identity));
			sail_check(tx[0] == identity[m->raw_reads++]);
			rx[0] = m->regs[tx[0]];
		} else {
			sail_check(tx_size == 2 && !rx && tx[0] == 0x85 && tx[1] == 2);
			sail_check(m->raw_reads == ARRAY_SIZE(identity));
			m->crc_enabled = true;
			m->writes[tx[0]]++;
		}
		return 0;
	}
	sail_check(m->elapsed_us >= 2000);
	if (rx_size) {
		sail_check(tx_size == 1 && rx_size == 2 && rx);
		rx[0] = m->regs[tx[0]];
		covered[3] = rx[0];
		rx[1] = frame_crc(covered, 4);
	} else {
		sail_check(tx_size == 3 && !rx);
		covered[2] = tx[1];
		sail_check(tx[2] == frame_crc(covered, 3));
		m->regs[tx[0]] = tx[1];
		m->writes[tx[0]]++;
	}
	return 0;
}

static int pmic_delay(void *context, uint32_t us)
{
	struct pmic_model *m = context;

	sail_check(m->delay_count < ARRAY_SIZE(m->delays));
	m->delays[m->delay_count++] = us;
	m->elapsed_us += us;
	return 0;
}

static int pmic_time(void *context, uint64_t *us)
{
	struct pmic_model *m = context;

	m->time_reads++;
	*us = m->elapsed_us;
	return 0;
}

static void test_real_pmic_chain(void)
{
	struct pmic_model m = {0};
	struct fake f;
	struct monaco_timer timer;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_result boot;
	struct monaco_post_pmic_clock_status status;
	struct monaco_clock c = prepare(&f, &timer, &pmic, &boot);
	struct monaco_pmic_io io = {&m, pmic_transfer, pmic_delay};
	struct monaco_pmic_boot_time time = {&m, pmic_time};
	struct monaco_pmic_pretarget_inputs pretarget = {
		.topology = MONACO_PMIC_TOPOLOGY_INDEPENDENT,
		.present_slaves = 7,
		.reset_fsm_valid = true,
		.reset_fsm = 0,
		.platform_type_valid = true,
		.platform_type = 0,
		.parameter_status_policy_validated = true,
	};
	struct monaco_pmic_tail_inputs tail = {
		.regulator_posttarget_policy_validated = true,
		.regulators = policies,
		.regulator_count = ARRAY_SIZE(policies),
	};
	struct monaco_pmic_regulator_result saved[MONACO_PMIC_BOOT_REGULATORS];
	static const uint8_t crc_vector[] = {0x50, 0x66, 0x51, 0x01};
	unsigned int transfers;

	sail_check(frame_crc(crc_vector, sizeof(crc_vector)) == 0x8b);
	m.regs[1] = 0x82;
	m.regs[2] = 0xa5;
	m.regs[3] = 2;
	m.regs[0x66] = 1;
	m.regs[0x81] = 8;
	for (unsigned int i = 0; i < 3; i++) {
		m.regs[0x23 + i] = 8;
	}
	m.regs[0x26] = 32;
	for (unsigned int i = 0; i < 5; i++) {
		m.regs[0x0e + 2 * i] = 20;
	}
	sail_check(monaco_pmic_init(&pmic, &io, 1000) == 0);
	sail_check(monaco_pmic_identify(&pmic) == 0);
	sail_check(monaco_pmic_pretarget(&pmic, &pretarget, MONACO_PMIC_REQUIRE_BOOT_BIST, &boot) ==
		   0);
	sail_check(boot.boot_bist_accepted && !boot.runtime_bist_attempted);
	sail_check(monaco_pmic_boot_tail(&pmic, &tail, &boot) == 0);
	sail_check(boot.regulators_completed == 9 && boot.posttarget_writes_completed == 6);
	sail_check(boot.regulators[5].enable_skipped && !boot.regulators[5].observed_enabled);
	sail_check(!boot.regulators[5].enable_sequence_complete);
	sail_check(m.regs[6] == 0 && m.writes[6] == 0);
	sail_check(m.writes[0x1d] == 2 && m.writes[0x20] == 2);
	for (unsigned int i = 2; i < 5; i++) {
		sail_check(m.writes[4 + 2 * i] == 2);
	}
	sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
					   &status) == MONACO_CLOCK_INVALID);
	sail_check(!f.accesses && !f.written);
	sail_check(monaco_pmic_boot_finish(&pmic, &time, &boot) == 0);
	sail_check(boot.settle_elapsed_valid && boot.settle_elapsed_us == 3000);
	sail_check(m.time_reads == 2 && m.delay_count == 7);
	sail_check(m.delays[0] == 2000 && m.delays[6] == 3000);
	for (unsigned int i = 1; i < 6; i++) {
		sail_check(m.delays[i] == 1000);
	}
	memcpy(saved, boot.regulators, sizeof(saved));
	transfers = m.transfers;
	sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
					   &status) == 0);
	sail_check(status.completed_domains == 4);
	sail_check(memcmp(saved, boot.regulators, sizeof(saved)) == 0);
	sail_check(m.transfers == transfers && m.time_reads == 2 && m.delay_count == 7);
	sail_check(m.regs[6] == 0 && m.writes[6] == 0);
}

static void test_policy_consistency(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_result boot;
	struct monaco_post_pmic_clock_status status;
	struct monaco_pmic_regulator_policy selected[ARRAY_SIZE(policies)];
	struct monaco_clock c;

	/* Every enable-request, permission, observation and outcome combination. */
	for (unsigned int i = 0; i < ARRAY_SIZE(policies); i++) {
		for (unsigned int bits = 0; bits < 32; bits++) {
			bool required = (bits & 1U) != 0;
			bool permitted = (bits & 2U) != 0;
			bool enabled = (bits & 4U) != 0;
			bool skipped = (bits & 8U) != 0;
			bool complete = (bits & 16U) != 0;
			bool valid = (required && !enabled) ? (permitted ? (complete && !skipped)
									 : (skipped && !complete))
							    : (!skipped && !complete);

			c = prepare(&f, &timer, &pmic, &boot);
			memcpy(selected, policies, sizeof(selected));
			selected[i].enable_required = required;
			selected[i].write_permitted = permitted;
			boot.regulators[i].observed_enabled = enabled;
			boot.regulators[i].enable_skipped = skipped;
			boot.regulators[i].enable_sequence_complete = complete;
			sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, selected,
						       ARRAY_SIZE(selected), &status) ==
			       (valid ? 0 : MONACO_CLOCK_INVALID));
			sail_check(boot.regulators[i].enable_skipped == skipped);
			if (!valid) {
				sail_check(!f.accesses && !f.written);
			}
		}
		for (unsigned int fault = 0; fault < 9; fault++) {
			c = prepare(&f, &timer, &pmic, &boot);
			memcpy(selected, policies, sizeof(selected));
			switch (fault) {
			case 0:
				selected[i].group = (enum monaco_pmic_regulator_group)99;
				break;
			case 1:
				selected[i].index++;
				break;
			case 2:
				selected[i].maximum_uv = selected[i].minimum_uv - 1;
				break;
			case 3:
				selected[i].required_mode ^= 1;
				break;
			case 4:
				boot.regulators[i].voltage_uv = selected[i].minimum_uv - 1;
				break;
			case 5:
				boot.regulators[i].voltage_uv = selected[i].maximum_uv + 1;
				break;
			case 6:
				boot.regulators[i].observed_mode = i < 4 ? 0 : 2;
				break;
			case 7:
				selected[i].group = i < 4 ? MONACO_PMIC_SMPS : MONACO_PMIC_LDO;
				break;
			case 8:
				boot.regulators[i].enable_sequence_complete = true;
				break;
			default:
				sail_check(false);
			}
			sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, selected,
						       ARRAY_SIZE(selected),
						       &status) == MONACO_CLOCK_INVALID);
			sail_check(!f.accesses && !f.written);
		}
		for (unsigned int upper = 0; upper < 2; upper++) {
			c = prepare(&f, &timer, &pmic, &boot);
			boot.regulators[i].voltage_uv =
				upper ? policies[i].maximum_uv : policies[i].minimum_uv;
			if (i >= 4) {
				boot.regulators[i].observed_mode = 1;
			}
			sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies,
						       ARRAY_SIZE(policies), &status) == 0);
		}
	}
	for (unsigned int count = 0; count <= 10; count++) {
		if (count == 9) {
			continue;
		}
		c = prepare(&f, &timer, &pmic, &boot);
		sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, count, &status) ==
		       MONACO_CLOCK_INVALID);
		sail_check(!f.accesses && !f.written);
	}
	c = prepare(&f, &timer, &pmic, &boot);
	sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, NULL, ARRAY_SIZE(policies), &status) ==
	       MONACO_CLOCK_INVALID);
	sail_check(!f.accesses && !f.written);
}

int sail_model_test(void)
{
	struct fake f;
	struct monaco_timer timer;
	struct monaco_pmic pmic;
	struct monaco_pmic_boot_result boot;
	struct monaco_post_pmic_clock_status status;
	struct monaco_clock c = prepare(&f, &timer, &pmic, &boot);

	sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
					   &status) == 0);
	sail_check(status.stage == MONACO_POST_PMIC_CLOCK_COMPLETE &&
		   status.completed_domains == 4);
	sail_check((*reg(&f, 0xf1101004) & 0xffU) == 96);
	sail_check(*reg(&f, 0xf1104010) == 26 && *reg(&f, 0xf1104014) == 0x0aaa);
	sail_check(*reg(&f, 0xf1104018) == 1);
	for (unsigned int i = 0; i < ARRAY_SIZE(commands); i++) {
		sail_check(*reg(&f, commands[i] + 4) == (0xa5a0c000U | after[i]));
		sail_check(first_write(&f, commands[i] + 4) < first_write(&f, commands[i]));
		if (i) {
			sail_check(first_write(&f, commands[i - 1]) <
				   first_write(&f, commands[i] + 4));
		}
	}
	sail_check(first_write(&f, 0xf1101010) < first_write(&f, commands[0] + 4));
	sail_check(first_write(&f, commands[2]) < first_write(&f, 0xf1104020));
	sail_check(first_write(&f, 0xf1104000) < first_write(&f, commands[3] + 4));
	unsigned int total = f.accesses;

	for (unsigned int failure = 1; failure <= total; failure++) {
		c = prepare(&f, &timer, &pmic, &boot);
		f.fail_at = failure;
		int rc = monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
						 &status);
		sail_check(rc == (f.timer_failed ? MONACO_CLOCK_TIMEBASE : MONACO_CLOCK_IO));
		sail_check(f.accesses == failure &&
			   status.stage != MONACO_POST_PMIC_CLOCK_COMPLETE);
	}
	for (unsigned int i = 0; i < ARRAY_SIZE(commands); i++) {
		c = prepare(&f, &timer, &pmic, &boot);
		*reg(&f, commands[i] + 4) ^= 1;
		sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
					       &status) == MONACO_CLOCK_STATE);
		sail_check(f.written == 0 && status.address == commands[i] + 4);
		c = prepare(&f, &timer, &pmic, &boot);
		f.hold_command = commands[i];
		sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
					       &status) == MONACO_CLOCK_POLL_LIMIT);
		sail_check(status.address == commands[i] && status.completed_domains == i);
	}
	c = prepare(&f, &timer, &pmic, &boot);
	*reg(&f, 0xf1100000) = 0;
	sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
					   &status) == MONACO_CLOCK_STATE);
	sail_check(f.written == 0);
	c = prepare(&f, &timer, &pmic, &boot);
	f.ignore_config = commands[2] + 4;
	sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
					   &status) == MONACO_CLOCK_STATE);
	sail_check(status.stage == MONACO_POST_PMIC_CLOCK_ASILB && status.completed_domains == 2);
	c = prepare(&f, &timer, &pmic, &boot);
	f.hold_pll4 = true;
	sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
					   &status) == MONACO_CLOCK_POLL_LIMIT);
	sail_check(status.stage == MONACO_POST_PMIC_CLOCK_DMA_SOURCE &&
		   status.completed_domains == 3);
	sail_check(first_write(&f, commands[3]) == UINT32_MAX);
	for (unsigned int fault = 0; fault < 8; fault++) {
		c = prepare(&f, &timer, &pmic, &boot);
		switch (fault) {
		case 0:
			pmic.state = MONACO_PMIC_IDENTIFIED;
			break;
		case 1:
			pmic.version.device = 0;
			break;
		case 2:
			boot.status = -1;
			break;
		case 3:
			boot.stage = MONACO_PMIC_BOOT_SKIP_COLD;
			break;
		case 4:
			boot.regulators_completed = 8;
			break;
		case 5:
			boot.last_delay_us = 2999;
			break;
		case 6:
			boot.last_callback_status = -1;
			break;
		case 7:
			boot.delay_us_completed = 4999;
			break;
		default:
			sail_check(false);
		}
		sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies, ARRAY_SIZE(policies),
					       &status) == MONACO_CLOCK_INVALID);
		sail_check(!f.accesses && !f.written);
	}
	sail_check(monaco_post_pmic_clocks(NULL, NULL, NULL, NULL, 0, NULL) ==
		   MONACO_CLOCK_INVALID);
	for (unsigned int i = 0; i < MONACO_PMIC_BOOT_REGULATORS; i++) {
		for (unsigned int fault = 0; fault < 5; fault++) {
			c = prepare(&f, &timer, &pmic, &boot);
			struct monaco_pmic_regulator_result *rail = &boot.regulators[i];

			switch (fault) {
			case 0:
				rail->stage = MONACO_PMIC_REGULATOR_ENABLE;
				break;
			case 1:
				rail->voltage_valid = false;
				break;
			case 2:
				rail->mode_valid = false;
				break;
			case 3:
				rail->enable_valid = false;
				break;
			case 4:
				rail->enable_skipped = true;
				break;
			default:
				sail_check(false);
			}
			sail_check(monaco_post_pmic_clocks(&c, &pmic, &boot, policies,
						       ARRAY_SIZE(policies),
						       &status) == MONACO_CLOCK_INVALID);
			sail_check(!f.accesses && !f.written);
		}
	}
	test_policy_consistency();
	test_real_pmic_chain();
	printf("Post-PMIC clocks: real denied-SMPS1 tail/settle, policy consistency, "
	       "ordered four-rate transition and %u access faults passed\n",
	       total);
	return 0;
}
