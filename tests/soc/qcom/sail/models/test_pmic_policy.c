/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic_policy.h"
#include "sail_check.h"
#include <limits.h>
#include <stdio.h>
#include <zephyr/sys/util.h>


#define TIMEOUT_US 700U

struct fixture {
	uint32_t sku, soc, fsm;
	unsigned int calls, fail_at, identity_reads, regulator_reads;
	int failure;
	uint8_t version[4];
	bool smps1;
};

static const uint32_t trace_address[] = {
	0xf0146014, 0xf0300028, 0xf1149024, 0x28, 0x29, 0x12,
};

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fixture *f = context;

	sail_check(f->calls < 3 && address == trace_address[f->calls]);
	*value = f->calls == 0 ? f->sku : (f->calls == 1 ? f->soc : f->fsm);
	f->calls++;
	return f->calls == f->fail_at ? f->failure : 0;
}

static int probe(void *context, uint8_t address, uint32_t timeout)
{
	struct fixture *f = context;

	sail_check(f->calls >= 3 && f->calls < 6);
	sail_check(address == trace_address[f->calls] && timeout == TIMEOUT_US);
	f->calls++;
	return f->calls == f->fail_at ? f->failure : 0;
}

static int transfer(void *context, uint8_t address, const uint8_t *tx, size_t tx_size, uint8_t *rx,
		    size_t rx_size, uint32_t timeout)
{
	static const uint8_t version_register[] = {1, 2, 3, 0xa7};
	struct fixture *f = context;

	sail_check(address == 0x28 && timeout == TIMEOUT_US && tx_size == 1);
	if (f->smps1) {
		/* Fixed read CRC vectors. No write is allowed. */
		static const uint8_t reg[] = {0x06, 0x10, 0x06, 0x06};
		static const uint8_t value[] = {0, 15, 0, 0};
		static const uint8_t crc[] = {0x49, 0xbb, 0x49, 0x49};

		sail_check(rx_size == 2 && f->regulator_reads < 4);
		sail_check(tx[0] == reg[f->regulator_reads]);
		rx[0] = value[f->regulator_reads];
		rx[1] = crc[f->regulator_reads++];
		return 0;
	}
	sail_check(rx_size == 1 && f->identity_reads < 4);
	sail_check(tx[0] == version_register[f->identity_reads]);
	rx[0] = f->version[f->identity_reads++];
	return 0;
}

static int delay_us(void *context, uint32_t us)
{
	(void)context;
	(void)us;
	sail_check(!"Policy selection and permission-denied SMPS1 must not delay");
	return -9000;
}

static void initialize(struct fixture *f)
{
	*f = (struct fixture){
		.sku = 0x2e7, .soc = 0x601a0100, .fsm = 3, .version = {0x82, 4, 1, 0}};
}

static int sample(struct fixture *f, struct monaco_pmic_policy_snapshot *s)
{
	const struct monaco_pmic_policy_io io = {f, read32, probe};

	f->calls = 0;
	return monaco_pmic_policy_sample(MONACO_PMIC_BOARD_VENTUNO_Q, &io, TIMEOUT_US, s);
}

static void identify(struct fixture *f, struct monaco_pmic *pmic)
{
	const struct monaco_pmic_io io = {f, transfer, delay_us};

	f->identity_reads = 0;
	sail_check(monaco_pmic_init(pmic, &io, TIMEOUT_US) == 0);
	sail_check(monaco_pmic_identify(pmic) == 0);
	sail_check(f->identity_reads == 4 && pmic->state == MONACO_PMIC_IDENTIFIED);
}

static void clear_selection(const struct monaco_pmic_policy_selection *p)
{
	sail_check(p->boot.regulators == NULL && p->boot.regulator_count == 0);
	sail_check(!p->boot.supply_policy_validated && !p->boot.reset_fsm_valid);
	sail_check(!p->boot.platform_type_valid && p->boot.present_slaves == 0);
	sail_check(p->boot.topology == MONACO_PMIC_TOPOLOGY_UNKNOWN);
	sail_check(p->sku == 0 && p->chip_variant == 0 && p->chip_version == 0);
	sail_check(p->pmic_version.device == 0 && p->pmic_version.revision == 0);
	sail_check(p->pmic_version.firmware == 0 && p->pmic_version.firmware_revision == 0);
}

