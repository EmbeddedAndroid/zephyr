/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "board_policy.h"
#include "sail_check.h"
#include <limits.h>
#include <stdio.h>

/* Allowed accesses: identity, repeat identity, then the clock aperture only.
 * Any GENI, GPIO, RAM or write access aborts.
 */
static const uint32_t addresses[] = {
	0xf0146014, 0xf0300028, 0xf1149024, 0xf0146014, 0xf0300028, 0xf1149024,
	0xf1115020, 0xf1115024, 0xf1111074, 0xf1111078, 0xf1115018, 0xf111500c,
	0xf1115004, 0xf1115008, 0xf1115054, 0xf1115040, 0xf1115038,
};

struct fixture {
	uint32_t values[17];
	unsigned int calls, fail_at;
	int failure;
	bool return_failed_value;
};

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fixture *f = context;

	sail_check(f->calls < 17);
	sail_check(address == addresses[f->calls]);
	unsigned int index = f->calls++;

	if (f->calls == f->fail_at) {
		if (f->return_failed_value) {
			*value = UINT32_MAX;
		}
		return f->failure;
	}
	*value = f->values[index];
	return 0;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	(void)context;
	(void)address;
	(void)value;
	sail_check(!"Board policy must never write hardware state");
	return -1;
}

static struct monaco_timer_io io_for(struct fixture *f)
{
	return (struct monaco_timer_io){f, read32, write32};
}

static void initialize(struct fixture *f)
{
	*f = (struct fixture){.values = {
				      0xabcd02e7,
				      0x601a0305,
				      0x12345673,
				      0xabcd02e7,
				      0x601a0305,
				      0x12345673,
				      0,
				      0x105,
				      0,
				      0x105,
				      1,
				      1,
				      1,
				      1,
				      0,
				      0,
				      0x80000000,
			      }};
}

static void dirty_outputs(struct monaco_i2c_boot_qualification *q, struct monaco_hbcu_policy *h)
{
	*q = (struct monaco_i2c_boot_qualification){
		MONACO_I2C_PROFILE_VENTUNO_Q, true, true, true, true,
	};
	*h = (struct monaco_hbcu_policy){true, 3};
}

static void cleared(const struct monaco_i2c_boot_qualification *q,
		    const struct monaco_hbcu_policy *h)
{
	sail_check(q->board == MONACO_I2C_PROFILE_BOARD_UNKNOWN);
	sail_check(!q->exclusive_ownership && !q->parent_clocks_qualified);
	sail_check(!q->reset_state_qualified && !q->board_state_qualified);
	sail_check(!h->valid && h->injected_failures == 0);
}

static void complete(const struct monaco_board_policy *p,
		     const struct monaco_i2c_boot_qualification *q,
		     const struct monaco_hbcu_policy *h)
{
	sail_check(p->status == 0 && p->callback_status == 0);
	sail_check(p->stage == MONACO_BOARD_COMPLETE && p->value_valid);
	sail_check(p->reads_attempted == 17 && p->reads_completed == 17);
	sail_check(p->last_address == 0xf1115038);
	sail_check(q->board == MONACO_I2C_PROFILE_VENTUNO_Q);
	sail_check(q->exclusive_ownership && q->parent_clocks_qualified);
	sail_check(q->reset_state_qualified && q->board_state_qualified);
	sail_check(h->valid && h->injected_failures == 0);
}

