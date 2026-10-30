/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cold_boot.h"
#include "cpu_pll.h"

#include "sail_check.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define SE            UINT32_C(0xf1980000)
#define IO_ERROR      (-71)

/* Synthetic peripheral model; every component entry point is the real one. */
struct reg {
	uint32_t address, value;
};
struct access {
	uint32_t address;
	enum monaco_cold_boot_stage stage;
	bool write;
};
struct clock_event {
	char kind;
	uint32_t address, value;
};
static struct clock_event clock_trace[81];
static unsigned int clock_events;

struct model {
	struct reg registers[512];
	struct access trace[32768];
	unsigned int register_count, count, writes, fail_at, high_reads;
	uint32_t cpsr, mpidr, frequency, identity_drift;
	uint64_t ticks;
	bool fault_after_effect, stopped_timer, crc_enabled;
	bool runtime_success, runtime_started, runtime_second_flag;
	bool stop_on_trigger, reverse_on_trigger, runtime_fail_read;
	uint32_t runtime_triggers, runtime_fail_reg, runtime_faults, pmic_reads[256];
	uint8_t pmic[256], address, probe_addresses[3];
	unsigned int pmic_writes[256], probes, transactions, raw_reads;
	unsigned int profile_writes;
	unsigned int publication_calls, publication_effects;
	uint32_t publication_words[5];
	bool with_publication, phase2_ready;
	enum md_bist_access_contract access_contract;
	struct monaco_cold_boot *boot;
};

static uint32_t *reg(struct model *m, uint32_t address)
{
	for (unsigned int i = 0; i < m->register_count; i++) {
		if (m->registers[i].address == address) {
			return &m->registers[i].value;
		}
	}
	sail_check(m->register_count < ARRAY_SIZE(m->registers));
	m->registers[m->register_count].address = address;
	return &m->registers[m->register_count++].value;
}

static bool known(const struct model *m, uint32_t address)
{
	for (unsigned int i = 0; i < m->register_count; i++) {
		if (m->registers[i].address == address) {
			return true;
		}
	}
	return false;
}

static bool command(uint32_t address)
{
	if (address == 0xf1115040) {
		return true;
	}
	for (unsigned int i = 0; i < clock_events; i++) {
		if (clock_trace[i].kind == 'R' && clock_trace[i].address == address) {
			return true;
		}
	}
	return false;
}

static bool record(struct model *m, uint32_t address, bool write)
{
	sail_check(m->count < ARRAY_SIZE(m->trace));
	if (m->count) {
		sail_check(m->trace[m->count - 1].stage <= m->boot->stage);
	}
	m->trace[m->count++] = (struct access){address, m->boot->stage, write};
	return m->count == m->fail_at;
}

static uint8_t crc(const uint8_t *data, size_t size)
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