static void policy_records(const struct monaco_pmic_policy_selection *p)
{
	/* Expected values for the nine rail electrical policies. */
	static const bool permitted[] = {true, true, false, true, false, false, true, true, true};
	static const bool enabled[] = {true, false, false, true, false, true, true, true, true};

	sail_check(p->boot.regulator_count == 9 && p->boot.regulators);
	for (unsigned int i = 0; i < 9; i++) {
		const struct monaco_pmic_regulator_policy *r = &p->boot.regulators[i];

		sail_check(r->group == (i < 4 ? MONACO_PMIC_LDO : MONACO_PMIC_SMPS));
		sail_check(r->index == (i < 4 ? i : i - 4));
		sail_check(r->write_permitted == permitted[i] && r->enable_required == enabled[i]);
		sail_check(r->required_mode == (i < 4 ? 2 : 0));
		sail_check(r->minimum_uv == (i == 3 ? 1200000U : (i < 4 ? 600000U : 300000U)));
		sail_check(r->maximum_uv == (i < 4 ? 3300000U : 3340000U));
	}
}

static void happy_and_supply_guard(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_policy_snapshot s;
	struct monaco_pmic_policy_selection p;
	struct monaco_pmic_boot_result result;

	initialize(&f);
	sail_check(sample(&f, &s) == 0 && s.sampled && f.calls == 6);
	sail_check(s.stage == MONACO_PMIC_POLICY_SAMPLED && s.status == 0);
	sail_check(s.reads_attempted == 3 && s.reads_completed == 3);
	sail_check(s.probes_attempted == 3 && s.probes_completed == 3);
	sail_check(s.last_address == 0x12 && s.last_callback_status == 0);
	sail_check(s.raw_sku == f.sku && s.raw_soc_identity == f.soc && s.raw_reset_fsm == f.fsm);
	identify(&f, &pmic);
	sail_check(monaco_pmic_policy_select(&s, &pmic, false, &p) == 0);
	sail_check(p.sku == 0x2e7 && p.chip_variant == 0x2a3 && p.chip_version == 0x10000);
	sail_check(p.boot.topology == MONACO_PMIC_TOPOLOGY_INDEPENDENT &&
		   p.boot.present_slaves == 7);
	sail_check(p.boot.reset_fsm_valid && p.boot.reset_fsm == 3);
	sail_check(p.boot.platform_type_valid && p.boot.platform_type == 0);
	sail_check(!p.boot.supply_policy_validated);
	sail_check(p.pmic_version.device == 0x82 && p.pmic_version.revision == 4);
	sail_check(p.pmic_version.firmware == 1 && p.pmic_version.firmware_revision == 0);
	policy_records(&p);
	sail_check(monaco_pmic_boot(&pmic, &p.boot, &result) == MONACO_PMIC_INVALID);
	sail_check(result.operations_attempted == 0 && f.identity_reads == 4 && f.calls == 6);
	sail_check(monaco_pmic_policy_select(&s, &pmic, true, &p) == 0);
	sail_check(p.boot.supply_policy_validated && f.calls == 6 && f.identity_reads == 4);
	policy_records(&p);
	/* SMPS1 requests enable but has no setter permission. */
	pmic.state = MONACO_PMIC_CRC_READY;
	f.smps1 = true;
	struct monaco_pmic_regulator_result rail;

	sail_check(monaco_pmic_regulator_init(&pmic, &p.boot.regulators[5], &rail) == 0);
	sail_check(f.regulator_reads == 4 && rail.enable_skipped && !rail.observed_enabled);
	sail_check(rail.voltage_uv == 600000 && !rail.enable_sequence_complete);
}

static void exact_identities(void)
{
	static const struct {
		uint16_t sku, variant;
	} supported[] = {
		{0x26c, 0x25e}, {0x26d, 0x25d}, {0x26e, 0x25f}, {0x28f, 0x26c},
		{0x2e6, 0x2a2}, {0x2e7, 0x2a3}, {0x2fb, 0x2b7},
	};
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_policy_snapshot s;
	struct monaco_pmic_policy_selection p;

	initialize(&f);
	identify(&f, &pmic);
	for (uint32_t sku = 0; sku <= UINT16_MAX; sku++) {
		uint16_t expected = 0;

		for (unsigned int i = 0; i < ARRAY_SIZE(supported); i++) {
			if (sku == supported[i].sku) {
				expected = supported[i].variant;
			}
		}
		f.sku = sku | UINT32_C(0xabcd0000);
		int rc = sample(&f, &s);

		sail_check(rc == (expected ? 0 : MONACO_PMIC_POLICY_IDENTITY));
		sail_check(f.calls == (expected ? 6U : 2U));
		if (expected) {
			sail_check(monaco_pmic_policy_select(&s, &pmic, false, &p) == 0);
			sail_check(p.sku == sku && p.chip_variant == expected);
			policy_records(&p);
		} else {
			sail_check(!s.sampled && s.probes_attempted == 0);
			sail_check(monaco_pmic_policy_select(&s, &pmic, true, &p) ==
			       MONACO_PMIC_POLICY_INCOMPLETE);
			clear_selection(&p);
		}
	}
	/* Matching a SKU does not override a non-Monaco architecture or part. */
	f.sku = 0x2e7;
	for (uint32_t family = 0; family <= UINT16_MAX; family++) {
		f.soc = (family << 16) | 0x0100;
		int rc = sample(&f, &s);

		sail_check(rc == (family == 0x601a ? 0 : MONACO_PMIC_POLICY_IDENTITY));
		sail_check(f.calls == (family == 0x601a ? 6U : 2U));
	}
	/* Both identity revision bytes are kept; no revision is rejected. */
	for (uint32_t revision = 0; revision <= UINT16_MAX; revision++) {
		f.soc = UINT32_C(0x601a0000) | revision;
		sail_check(sample(&f, &s) == 0);
		sail_check(monaco_pmic_policy_select(&s, &pmic, false, &p) == 0);
		sail_check(p.chip_version == (revision / 256U) * 65536U + revision % 256U);
	}
}

