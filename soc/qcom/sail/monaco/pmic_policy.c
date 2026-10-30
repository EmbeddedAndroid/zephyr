/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic_policy.h"
#include <zephyr/sys/util.h>

#define SKU_ADDRESS          UINT32_C(0xf0146014)
#define SOC_IDENTITY_ADDRESS UINT32_C(0xf0300028)
#define RESET_FSM_ADDRESS    UINT32_C(0xf1149024)
#define MONACO_FAMILY        UINT32_C(0x601a0000)

/* VENTUNO Q rails: write permitted, enable required, mode, voltage limits in uV. */
static const struct monaco_pmic_regulator_policy ventuno_q_regulators[] = {
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

_Static_assert(ARRAY_SIZE(ventuno_q_regulators) == MONACO_PMIC_BOOT_REGULATORS,
	       "PMIC profile must cover nine regulators");

static uint16_t variant(uint32_t raw_sku)
{
	switch (raw_sku & UINT32_C(0xffff)) {
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

static bool identity_supported(const struct monaco_pmic_policy_snapshot *s)
{
	return (s->raw_soc_identity & UINT32_C(0xffff0000)) == MONACO_FAMILY &&
	       variant(s->raw_sku) != 0;
}

static int callback_result(int status)
{
	return status > 0 ? MONACO_PMIC_CALLBACK : status;
}

static int sample_read(const struct monaco_pmic_policy_io *io,
		       struct monaco_pmic_policy_snapshot *s, enum monaco_pmic_policy_stage stage,
		       uint32_t address, uint32_t *destination)
{
	uint32_t value = 0;

	s->stage = stage;
	s->last_address = address;
	s->reads_attempted++;
	s->last_callback_status = io->read32(io->context, address, &value);
	s->status = callback_result(s->last_callback_status);
	if (!s->status) {
		*destination = value;
		s->reads_completed++;
	}
	return s->status;
}

int monaco_pmic_policy_sample(enum monaco_pmic_board_profile profile,
			      const struct monaco_pmic_policy_io *io, uint32_t probe_timeout_us,
			      struct monaco_pmic_policy_snapshot *s)
{
	static const uint8_t addresses[] = {0x28, 0x29, 0x12};

	if (!s) {
		return MONACO_PMIC_INVALID;
	}
	*s = (struct monaco_pmic_policy_snapshot){
		.profile = profile,
		.stage = MONACO_PMIC_POLICY_VALIDATE,
	};
	if (!io || !io->read32 || !io->probe || !probe_timeout_us) {
		s->status = MONACO_PMIC_INVALID;
		return s->status;
	}
	if (profile != MONACO_PMIC_BOARD_VENTUNO_Q) {
		s->status = MONACO_PMIC_POLICY_PROFILE;
		return s->status;
	}
	if (sample_read(io, s, MONACO_PMIC_POLICY_SKU, SKU_ADDRESS, &s->raw_sku) ||
	    sample_read(io, s, MONACO_PMIC_POLICY_SOC, SOC_IDENTITY_ADDRESS,
			&s->raw_soc_identity)) {
		return s->status;
	}
	if (!identity_supported(s)) {
		s->status = MONACO_PMIC_POLICY_IDENTITY;
		return s->status;
	}
	if (sample_read(io, s, MONACO_PMIC_POLICY_FSM, RESET_FSM_ADDRESS, &s->raw_reset_fsm)) {
		return s->status;
	}
	s->stage = MONACO_PMIC_POLICY_PROBE;
	for (unsigned int i = 0; i < ARRAY_SIZE(addresses); i++) {
		s->last_address = addresses[i];
		s->probes_attempted++;
		s->last_callback_status = io->probe(io->context, addresses[i], probe_timeout_us);
		s->status = callback_result(s->last_callback_status);
		if (s->status) {
			return s->status;
		}
		s->probes_completed++;
	}
	s->sampled = true;
	s->stage = MONACO_PMIC_POLICY_SAMPLED;
	return 0;
}

int monaco_pmic_policy_select(const struct monaco_pmic_policy_snapshot *s,
			      const struct monaco_pmic *pmic, bool supply_policy_validated,
			      struct monaco_pmic_policy_selection *selection)
{
	if (selection) {
		*selection = (struct monaco_pmic_policy_selection){0};
	}
	if (!s || !pmic || !selection) {
		return MONACO_PMIC_INVALID;
	}
	if (s->profile != MONACO_PMIC_BOARD_VENTUNO_Q) {
		return MONACO_PMIC_POLICY_PROFILE;
	}
	if (!s->sampled || s->status || s->stage != MONACO_PMIC_POLICY_SAMPLED ||
	    s->reads_attempted != 3 || s->reads_completed != 3 || s->probes_attempted != 3 ||
	    s->probes_completed != 3 || s->last_callback_status) {
		return MONACO_PMIC_POLICY_INCOMPLETE;
	}
	if (!identity_supported(s)) {
		return MONACO_PMIC_POLICY_IDENTITY;
	}
	if (pmic->state != MONACO_PMIC_IDENTIFIED) {
		return MONACO_PMIC_STATE;
	}
	if (pmic->version.device != 0x82) {
		return MONACO_PMIC_UNSUPPORTED;
	}
	selection->sku = (uint16_t)(s->raw_sku & UINT32_C(0xffff));
	selection->chip_variant = variant(s->raw_sku);
	selection->chip_version = ((s->raw_soc_identity & UINT32_C(0xff00)) << 8) |
				  (s->raw_soc_identity & UINT32_C(0xff));
	selection->pmic_version = pmic->version;
	selection->boot = (struct monaco_pmic_boot_inputs){
		.topology = MONACO_PMIC_TOPOLOGY_INDEPENDENT,
		.present_slaves = 7,
		.reset_fsm_valid = true,
		.reset_fsm = s->raw_reset_fsm & UINT32_C(0xf),
		/* The PMIC platform type is 0 on this board, not the CDT board type. */
		.platform_type_valid = true,
		.platform_type = 0,
		.supply_policy_validated = supply_policy_validated,
		.regulators = ventuno_q_regulators,
		.regulator_count = MONACO_PMIC_BOOT_REGULATORS,
	};
	return selection->boot.reset_fsm >= 12 ? MONACO_PMIC_BOOT_SKIPPED : 0;
}