static void pmic_command(struct model *m, uint32_t word)
{
	uint32_t tx = *reg(m, SE + 0x26c), rx = *reg(m, SE + 0x270);
	uint8_t offset = (uint8_t)word, value = (uint8_t)(word >> 8);
	uint8_t covered[] = {0x50, offset, 0x51, m->pmic[offset]};

	sail_check(m->address == 0x28 && tx >= 1 && tx <= 3 && rx <= 2);
	m->transactions++;
	if (rx > 0U) {
		m->pmic_reads[offset]++;
	}
	if (m->runtime_started && m->runtime_faults == 0U && offset == m->runtime_fail_reg &&
	    ((rx > 0U) == m->runtime_fail_read)) {
		/* Fail the completion read of this actual PMIC transfer. */
		m->fail_at = m->count + 1U;
		m->runtime_faults++;
	}
	if (rx) {
		sail_check(tx == 1 && rx == (m->crc_enabled ? 2U : 1U));
		if (!m->crc_enabled) {
			m->raw_reads++;
		}
		*reg(m, SE + 0x780) =
			m->pmic[offset] | (rx == 2 ? (uint32_t)crc(covered, 4) << 8 : 0);
		*reg(m, SE + 0x804) = UINT32_C(0x80000001) | rx << 28;
	} else {
		if (!m->crc_enabled) {
			sail_check(tx == 2 && offset == 0x85 && value == 2);
			sail_check(m->raw_reads == 4);
			m->crc_enabled = true;
		} else {
			sail_check(tx == 3);
			covered[2] = value;
			sail_check((uint8_t)(word >> 16) == crc(covered, 3));
		}
		m->pmic[offset] = value;
		m->pmic_writes[offset]++;
		if (offset == 0x85U && value == 4U) {
			m->runtime_triggers++;
			m->runtime_started = true;
			if (m->runtime_success) {
				m->pmic[0x66] = 1U;
				m->pmic[0x81] = m->runtime_second_flag ? 8U : 0U;
			}
			if (m->stop_on_trigger) {
				m->stopped_timer = true;
			}
			if (m->reverse_on_trigger) {
				m->ticks = 0U;
			}
		}
	}
	*reg(m, SE + 0x610) = 1;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct model *m = context;
	bool failed = record(m, address, false);

	if (failed) {
		*value = UINT32_MAX;
		return IO_ERROR;
	}
	if (address == 0xf1400010) {
		*value = m->frequency;
		return 0;
	}
	if (address == 0xf1400000) {
		*value = (uint32_t)m->ticks;
		return 0;
	}
	if (address == 0xf1400004) {
		*value = (uint32_t)(m->ticks >> 32);
		if (++m->high_reads % 2 == 0 && !m->stopped_timer) {
			m->ticks += 192;
		}
		return 0;
	}
	if (m->boot->stage == MONACO_COLD_MD_BIST && address == 0xf03050c4) {
		*value = m->phase2_ready ? 5U : 1U;
		return 0;
	}
	if (!known(m, address)) {
		fprintf(stderr, "unmodeled read %08x at stage %u\n", address,
			(unsigned int)m->boot->stage);
		sail_check(false);
	}
	uint32_t *r = reg(m, address);

	if (command(address)) {
		*r &= ~UINT32_C(1);
	}
	if (address == 0xf1104000 && (*r & 4U)) {
		*r |= UINT32_C(0x80000000);
	}
	if (address == 0xf1115038 && (*r & 1U)) {
		*r &= ~UINT32_C(0xd0000000);
	}
	*value = *r;
	if (address == SE + 0x780) {
		*reg(m, SE + 0x804) = 0;
	}
	if (m->boot->stage == MONACO_COLD_PMIC_SAMPLE && address == m->identity_drift) {
		*value ^= 1U;
	}
	return 0;
}

static bool write_allowed(const struct model *m, uint32_t address)
{
	switch (m->boot->stage) {
	case MONACO_COLD_MD_BIST:
		return md_bist_write_allowed(address);
	case MONACO_COLD_TIMER:
		return address == 0xf140c000;
	case MONACO_COLD_EARLY_CLOCKS:
	case MONACO_COLD_POST_PMIC_CLOCKS:
		return monaco_clock_write_allowed(address) || monaco_cpu_pll_write_allowed(address);
	case MONACO_COLD_HBCU:
		return address == 0xf8800004 ||
		       (address >= 0xf0305024 && address <= 0xf0305038 && !(address & 3U));
	case MONACO_COLD_I2C:
		return address == 0xf1115040 || address == 0xf1115044 || address == 0xf1115038 ||
		       monaco_i2c_profile_write_allowed(address) ||
		       monaco_i2c_write_allowed(address);
	case MONACO_COLD_PMIC_SAMPLE:
	case MONACO_COLD_PMIC_IDENTIFY:
	case MONACO_COLD_PMIC_PRETARGET:
	case MONACO_COLD_PMIC_TAIL:
		return address == SE + 0x26c || address == SE + 0x270 || address == SE + 0x600 ||
		       address == SE + 0x604 || address == SE + 0x618 || address == SE + 0x700;
	default:
		return false;
	}
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct model *m = context;

	sail_check(write_allowed(m, address));
	bool failed = record(m, address, true);

	if (failed && !m->fault_after_effect) {
		return IO_ERROR;
	}
	m->writes++;
	if (m->boot->stage == MONACO_COLD_MD_BIST && address == 0xf1000000) {
		*reg(m, address) = value;
		*reg(m, 0xf1000008) = value & 2U ? 8U : 0;
	} else if (m->boot->stage == MONACO_COLD_MD_BIST &&
		   (address == 0xf1110700 || address == 0xf1111700)) {
		*reg(m, address) =
			value & 1U ? value | UINT32_C(0x80000000) : value & ~UINT32_C(0x80000000);
	} else if (address == SE + 0x618) {
		*reg(m, SE + 0x610) &= ~value;
	} else if (address == SE + 0x600) {
		m->address = (uint8_t)((value >> 9) & 0x7fU);
		if ((value >> 27) == 4) {
			sail_check(m->probes < ARRAY_SIZE(m->probe_addresses));
			m->probe_addresses[m->probes++] = m->address;
			*reg(m, SE + 0x610) = 1;
		}
	} else if (address == SE + 0x604) {
		sail_check(value == 4 || value == 2);
		*reg(m, SE + 0x610) = value == 4 ? 0x10U : 0x20U;
		*reg(m, SE + 0x804) = 0;
	} else if (address == SE + 0x700) {
		pmic_command(m, value);
	} else {
		*reg(m, address) = value;
	}
	if (m->boot->stage == MONACO_COLD_I2C &&
	    m->boot->i2c_setup.stage == MONACO_I2C_BOOT_PROFILE) {
		m->profile_writes++;
		if (address == SE + 0x2c4 && m->profile_writes == 75) {
			*reg(m, SE + 0x68) = 0x303;
		}
	}
	return failed ? IO_ERROR : 0;
}

