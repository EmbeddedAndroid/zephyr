/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "board_policy.h"

#define SKU UINT32_C(0xf0146014)
#define SOC UINT32_C(0xf0300028)
#define FSM UINT32_C(0xf1149024)

static const struct monaco_board_descriptor ventuno_q = {
	.board = MONACO_BOARD_VENTUNO_Q,
	.entry = MONACO_BOARD_PBL_NORMAL_COLD,
	.name = "VENTUNO Q",
	.inherited_xo_hz = 19200000,
	.assumptions = MONACO_BOARD_INHERITED_XO | MONACO_BOARD_EXCLUSIVE_EARLY_OWNER |
		       MONACO_BOARD_NO_CONCURRENT_RESET,
	.hbcu_injected_failures = 0,
};

static uint16_t chip_variant(uint16_t sku)
{
	switch (sku) {
	case 0x026e:
		return 0x025f; /* SA_MONACOAU_SRV1L */
	case 0x026c:
		return 0x025e; /* SA_MONACOAU_IVI */
	case 0x026d:
		return 0x025d; /* SA_MONACOAU_ADAS */
	case 0x028f:
		return 0x026c; /* SA_MONACOAU_SRV1L_FFC */
	case 0x02e6:
		return 0x02a2; /* QCS_MONACO_INDUSTRIAL_SAFE */
	case 0x02e7:
		return 0x02a3; /* QCS_MONACO_INDUSTRIAL */
	case 0x02fb:
		return 0x02b7; /* SA_MONACOAU_FLEX */
	default:
		return 0;
	}
}

static int decode_identity(struct monaco_board_identity *id)
{
	id->sku = (uint16_t)(id->raw_sku & UINT32_C(0xffff));
	id->chip_variant = chip_variant(id->sku);
	id->chip_version = ((id->raw_soc_identity & UINT32_C(0xff00)) << 8) |
			   (id->raw_soc_identity & UINT32_C(0xff));
	id->reset_fsm = (uint8_t)(id->raw_reset_fsm & UINT32_C(0xf));
	if (!id->chip_variant ||
	    (id->raw_soc_identity & UINT32_C(0xffff0000)) != UINT32_C(0x601a0000)) {
		return MONACO_BOARD_IDENTITY;
	}
	return id->reset_fsm >= 12 ? MONACO_BOARD_NOT_COLD : 0;
}

static int read32(const struct monaco_timer_io *io, struct monaco_board_policy *p,
		  enum monaco_board_stage stage, uint32_t address, uint32_t *out)
{
	uint32_t sampled = 0;

	p->stage = stage;
	p->last_address = address;
	p->last_value = 0;
	p->value_valid = false;
	p->reads_attempted++;
	p->callback_status = io->read32(io->context, address, &sampled);
	p->status = p->callback_status > 0 ? MONACO_BOARD_CALLBACK : p->callback_status;
	if (!p->status) {
		p->reads_completed++;
		p->last_value = sampled;
		p->value_valid = true;
		*out = sampled;
	}
	return p->status;
}

static int sample_identity(const struct monaco_timer_io *io, struct monaco_board_policy *p,
			   struct monaco_board_identity *id)
{
	if (read32(io, p, MONACO_BOARD_SKU, SKU, &id->raw_sku) ||
	    read32(io, p, MONACO_BOARD_SOC, SOC, &id->raw_soc_identity)) {
		return p->status;
	}
	p->stage = MONACO_BOARD_CHECK_IDENTITY;
	/* Refuse unsupported silicon before reading the reset-state aperture. */
	p->status = decode_identity(id);
	if (p->status) {
		return p->status;
	}
	if (read32(io, p, MONACO_BOARD_FSM, FSM, &id->raw_reset_fsm)) {
		return p->status;
	}
	p->status = decode_identity(id);
	return p->status;
}

int monaco_board_prepare(const struct monaco_timer_io *io, enum monaco_board board,
			 struct monaco_board_policy *p)
{
	if (!p) {
		return MONACO_BOARD_INVALID;
	}
	*p = (struct monaco_board_policy){.stage = MONACO_BOARD_INPUT,
					  .status = MONACO_BOARD_INVALID};
	if (!io || !io->read32) {
		return p->status;
	}
	if (board != MONACO_BOARD_VENTUNO_Q) {
		p->status = MONACO_BOARD_UNSUPPORTED;
		return p->status;
	}
	p->descriptor = &ventuno_q;
	if (sample_identity(io, p, &p->identity)) {
		return p->status;
	}
	p->stage = MONACO_BOARD_PREPARED;
	return 0;
}