static void golden_and_lifetime(void)
{
	struct fixture f;
	struct monaco_board_policy p;
	struct monaco_i2c_boot_qualification q;
	struct monaco_hbcu_policy h;

	initialize(&f);
	struct monaco_timer_io io = io_for(&f);

	sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
	sail_check(f.calls == 3 && p.stage == MONACO_BOARD_PREPARED);
	sail_check(p.descriptor && p.descriptor->board == MONACO_BOARD_VENTUNO_Q);
	sail_check(p.descriptor->entry == MONACO_BOARD_PBL_NORMAL_COLD);
	sail_check(p.descriptor->inherited_xo_hz == 19200000);
	sail_check(p.descriptor->assumptions ==
	       (MONACO_BOARD_INHERITED_XO | MONACO_BOARD_EXCLUSIVE_EARLY_OWNER |
		MONACO_BOARD_NO_CONCURRENT_RESET));
	sail_check(p.descriptor->hbcu_injected_failures == 0);
	sail_check(p.identity.raw_sku == f.values[0]);
	sail_check(p.identity.raw_soc_identity == f.values[1]);
	sail_check(p.identity.raw_reset_fsm == f.values[2]);
	sail_check(p.identity.sku == 0x2e7 && p.identity.chip_variant == 0x2a3);
	sail_check(p.identity.chip_version == 0x30005 && p.identity.reset_fsm == 3);
	sail_check(p.current_identity.raw_soc_identity == 0 && p.clocks.core_config == 0);
	dirty_outputs(&q, &h);
	sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == 0);
	complete(&p, &q, &h);
	sail_check(f.calls == 17 && p.current_identity.raw_soc_identity == f.values[4]);
	sail_check(p.clocks.core_command == 0 && p.clocks.core_config == 0x105);
	sail_check(p.clocks.ahb_command == 0 && p.clocks.ahb_config == 0x105);
	sail_check(p.clocks.core2x_branch == 1 && p.clocks.core_branch == 1);
	sail_check(p.clocks.master_ahb_branch == 1 && p.clocks.slave_ahb_branch == 1);
	sail_check(p.clocks.se_dfs == 0 && p.clocks.se_command == 0);
	sail_check(p.clocks.se_branch == 0x80000000 && p.last_value == 0x80000000);
	sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == MONACO_BOARD_STALE);
	sail_check(f.calls == 17);
	cleared(&q, &h);
	sail_check(p.clocks.core_config == 0 && p.current_identity.raw_soc_identity == 0);
	initialize(&f);
	/* A fresh sample, not a retained success flag, allows another attempt. */
	sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
	sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == 0);
	complete(&p, &q, &h);
	initialize(&f);
	io.write32 = NULL;
	sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
	sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == 0);
}

static uint16_t expected_variant(uint32_t sku)
{
	static const uint16_t accepted[][2] = {
		{0x026c, 0x025e}, {0x026d, 0x025d}, {0x026e, 0x025f}, {0x028f, 0x026c},
		{0x02e6, 0x02a2}, {0x02e7, 0x02a3}, {0x02fb, 0x02b7},
	};
	for (unsigned int i = 0; i < 7; i++) {
		if (sku == accepted[i][0]) {
			return accepted[i][1];
		}
	}
	return 0;
}

static void identities(void)
{
	struct fixture f;
	struct monaco_board_policy p;
	struct monaco_timer_io io = io_for(&f);

	for (uint32_t sku = 0; sku <= UINT16_MAX; sku++) {
		initialize(&f);
		f.values[0] = 0xffff0000U | sku;
		uint16_t variant = expected_variant(sku);
		int rc = monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p);

		sail_check(rc == (variant ? 0 : MONACO_BOARD_IDENTITY));
		sail_check(f.calls == (variant ? 3U : 2U));
		sail_check(p.identity.sku == sku && p.identity.chip_variant == variant);
	}
	for (unsigned int bit = 16; bit < 32; bit++) {
		initialize(&f);
		f.values[1] ^= UINT32_C(1) << bit;
		sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) ==
		       MONACO_BOARD_IDENTITY);
		sail_check(f.calls == 2);
	}
	for (uint32_t revision = 0; revision <= UINT16_MAX; revision++) {
		initialize(&f);
		f.values[1] = 0x601a0000U | revision;
		sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
		sail_check(p.identity.chip_version == ((revision >> 8) << 16) + (revision & 255U));
	}
	for (uint32_t fsm = 0; fsm < 16; fsm++) {
		initialize(&f);
		f.values[2] = 0xfffffff0U | fsm;
		sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) ==
		       (fsm < 12 ? 0 : MONACO_BOARD_NOT_COLD));
		sail_check(f.calls == 3 && p.identity.reset_fsm == fsm);
	}
}