static int publication_write(void *context, uint32_t address, uint32_t value)
{
	static const uint8_t bytes[5][4] = {
		{0, 0, 1, 0xba}, {4, 0, 1, 0xba}, {2, 0, 1, 0xba}, {5, 0, 1, 0xba}, {3, 0, 1, 0xba},
	};
	struct model *m = context;
	unsigned int n = m->publication_calls++;

	sail_check(m->boot->stage == MONACO_COLD_MD_BIST && n < 20);
	sail_check(address == 0xf0305084 + 4 * (n % 4));
	sail_check(value == bytes[n / 4][n % 4]);
	sail_check(md_publication_write_allowed(address, value));
	sail_check(!md_bist_write_allowed(address));
	bool failed = record(m, address, true);

	if (failed && !m->fault_after_effect) {
		return IO_ERROR;
	}
	m->publication_words[n / 4] |= value << (8 * (n % 4));
	m->publication_effects++;
	m->writes++;
	if (n == 11) {
		m->phase2_ready = true;
	}
	return failed ? IO_ERROR : 0;
}

static int read_mpidr(void *context, uint32_t *value)
{
	struct model *m = context;
	*value = m->mpidr;
	return record(m, 1, false) ? IO_ERROR : 0;
}

static int read_cpsr(void *context, uint32_t *value)
{
	struct model *m = context;
	*value = m->cpsr;
	return record(m, 2, false) ? IO_ERROR : 0;
}

static void seed_pll(struct model *m, uint32_t base)
{
	if (base == 0xf1100000 || base == 0xf1102000) {
		*reg(m, base) = 0x80000005;
		*reg(m, base + 4) = 1;
		*reg(m, base + 0x10) = base == 0xf1100000 ? 31U : 34U;
		*reg(m, base + 0x14) = base == 0xf1100000 ? 0x4000U : 0x9555U;
		*reg(m, base + 0x18) = base == 0xf1100000 ? 0x401U : 1U;
		*reg(m, base + 0x1c) = 0;
	} else if (base == 0xf1101000) {
		*reg(m, base) = 0x80000007;
		*reg(m, base + 4) = 83;
		*reg(m, base + 8) = 0;
		*reg(m, base + 0xc) = 1;
		*reg(m, base + 0x10) = 0x02000000;
	} else {
		*reg(m, base) = 0x80001005;
		*reg(m, base + 4) = 1;
		*reg(m, base + 0x10) = 2;
		*reg(m, base + 0x14) = 0x403;
		*reg(m, base + 0x18) = 0x200000;
	}
}