static bool prepared(const struct monaco_board_policy *p)
{
	struct monaco_board_identity decoded = p->identity;

	return p->descriptor == &ventuno_q && p->stage == MONACO_BOARD_PREPARED && !p->status &&
	       !p->callback_status && p->value_valid && p->reads_attempted == 3 &&
	       p->reads_completed == 3 && p->last_address == FSM &&
	       p->last_value == p->identity.raw_reset_fsm && !decode_identity(&decoded) &&
	       decoded.sku == p->identity.sku && decoded.chip_variant == p->identity.chip_variant &&
	       decoded.chip_version == p->identity.chip_version &&
	       decoded.reset_fsm == p->identity.reset_fsm;
}

static int check(const struct monaco_timer_io *io, struct monaco_board_policy *p,
		 enum monaco_board_stage stage, uint32_t address, uint32_t mask, uint32_t expected,
		 uint32_t *out)
{
	if (read32(io, p, stage, address, out)) {
		return p->status;
	}
	if (*out == UINT32_MAX || (*out & mask) != expected) {
		p->status = stage == MONACO_BOARD_SE_CONTROL ? MONACO_BOARD_SE_STATE
							     : MONACO_BOARD_PARENT;
	}
	return p->status;
}

static int sample_clocks(const struct monaco_timer_io *io, struct monaco_board_policy *p)
{
	struct monaco_board_clocks *c = &p->clocks;
	/* QUP core and AHB roots must select PLL0 (source 1) with divider encoding 6. */
	if (check(io, p, MONACO_BOARD_CORE_ROOT, 0xf1115020, 1, 0, &c->core_command) ||
	    check(io, p, MONACO_BOARD_CORE_ROOT, 0xf1115024, 0x371f, 0x105, &c->core_config) ||
	    check(io, p, MONACO_BOARD_AHB_ROOT, 0xf1111074, 1, 0, &c->ahb_command) ||
	    check(io, p, MONACO_BOARD_AHB_ROOT, 0xf1111078, 0x371f, 0x105, &c->ahb_config) ||
	    check(io, p, MONACO_BOARD_QUP_BRANCHES, 0xf1115018, 0x80000001, 1, &c->core2x_branch) ||
	    check(io, p, MONACO_BOARD_QUP_BRANCHES, 0xf111500c, 0x80000001, 1, &c->core_branch) ||
	    check(io, p, MONACO_BOARD_QUP_BRANCHES, 0xf1115004, 0x80000001, 1,
		  &c->master_ahb_branch) ||
	    check(io, p, MONACO_BOARD_QUP_BRANCHES, 0xf1115008, 0x80000001, 1,
		  &c->slave_ahb_branch) ||
	    check(io, p, MONACO_BOARD_SE_CONTROL, 0xf1115054, 1, 0, &c->se_dfs) ||
	    check(io, p, MONACO_BOARD_SE_CONTROL, 0xf1115040, 1, 0, &c->se_command) ||
	    check(io, p, MONACO_BOARD_SE_CONTROL, 0xf1115038, 1, 0, &c->se_branch)) {
		return p->status;
	}
	return 0;
}

int monaco_board_after_clocks(const struct monaco_timer_io *io, struct monaco_board_policy *p,
			      struct monaco_i2c_boot_qualification *i2c,
			      struct monaco_hbcu_policy *hbcu)
{
	if (i2c) {
		*i2c = (struct monaco_i2c_boot_qualification){0};
	}
	if (hbcu) {
		*hbcu = (struct monaco_hbcu_policy){0};
	}
	if (!p) {
		return MONACO_BOARD_INVALID;
	}
	bool was_prepared = prepared(p);

	p->stage = MONACO_BOARD_AFTER_CLOCKS;
	p->current_identity = (struct monaco_board_identity){0};
	p->clocks = (struct monaco_board_clocks){0};
	p->value_valid = false;
	p->last_value = 0;
	p->status = MONACO_BOARD_INVALID;
	if (!io || !io->read32 || !i2c || !hbcu) {
		return p->status;
	}
	if (!was_prepared) {
		p->status = MONACO_BOARD_STALE;
		return p->status;
	}
	if (sample_identity(io, p, &p->current_identity)) {
		return p->status;
	}
	if (p->identity.sku != p->current_identity.sku ||
	    p->identity.raw_soc_identity != p->current_identity.raw_soc_identity ||
	    p->identity.reset_fsm != p->current_identity.reset_fsm) {
		p->status = MONACO_BOARD_STALE;
		return p->status;
	}
	if (sample_clocks(io, p)) {
		return p->status;
	}
	*i2c = (struct monaco_i2c_boot_qualification){
		.board = MONACO_I2C_PROFILE_VENTUNO_Q,
		/* Implied by the cold-boot entry state and the clock checks above. */
		.exclusive_ownership = true,
		.parent_clocks_qualified = true,
		.reset_state_qualified = true,
		.board_state_qualified = true,
	};
	*hbcu = (struct monaco_hbcu_policy){.valid = true,
					    .injected_failures = ventuno_q.hbcu_injected_failures};
	p->stage = MONACO_BOARD_COMPLETE;
	return 0;
}