static void faults_and_retirement(void)
{
	static const int errors[] = {-17, 1, INT_MAX, INT_MIN};
	struct fixture f;
	struct monaco_board_policy p;
	struct monaco_i2c_boot_qualification q;
	struct monaco_hbcu_policy h;
	struct monaco_timer_io io = io_for(&f);

	for (unsigned int position = 1; position <= 17; position++) {
		for (unsigned int e = 0; e < 4; e++) {
			for (unsigned int effect = 0; effect < 2; effect++) {
				initialize(&f);
				f.fail_at = position;
				f.failure = errors[e];
				f.return_failed_value = effect != 0;
				int expected = errors[e] > 0 ? MONACO_BOARD_CALLBACK : errors[e];
				int rc = monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p);

				if (position > 3) {
					sail_check(rc == 0);
					dirty_outputs(&q, &h);
					rc = monaco_board_after_clocks(&io, &p, &q, &h);
					cleared(&q, &h);
				}
				sail_check(rc == expected && p.status == expected);
				sail_check(p.callback_status == errors[e] && f.calls == position);
				sail_check(p.reads_attempted == position &&
				       p.reads_completed == position - 1);
				sail_check(!p.value_valid && p.last_value == 0);
				sail_check(p.last_address == addresses[position - 1]);
				dirty_outputs(&q, &h);
				sail_check(monaco_board_after_clocks(&io, &p, &q, &h) ==
				       MONACO_BOARD_STALE);
				sail_check(f.calls == position);
				cleared(&q, &h);
			}
		}
	}
	/* A failed prepare retires a previous complete result and all measurements. */
	initialize(&f);
	sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
	sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == 0);
	initialize(&f);
	f.fail_at = 1;
	f.failure = -17;
	sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == -17);
	sail_check(p.identity.raw_sku == 0 && p.current_identity.raw_sku == 0);
	sail_check(p.clocks.se_branch == 0 && p.reads_completed == 0);
}

static void fresh_identity(void)
{
	struct fixture f;
	struct monaco_board_policy p;
	struct monaco_i2c_boot_qualification q;
	struct monaco_hbcu_policy h;
	struct monaco_timer_io io = io_for(&f);
	const uint32_t changed[] = {0x02e6, 0x601a0306, 4, 0, 0x601b0305, 12};
	const unsigned int index[] = {3, 4, 5, 3, 4, 5};
	const unsigned int calls[] = {6, 6, 6, 5, 5, 6};
	const int expected[] = {MONACO_BOARD_STALE,    MONACO_BOARD_STALE,
				MONACO_BOARD_STALE,    MONACO_BOARD_IDENTITY,
				MONACO_BOARD_IDENTITY, MONACO_BOARD_NOT_COLD};
	for (unsigned int i = 0; i < 6; i++) {
		initialize(&f);
		sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
		f.values[index[i]] = changed[i];
		dirty_outputs(&q, &h);
		sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == expected[i]);
		sail_check(f.calls == calls[i]);
		cleared(&q, &h);
	}
	/* Unknown or reserved SKU and FSM bits are reported but never select a policy. */
	initialize(&f);
	sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
	f.values[3] ^= 0xffff0000U;
	f.values[5] ^= 0xfffffff0U;
	sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == 0);
	sail_check(p.current_identity.raw_sku == f.values[3]);
	sail_check(p.current_identity.raw_reset_fsm == f.values[5]);
	complete(&p, &q, &h);
}

static void clock_masks(void)
{
	/* Masks of the supported control states, written out independently of the driver. */
	static const uint32_t masks[] = {1,          0x371f,     1, 0x371f, 0x80000001, 0x80000001,
					 0x80000001, 0x80000001, 1, 1,      1};
	struct fixture f;
	struct monaco_board_policy p;
	struct monaco_i2c_boot_qualification q;
	struct monaco_hbcu_policy h;
	struct monaco_timer_io io = io_for(&f);

	for (unsigned int index = 0; index < 11; index++) {
		for (unsigned int bit = 0; bit < 33; bit++) {
			initialize(&f);
			sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
			bool bad = bit == 32 || (masks[index] & (UINT32_C(1) << bit));

			if (bit == 32) {
				f.values[index + 6] = UINT32_MAX;
			} else {
				f.values[index + 6] ^= UINT32_C(1) << bit;
			}
			dirty_outputs(&q, &h);
			int rc = monaco_board_after_clocks(&io, &p, &q, &h);

			sail_check(rc ==
			       (bad ? (index < 8 ? MONACO_BOARD_PARENT : MONACO_BOARD_SE_STATE)
				    : 0));
			if (bad) {
				sail_check(f.calls == index + 7);
				sail_check(p.value_valid && p.last_address == addresses[index + 6]);
				sail_check(p.last_value == f.values[index + 6]);
				cleared(&q, &h);
			} else {
				sail_check(f.calls == 17);
				complete(&p, &q, &h);
			}
		}
	}
}