static void prepare(struct model *m, struct monaco_cold_boot *boot, bool skip)
{
	*boot = (struct monaco_cold_boot){0};
	*m = (struct model){.cpsr = 0xdf,
			    .mpidr = 0x80000000,
			    .frequency = 19200000,
			    .boot = boot,
			    .runtime_fail_reg = 256U,
			    .runtime_second_flag = true,
			    .access_contract = MD_BIST_ACCESS_PBL_COLD_INHERITED_DEVICE};
	const struct reg initial[] = {
		{0xf0146014, 0x2e7},      {0xf0300028, 0x601a0100},
		{0xf1149024, 3},          {0xf03050c0, 0xcb},
		{0xf03050b0, 0xcb},       {0xf03050ac, 0},
		{0xf03050a8, 0},          {0xf03050a4, 0x13},
		{0xf8706100, 1U << 11},   {0xc0786064, skip ? 1U << 8 : 0},
		{0xc1ff900c, 3},          {0xf1115054, 0},
		{0xf1115040, 0},          {0xf1115044, 0x301},
		{0xf1115038, 0x80000000}, {0xf19c0004, 0x300b0000},
		{0xf19c0120, 0},          {0xf19c0118, 0},
		{SE + 0xe24, 0x20100000}, {SE + 0xe28, 0x20100000},
		{SE + 0xe2c, 0x1000},     {SE + 0x40, 0},
		{SE + 0x68, 0xffff},      {SE + 0x800, 0},
		{SE + 0x804, 0},          {SE + 0x80, 0},
		{SE + 0x610, 0},          {0xf1000000, 0},
		{0xf1000008, 0},          {0xf1000034, 0},
		{0xf03050c4, 1},          {0xf03050c8, 0},
		{0xc1ffa01c, 0},          {0xf1110700, 0x80000001},
		{0xf1111700, 0x80000001}, {0xf111d020, 0},
		{0xf111101c, 0x10},       {0xf1110050, 0x20},
		{0xf0301004, 0x80},       {0xf0305010, 0xff},
		{0xf0305014, 0xa7},       {0xf8800004, 1},
		{0xf8800068, 1},          {0xf880006c, 1},
		{0xf8800070, 0x80000000}, {0xf8800074, 1},
		{0xf8800078, 1},          {0xf880007c, 0x80000000},
	};
	for (size_t i = 0; i < ARRAY_SIZE(initial); i++) {
		*reg(m, initial[i].address) = initial[i].value;
	}
	for (unsigned int i = 0; i < clock_events; i++) {
		const struct clock_event *e = &clock_trace[i];
		*reg(m, e->address) = e->kind == 'R' ? 0x80U : 0x20U;
		if (e->kind == 'R') {
			*reg(m, e->address + 4) = 0xa000c000U | e->value;
		}
	}
	const uint32_t pll[] = {0xf1100000, 0xf1101000, 0xf1102000, 0xf1105000, 0xf1106000};

	for (size_t i = 0; i < ARRAY_SIZE(pll); i++) {
		seed_pll(m, pll[i]);
	}
	for (uint32_t a = 0xf1104000; a <= 0xf1104034; a += 4) {
		*reg(m, a) = 0;
	}
	m->pmic[1] = 0x82;
	m->pmic[2] = 0xa5;
	m->pmic[3] = 2;
	m->pmic[0x66] = 1;
	m->pmic[0x81] = 8;
	for (unsigned int i = 0; i < 3; i++) {
		m->pmic[0x23 + i] = 8;
	}
	m->pmic[0x26] = 32;
	for (unsigned int i = 0; i < 5; i++) {
		m->pmic[0x0e + 2 * i] = 20;
	}
}

static int run(struct model *m, struct monaco_cold_boot *boot)
{
	const struct monaco_cold_boot_io io = {
		.context = m,
		.read32 = read32,
		.write32 = write32,
		.read_mpidr = read_mpidr,
		.read_cpsr = read_cpsr,
		.md_access_contract = m->access_contract,
		.md_publication = {m, m->with_publication ? publication_write : NULL},
	};
	struct monaco_cold_boot_limits limits = monaco_cold_boot_defaults;

	limits.timer_read_budget = m->with_publication ? 5000U : 1000U;
	limits.md_bist.max_poll_reads = 32;
	limits.clock_poll_reads = 16;
	return monaco_cold_boot_run(boot, &io, &limits);
}

