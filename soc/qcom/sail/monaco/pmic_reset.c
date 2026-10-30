/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic_reset.h"

static bool sample_ready(const struct monaco_pmic_policy_snapshot *s)
{
	if (s->profile != MONACO_PMIC_BOARD_VENTUNO_Q || !s->sampled || s->status != 0 ||
	    s->stage != MONACO_PMIC_POLICY_SAMPLED || s->last_callback_status != 0 ||
	    s->reads_attempted != 3 || s->reads_completed != 3 || s->probes_attempted != 3 ||
	    s->probes_completed != 3 || (s->raw_reset_fsm & UINT32_C(0xf)) >= 12 ||
	    (s->raw_soc_identity & UINT32_C(0xffff0000)) != UINT32_C(0x601a0000)) {
		return false;
	}
	switch (s->raw_sku & UINT32_C(0xffff)) {
	case 0x026e:
	case 0x026c:
	case 0x026d:
	case 0x028f:
	case 0x02e6:
	case 0x02e7:
	case 0x02fb:
		return true;
	default:
		return false;
	}
}

static bool boot_ready(const struct monaco_pmic *pmic, const struct monaco_pmic_boot_result *boot)
{
	const struct monaco_pmic *saved = &boot->tail_context;

	if (boot->tail_owner != pmic || boot->tail_result != boot || boot->status != 0 ||
	    boot->stage != MONACO_PMIC_BOOT_COMPLETE || pmic->state != MONACO_PMIC_CRC_READY ||
	    saved->state != MONACO_PMIC_CRC_READY || pmic->version.device != 0x82 ||
	    pmic->io.context != saved->io.context || pmic->io.transfer != saved->io.transfer ||
	    pmic->io.delay_us != saved->io.delay_us ||
	    pmic->transfer_timeout_us != saved->transfer_timeout_us ||
	    pmic->version.device != saved->version.device ||
	    pmic->version.revision != saved->version.revision ||
	    pmic->version.firmware != saved->version.firmware ||
	    pmic->version.firmware_revision != saved->version.firmware_revision ||
	    boot->operations_attempted != boot->operations_completed ||
	    boot->last_operation != MONACO_PMIC_BOOT_DELAY || boot->last_delay_us != 3000 ||
	    boot->last_callback_status != 0 || boot->delay_us_completed < 5000 ||
	    !boot->boot_bist_flags_valid ||
	    (!boot->boot_bist_accepted &&
	     (!boot->runtime_bist_passed || !boot->runtime_bist_cleanup_complete)) ||
	    boot->regulators_completed != MONACO_PMIC_BOOT_REGULATORS ||
	    boot->posttarget_writes_completed != 6 || boot->settle_time_reads_attempted != 2 ||
	    boot->settle_time_reads_completed != 2 || !boot->settle_elapsed_valid ||
	    boot->settle_elapsed_us < 3000) {
		return false;
	}
	for (unsigned int i = 0; i < MONACO_PMIC_BOOT_REGULATORS; ++i) {
		const struct monaco_pmic_regulator_result *rail = &boot->regulators[i];

		if (rail->stage != MONACO_PMIC_REGULATOR_COMPLETE || !rail->voltage_valid ||
		    !rail->mode_valid || !rail->enable_valid) {
			return false;
		}
	}
	return true;
}

struct reset_guard {
	int (*check)(void *context);
	void *context;
};

static int check_guard(const struct reset_guard *guard, struct monaco_pmic_reset_result *r)
{
	if (guard->check == NULL) {
		return 0;
	}
	r->guards_attempted++;
	r->guard_status = guard->check(guard->context);
	if (r->guard_status != 0) {
		r->status = r->guard_status < 0 ? r->guard_status : MONACO_PMIC_CALLBACK;
		return r->status;
	}
	r->guards_completed++;
	return 0;
}

static int write_policy(struct monaco_pmic *pmic, struct monaco_pmic_reset_result *r,
			enum monaco_pmic_reset_stage stage, uint8_t reg, uint8_t value,
			const struct reset_guard *guard)
{
	r->stage = stage;
	r->last_register = reg;
	if (check_guard(guard, r) != 0) {
		return r->status;
	}
	r->writes_attempted++;
	r->status = monaco_pmic_write(pmic, 0, reg, value);
	if (r->status == 0) {
		r->writes_completed++;
	}
	return r->status;
}

static int read_policy(struct monaco_pmic *pmic, struct monaco_pmic_reset_result *r,
		       enum monaco_pmic_reset_stage stage, uint8_t reg, uint8_t *value, bool *valid,
		       const struct reset_guard *guard)
{
	r->stage = stage;
	r->last_register = reg;
	if (check_guard(guard, r) != 0) {
		return r->status;
	}
	r->reads_attempted++;
	r->status = monaco_pmic_read(pmic, 0, reg, value);
	if (r->status == 0) {
		r->reads_completed++;
		*valid = true;
	}
	return r->status;
}

int monaco_pmic_reset_configure_readback_guarded(struct monaco_pmic *pmic,
						 const struct monaco_pmic_policy_snapshot *snapshot,
						 const struct monaco_pmic_boot_result *boot,
						 struct monaco_pmic_reset_result *r,
						 int (*check)(void *context), void *guard_context)
{
	const struct reset_guard guard = {check, guard_context};

	if (r == NULL) {
		return MONACO_PMIC_INVALID;
	}
	if (r->stage != MONACO_PMIC_RESET_FRESH) {
		return MONACO_PMIC_STATE;
	}
	*r = (struct monaco_pmic_reset_result){.stage = MONACO_PMIC_RESET_VALIDATE};
	if (pmic == NULL || pmic->io.transfer == NULL || pmic->io.delay_us == NULL ||
	    pmic->transfer_timeout_us == 0 || snapshot == NULL || boot == NULL) {
		r->status = MONACO_PMIC_INVALID;
		return r->status;
	}
	if (!sample_ready(snapshot) || !boot_ready(pmic, boot)) {
		r->status = MONACO_PMIC_STATE;
		return r->status;
	}
	if (write_policy(pmic, r, MONACO_PMIC_RESET_WRITE_C9, 0xc9, 0x00, &guard) != 0 ||
	    write_policy(pmic, r, MONACO_PMIC_RESET_WRITE_CB, 0xcb, 0x0a, &guard) != 0 ||
	    write_policy(pmic, r, MONACO_PMIC_RESET_WRITE_CC, 0xcc, 0x01, &guard) != 0 ||
	    read_policy(pmic, r, MONACO_PMIC_RESET_READ_C9, 0xc9, &r->observed_c9, &r->c9_valid,
			&guard) != 0 ||
	    read_policy(pmic, r, MONACO_PMIC_RESET_READ_CC, 0xcc, &r->observed_cc, &r->cc_valid,
			&guard) != 0) {
		return r->status;
	}
	r->stage = MONACO_PMIC_RESET_COMPARE;
	if (r->observed_c9 != 0x00 || r->observed_cc != 0x01) {
		r->status = MONACO_PMIC_RESET_VERIFY;
		return r->status;
	}
	if (check_guard(&guard, r) != 0) {
		return r->status;
	}
	r->stage = MONACO_PMIC_RESET_VERIFIED;
	return 0;
}

int monaco_pmic_reset_configure_readback(struct monaco_pmic *pmic,
					 const struct monaco_pmic_policy_snapshot *snapshot,
					 const struct monaco_pmic_boot_result *boot,
					 struct monaco_pmic_reset_result *result)
{
	return monaco_pmic_reset_configure_readback_guarded(pmic, snapshot, boot, result, NULL,
							    NULL);
}