static void reset_and_versions(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_policy_snapshot s;
	struct monaco_pmic_policy_selection p;
	struct monaco_pmic_boot_result result;

	initialize(&f);
	identify(&f, &pmic);
	for (unsigned int n = 0; n < 16; n++) {
		f.fsm = UINT32_C(0xfffffff0) | n;
		sail_check(sample(&f, &s) == 0);
		sail_check(monaco_pmic_policy_select(&s, &pmic, true, &p) ==
		       (n >= 12 ? MONACO_PMIC_BOOT_SKIPPED : 0));
		sail_check(p.boot.reset_fsm == n);
		if (n >= 12) {
			sail_check(monaco_pmic_boot(&pmic, &p.boot, &result) ==
			       MONACO_PMIC_BOOT_SKIPPED);
			sail_check(result.stage == MONACO_PMIC_BOOT_SKIP_COLD &&
			       result.operations_attempted == 0);
			sail_check(f.calls == 6 && f.identity_reads == 4);
		}
	}
	f.fsm = 0;
	sail_check(sample(&f, &s) == 0);
	for (unsigned int field = 0; field < 4; field++) {
		for (unsigned int value = 0; value <= UINT8_MAX; value++) {
			struct monaco_pmic candidate = pmic;
			uint8_t byte = (uint8_t)value;

			switch (field) {
			case 0:
				candidate.version.device = byte;
				break;
			case 1:
				candidate.version.revision = byte;
				break;
			case 2:
				candidate.version.firmware = byte;
				break;
			default:
				candidate.version.firmware_revision = byte;
				break;
			}
			int rc = monaco_pmic_policy_select(&s, &candidate, false, &p);

			sail_check(rc ==
				   (field == 0 && value != 0x82 ? MONACO_PMIC_UNSUPPORTED : 0));
			if (rc) {
				clear_selection(&p);
			} else {
				sail_check(p.pmic_version.device == candidate.version.device);
				sail_check(p.pmic_version.revision == candidate.version.revision);
				sail_check(p.pmic_version.firmware == candidate.version.firmware);
				sail_check(p.pmic_version.firmware_revision ==
				       candidate.version.firmware_revision);
			}
		}
	}
	sail_check(f.calls == 6 && f.identity_reads == 4);
}

static void failures_and_retirement(void)
{
	static const int errors[] = {-1, -1973, INT_MIN, 1, INT_MAX};
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_policy_snapshot s;
	struct monaco_pmic_policy_selection p;

	initialize(&f);
	identify(&f, &pmic);
	for (unsigned int e = 0; e < ARRAY_SIZE(errors); e++) {
		for (unsigned int fail = 1; fail <= 6; fail++) {
			f.fail_at = 0;
			sail_check(sample(&f, &s) == 0);
			sail_check(monaco_pmic_policy_select(&s, &pmic, true, &p) == 0);
			f.fail_at = fail;
			f.failure = errors[e];
			int expected = errors[e] > 0 ? MONACO_PMIC_CALLBACK : errors[e];

			sail_check(sample(&f, &s) == expected);
			sail_check(f.calls == fail && !s.sampled && s.status == expected);
			sail_check(s.last_address == trace_address[fail - 1]);
			sail_check(s.last_callback_status == errors[e]);
			sail_check(s.reads_attempted == (fail > 3 ? 3 : fail));
			sail_check(s.reads_completed == (fail > 3 ? 3 : fail - 1));
			sail_check(s.probes_attempted == (fail > 3 ? fail - 3 : 0));
			sail_check(s.probes_completed == (fail > 3 ? fail - 4 : 0));
			if (fail == 1) {
				sail_check(s.raw_sku == 0);
			}
			if (fail <= 2) {
				sail_check(s.raw_soc_identity == 0);
			}
			if (fail <= 3) {
				sail_check(s.raw_reset_fsm == 0);
			}
			sail_check(s.stage == (fail == 1   ? MONACO_PMIC_POLICY_SKU
					   : fail == 2 ? MONACO_PMIC_POLICY_SOC
					   : fail == 3 ? MONACO_PMIC_POLICY_FSM
						       : MONACO_PMIC_POLICY_PROBE));
			sail_check(monaco_pmic_policy_select(&s, &pmic, true, &p) ==
			       MONACO_PMIC_POLICY_INCOMPLETE);
			clear_selection(&p);
		}
	}
}