static void success_and_faults(void)
{
	struct model good, m;
	struct monaco_cold_boot boot;

	prepare(&good, &boot, true);
	sail_check(run(&good, &boot) == MONACO_COLD_PCINIT_REQUIRED);
	sail_check(boot.stage == MONACO_COLD_PCINIT && boot.md_bist_skipped);
	sail_check(boot.early_clocks.completed_branches == 62);
	sail_check(boot.board.stage == MONACO_BOARD_COMPLETE);
	sail_check(boot.hbcu.mbist.outcome == MONACO_HBCU_PASSED);
	sail_check(boot.hbcu.lbist.outcome == MONACO_HBCU_PASSED);
	sail_check(boot.i2c_setup.completed_stages == 3 && good.profile_writes == 75);
	sail_check(good.probes == 3 && good.probe_addresses[0] == 0x28 &&
	       good.probe_addresses[1] == 0x29 && good.probe_addresses[2] == 0x12);
	sail_check(good.raw_reads == 4 && good.crc_enabled);
	sail_check(boot.pmic_boot.boot_bist_accepted && !boot.pmic_boot.runtime_bist_attempted);
	sail_check(boot.pmic_boot.stage == MONACO_PMIC_BOOT_COMPLETE);
	sail_check(boot.pmic_boot.regulators_completed == 9 &&
	       boot.pmic_boot.posttarget_writes_completed == 6);
	sail_check(boot.pmic_boot.settle_elapsed_valid && boot.pmic_boot.settle_elapsed_us >= 3000);
	sail_check(boot.pmic_boot.regulators[5].enable_skipped && good.pmic_writes[6] == 0);
	sail_check(boot.post_clocks.completed_domains == 4);
	sail_check(boot.timer.io.context == &boot && boot.clock.timer == &boot.timer);
	sail_check(boot.i2c.io.context == &boot && boot.i2c.timer == &boot.timer);
	sail_check(boot.pmic.io.context == &boot);
	unsigned int count = good.count;
	struct monaco_cold_boot saved = boot;

	sail_check(run(&good, &boot) == MONACO_COLD_STATE && good.count == count);
	sail_check(memcmp(&saved, &boot, sizeof(boot)) == 0);

	/* Fail both sides of every inter-component boundary and every mutating
	 * operation. Individual operations are covered by the component tests.
	 */
	unsigned int faults = 0;

	for (unsigned int n = 0; n < count; n++) {
		bool boundary = n == 0 || good.trace[n - 1].stage != good.trace[n].stage ||
				n + 1 == count || good.trace[n + 1].stage != good.trace[n].stage;
		if (!boundary && !good.trace[n].write) {
			continue;
		}
		for (unsigned int after = 0; after < (good.trace[n].write ? 2U : 1U); after++) {
			prepare(&m, &boot, true);
			m.fail_at = n + 1;
			m.fault_after_effect = after != 0;
			int rc = run(&m, &boot);

			sail_check(rc != 0 && rc != MONACO_COLD_PCINIT_REQUIRED);
			sail_check(boot.stage == good.trace[n].stage);
			sail_check(boot.status == rc && m.count >= m.fail_at);
			for (unsigned int k = m.fail_at; k < m.count; k++) {
				sail_check(m.trace[k].stage == boot.stage);
			}
			sail_check(run(&m, &boot) == MONACO_COLD_STATE);
			faults++;
		}
	}
	printf("cold boot: real chain through PCInit boundary, %u callbacks, %u injected faults\n",
	       count, faults);
}