static void invalid_inputs(void)
{
	struct fixture f;
	struct monaco_board_policy p = {0};
	struct monaco_i2c_boot_qualification q;
	struct monaco_hbcu_policy h;

	initialize(&f);
	struct monaco_timer_io io = io_for(&f), missing = {0};

	sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, NULL) == MONACO_BOARD_INVALID);
	sail_check(monaco_board_prepare(NULL, MONACO_BOARD_VENTUNO_Q, &p) == MONACO_BOARD_INVALID);
	sail_check(monaco_board_prepare(&missing, MONACO_BOARD_VENTUNO_Q, &p) ==
		   MONACO_BOARD_INVALID);
	sail_check(monaco_board_prepare(&io, MONACO_BOARD_UNKNOWN, &p) == MONACO_BOARD_UNSUPPORTED);
	sail_check(monaco_board_prepare(&io, (enum monaco_board)99, &p) ==
		   MONACO_BOARD_UNSUPPORTED);
	sail_check(p.descriptor == NULL && f.calls == 0);
	dirty_outputs(&q, &h);
	sail_check(monaco_board_after_clocks(&io, NULL, &q, &h) == MONACO_BOARD_INVALID);
	cleared(&q, &h);
	sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == MONACO_BOARD_STALE);
	sail_check(f.calls == 0);
	for (unsigned int i = 0; i < 4; i++) {
		initialize(&f);
		sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
		dirty_outputs(&q, &h);
		sail_check(monaco_board_after_clocks(i == 0 ? NULL : (i == 1 ? &missing : &io), &p,
						 i == 2 ? NULL : &q,
						 i == 3 ? NULL : &h) == MONACO_BOARD_INVALID);
		sail_check(f.calls == 3);
		if (i == 2) {
			sail_check(!h.valid);
		} else if (i == 3) {
			sail_check(!q.exclusive_ownership);
		} else {
			cleared(&q, &h);
		}
		sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == MONACO_BOARD_STALE);
		sail_check(f.calls == 3);
	}
	/* Corrupted software records cannot skip fresh sampling or reuse a result. */
	for (unsigned int mutation = 0; mutation < 14; mutation++) {
		initialize(&f);
		sail_check(monaco_board_prepare(&io, MONACO_BOARD_VENTUNO_Q, &p) == 0);
		switch (mutation) {
		case 0:
			p.descriptor = NULL;
			break;
		case 1:
			p.stage = MONACO_BOARD_COMPLETE;
			break;
		case 2:
			p.status = -17;
			break;
		case 3:
			p.callback_status = 1;
			break;
		case 4:
			p.value_valid = false;
			break;
		case 5:
			p.reads_attempted = 17;
			break;
		case 6:
			p.reads_completed = 2;
			break;
		case 7:
			p.last_address = 0xf1115038;
			break;
		case 8:
			p.last_value ^= 1;
			break;
		case 9:
			p.identity.sku ^= 1;
			break;
		case 10:
			p.identity.chip_variant ^= 1;
			break;
		case 11:
			p.identity.chip_version ^= 1;
			break;
		case 12:
			p.identity.reset_fsm ^= 1;
			break;
		case 13:
			p.identity.raw_soc_identity ^= 0x10000;
			break;
		default:
			sail_check(false);
		}
		dirty_outputs(&q, &h);
		sail_check(monaco_board_after_clocks(&io, &p, &q, &h) == MONACO_BOARD_STALE);
		sail_check(f.calls == 3);
		cleared(&q, &h);
	}
}

int sail_model_test(void)
{
	golden_and_lifetime();
	identities();
	faults_and_retirement();
	fresh_identity();
	clock_masks();
	invalid_inputs();
	puts("board_policy: exact 17-read trace, all SKUs/revisions/FSMs, masks, 136 callback "
	     "faults and retirement pass");
	return 0;
}