static void invalid_inputs(void)
{
	struct fixture f;
	struct monaco_pmic pmic;
	struct monaco_pmic_policy_snapshot s, good;
	struct monaco_pmic_policy_selection p;

	initialize(&f);
	identify(&f, &pmic);
	sail_check(sample(&f, &good) == 0);
	for (unsigned int n = 0; n < 8; n++) {
		struct monaco_pmic_policy_io io = {&f, read32, probe};
		enum monaco_pmic_board_profile profile = MONACO_PMIC_BOARD_VENTUNO_Q;
		uint32_t timeout = TIMEOUT_US;

		s = good;
		f.calls = 0;
		if (n == 2) {
			io.read32 = NULL;
		}
		if (n == 3) {
			io.probe = NULL;
		}
		if (n == 4) {
			timeout = 0;
		}
		if (n == 5) {
			profile = MONACO_PMIC_BOARD_UNKNOWN;
		}
		if (n == 6) {
			profile = (enum monaco_pmic_board_profile)99;
		}
		if (n == 7) {
			profile = (enum monaco_pmic_board_profile)(-1);
		}
		int rc = monaco_pmic_policy_sample(profile, n == 1 ? NULL : &io, timeout,
						   n == 0 ? NULL : &s);
		sail_check(rc == (n >= 5 ? MONACO_PMIC_POLICY_PROFILE : MONACO_PMIC_INVALID));
		sail_check(f.calls == 0);
		if (n) {
			sail_check(!s.sampled && s.reads_attempted == 0 && s.probes_attempted == 0);
		}
	}
	for (unsigned int n = 0; n < 19; n++) {
		struct monaco_pmic candidate = pmic;
		int expected = MONACO_PMIC_POLICY_INCOMPLETE;

		s = good;
		sail_check(monaco_pmic_policy_select(&good, &pmic, true, &p) == 0);
		switch (n) {
		case 0:
			expected = MONACO_PMIC_INVALID;
			break;
		case 1:
			expected = MONACO_PMIC_INVALID;
			break;
		case 2:
			expected = MONACO_PMIC_INVALID;
			break;
		case 3:
			s.profile = MONACO_PMIC_BOARD_UNKNOWN;
			expected = MONACO_PMIC_POLICY_PROFILE;
			break;
		case 4:
			s.sampled = false;
			break;
		case 5:
			s.status = -1;
			break;
		case 6:
			s.stage = MONACO_PMIC_POLICY_PROBE;
			break;
		case 7:
			s.reads_attempted = 2;
			break;
		case 8:
			s.reads_completed = 2;
			break;
		case 9:
			s.probes_attempted = 2;
			break;
		case 10:
			s.probes_completed = 2;
			break;
		case 11:
			s.last_callback_status = -1;
			break;
		case 12:
			s.raw_sku = 0xffff;
			expected = MONACO_PMIC_POLICY_IDENTITY;
			break;
		case 13:
			s.raw_soc_identity = 0x701a0100;
			expected = MONACO_PMIC_POLICY_IDENTITY;
			break;
		case 14:
			candidate.state = MONACO_PMIC_RAW;
			expected = MONACO_PMIC_STATE;
			break;
		case 15:
			candidate.state = MONACO_PMIC_TRANSITION;
			expected = MONACO_PMIC_STATE;
			break;
		case 16:
			candidate.state = MONACO_PMIC_CRC_READY;
			expected = MONACO_PMIC_STATE;
			break;
		case 17:
			candidate.state = MONACO_PMIC_UNINITIALIZED;
			expected = MONACO_PMIC_STATE;
			break;
		default:
			candidate.state = (enum monaco_pmic_state)99;
			expected = MONACO_PMIC_STATE;
			break;
		}
		sail_check(monaco_pmic_policy_select(n == 0 ? NULL : &s, n == 1 ? NULL : &candidate,
						 true, n == 2 ? NULL : &p) == expected);
		if (n != 2) {
			clear_selection(&p);
		}
	}
	sail_check(f.calls == 0 && f.identity_reads == 4);
}

int sail_model_test(void)
{
	happy_and_supply_guard();
	exact_identities();
	reset_and_versions();
	failures_and_retirement();
	invalid_inputs();
	puts("pmic-policy: exact VENTUNO profile, exhaustive identities/revisions, 6-operation "
	     "faults and boot guards passed");
	return 0;
}