static void active_success_and_faults(void)
{
	static struct model good, m;
	struct monaco_cold_boot boot;

	prepare(&good, &boot, false);
	good.with_publication = true;
	sail_check(run(&good, &boot) == MONACO_COLD_PCINIT_REQUIRED);
	sail_check(!boot.md_bist_skipped && boot.md_bist_completed && !boot.md_bist_result);
	sail_check(boot.md_bist_diagnostic.stage == MD_STAGE_COMPLETE);
	sail_check(md_publication_is_complete(&boot.md_publication));
	sail_check(good.publication_calls == 20 && good.publication_effects == 20);
	sail_check(boot.pmic_boot.stage == MONACO_PMIC_BOOT_COMPLETE && good.probes == 3);
	sail_check(boot.post_clocks.completed_domains == 4);
	const uint32_t words[] = {0xba010000, 0xba010004, 0xba010002, 0xba010005, 0xba010003};

	sail_check(memcmp(words, good.publication_words, sizeof(words)) == 0);
	unsigned int faults = 0;
	/* Every active callback, including all timer samples, fails once. Every
	 * write also fails after effect: no later component may execute.
	 */
	for (unsigned int i = 0; i < good.count; i++) {
		if (good.trace[i].stage != MONACO_COLD_MD_BIST) {
			continue;
		}
		for (unsigned int after = 0; after < (good.trace[i].write ? 2U : 1U); after++) {
			prepare(&m, &boot, false);
			m.with_publication = true;
			m.fail_at = i + 1;
			m.fault_after_effect = after != 0;
			int rc = run(&m, &boot);

			sail_check(rc != 0 && rc != MONACO_COLD_PCINIT_REQUIRED);
			sail_check(boot.stage == MONACO_COLD_MD_BIST && !boot.md_bist_completed);
			sail_check(boot.md_bist_result == rc &&
				   boot.md_bist_diagnostic.result == rc);
			sail_check(!boot.md_bist_skipped &&
			       boot.hbcu.mbist.outcome == MONACO_HBCU_UNOBSERVED && !m.probes);
			sail_check(!m.transactions && m.count == m.fail_at);
			sail_check(!md_publication_is_complete(&boot.md_publication));
			sail_check(run(&m, &boot) == MONACO_COLD_STATE && m.count == m.fail_at);
			faults++;
		}
	}
	for (int contract = -1; contract <= 2; contract++) {
		if (contract == MD_BIST_ACCESS_PBL_COLD_INHERITED_DEVICE) {
			continue;
		}
		prepare(&m, &boot, false);
		m.with_publication = true;
		m.access_contract = (enum md_bist_access_contract)contract;
		sail_check(run(&m, &boot) == MD_BIST_PREREQUISITE);
		sail_check(!m.publication_calls && !boot.md_publication.started && !m.probes);
	}
	printf("cold boot: active real MD to PMIC/PostClock boundary, %u callbacks, %u MD faults\n",
	       good.count, faults);
}

static void refusals(void)
{
	struct model m;
	struct monaco_cold_boot boot;

	prepare(&m, &boot, false);
	sail_check(run(&m, &boot) == MONACO_COLD_MD_PUBLICATION_REQUIRED);
	sail_check(boot.stage == MONACO_COLD_MD_BIST && !boot.md_bist_skipped);
	sail_check(!m.probes && !m.transactions && !boot.i2c.initialized);
	for (unsigned int bit = 0; bit < 32; bit++) {
		prepare(&m, &boot, true);
		m.cpsr ^= UINT32_C(1) << bit;
		if ((UINT32_C(1) << bit) & UINT32_C(0x0700feff)) {
			sail_check(run(&m, &boot) == MONACO_COLD_ENTRY);
			sail_check(m.count == 1 && m.writes == 0);
		} else {
			sail_check(run(&m, &boot) == MONACO_COLD_PCINIT_REQUIRED);
		}
	}
	for (unsigned int bit = 0; bit < 24; bit++) {
		prepare(&m, &boot, true);
		m.mpidr ^= UINT32_C(1) << bit;
		sail_check(run(&m, &boot) == MONACO_COLD_ENTRY && m.writes == 0);
	}
	const uint32_t identity[] = {0xf0146014, 0xf0300028, 0xf1149024};

	for (size_t i = 0; i < ARRAY_SIZE(identity); i++) {
		prepare(&m, &boot, true);
		m.identity_drift = identity[i];
		sail_check(run(&m, &boot) == MONACO_COLD_POLICY_CHANGED);
		sail_check(boot.stage == MONACO_COLD_PMIC_SAMPLE && m.probes == 3 &&
			   !m.transactions);
	}
	prepare(&m, &boot, true);
	m.pmic[1] = 0x81;
	sail_check(run(&m, &boot) == MONACO_PMIC_UNSUPPORTED);
	sail_check(boot.stage == MONACO_COLD_PMIC_IDENTIFY && !m.crc_enabled);
	sail_check(m.transactions == 4 && m.pmic_writes[0x85] == 0);
	prepare(&m, &boot, true);
	m.pmic[0x66] = m.pmic[0x81] = 0;
	sail_check(run(&m, &boot) == MONACO_PMIC_BOOT_BIST_FAILED);
	sail_check(boot.stage == MONACO_COLD_PMIC_PRETARGET);
	sail_check(boot.pmic_boot.runtime_bist_required && boot.pmic_boot.runtime_bist_attempted);
	sail_check(!boot.pmic_boot.regulators_completed &&
		   !boot.pmic_boot.posttarget_writes_completed);
	sail_check(boot.post_clocks.completed_domains == 0);
	prepare(&m, &boot, true);
	m.frequency = 1000000;
	sail_check(run(&m, &boot) == MONACO_TIMER_INVALID && !m.writes);
	prepare(&m, &boot, true);
	m.stopped_timer = true;
	sail_check(run(&m, &boot) != MONACO_COLD_PCINIT_REQUIRED);
	sail_check(boot.stage <= MONACO_COLD_EARLY_CLOCKS);
	sail_check(monaco_cold_boot_run(NULL, NULL, NULL) == MONACO_COLD_INVALID);
	prepare(&m, &boot, true);
	sail_check(monaco_cold_boot_run(&boot, NULL, NULL) == MONACO_COLD_INVALID);
	sail_check(run(&m, &boot) == MONACO_COLD_STATE && !m.count);
}

static void runtime_bist_cases(void)
{
	static struct model model;
	struct monaco_cold_boot boot;

	prepare(&model, &boot, true);
	model.pmic[0x66] = 0U;
	model.pmic[0x81] = 0U;
	model.runtime_success = true;
	sail_check(run(&model, &boot) == MONACO_COLD_PCINIT_REQUIRED);
	sail_check(model.runtime_triggers == 1U);
	sail_check(boot.pmic_boot.runtime_bist_required && boot.pmic_boot.runtime_bist_attempted);
	sail_check(!boot.pmic_boot.boot_bist_accepted && boot.pmic_boot.runtime_bist_flags_valid);
	sail_check(boot.pmic_boot.runtime_bist_passed &&
		   boot.pmic_boot.runtime_bist_cleanup_complete);
	sail_check(model.pmic_reads[0x66] >= 2U && model.pmic_reads[0x81] >= 2U);
	sail_check(boot.pmic_boot.regulators_completed == 9U);
	sail_check(boot.pmic_boot.posttarget_writes_completed == 6U);
	sail_check(boot.pmic_boot.settle_elapsed_valid &&
		   boot.pmic_boot.settle_elapsed_us >= 3000U);
	uint32_t count = model.count;

	sail_check(run(&model, &boot) == MONACO_COLD_STATE && model.count == count);
	for (uint32_t failure = 0U; failure < 5U; failure++) {
		prepare(&model, &boot, true);
		model.pmic[0x66] = 0U;
		model.pmic[0x81] = 0U;
		model.runtime_success = failure != 0U;
		model.stop_on_trigger = failure == 1U;
		model.reverse_on_trigger = failure == 2U;
		if (failure >= 3U) {
			model.runtime_fail_reg = failure == 3U ? 0x66U : 0x4eU;
			model.runtime_fail_read = failure == 3U;
		}
		int result = run(&model, &boot);

		sail_check(result < 0 && result != MONACO_COLD_PCINIT_REQUIRED);
		sail_check(boot.status == result && boot.stage == MONACO_COLD_PMIC_PRETARGET);
		sail_check(model.runtime_triggers == 1U);
		sail_check(!boot.pmic_boot.runtime_bist_cleanup_complete);
		sail_check(boot.pmic_boot.regulators_completed == 0U);
		sail_check(boot.pmic_boot.posttarget_writes_completed == 0U);
		sail_check(boot.post_clocks.completed_domains == 0U);
		if (failure == 0U) {
			sail_check(result == MONACO_PMIC_BOOT_BIST_FAILED);
		}
		if (failure >= 3U) {
			sail_check(model.runtime_faults == 1U);
		}
		count = model.count;
		sail_check(run(&model, &boot) == MONACO_COLD_STATE && model.count == count);
	}
	puts("cold boot: conditional runtime BIST, fresh flags, cleanup and failure stops passed");
}

int sail_model_test(void)
{
	FILE *file = fopen(SAIL_TEST_FIXTURE_DIR "/early-clock.trace", "r");

	sail_check(file);
	while (clock_events < ARRAY_SIZE(clock_trace)) {
		struct clock_event *e = &clock_trace[clock_events];

		if (fscanf(file, " %c %x %x", &e->kind, &e->address, &e->value) != 3) {
			break;
		}
		clock_events++;
	}
	sail_check(fclose(file) == 0 && clock_events == 81);
	success_and_faults();
	refusals();
	runtime_bist_cases();
	active_success_and_faults();
	puts("cold boot: entry, real skip, missing-publication stop, identity drift and "
	     "one-attempt checks passed");
	return 0;
}
