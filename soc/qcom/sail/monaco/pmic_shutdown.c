/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic_shutdown.h"
#include "pmic_runtime_internal.h"

struct shutdown_guard {
	int (*check)(void *context);
	void *context;
};

static int guard(const struct shutdown_guard *g, struct monaco_pmic_shutdown_result *r)
{
	r->guards_attempted++;
	r->guard_status = g->check(g->context);
	if (r->guard_status != 0) {
		r->status = r->guard_status < 0 ? r->guard_status : MONACO_PMIC_CALLBACK;
		return r->status;
	}
	r->guards_completed++;
	return 0;
}

static int read_value(struct monaco_pmic *p, struct monaco_pmic_shutdown_result *r,
		      enum monaco_pmic_shutdown_stage stage, unsigned int slave, uint8_t reg,
		      uint8_t *value, uint8_t valid, const struct shutdown_guard *g)
{
	r->stage = stage;
	r->last_slave = slave;
	r->last_register = reg;
	if (guard(g, r) != 0) {
		return r->status;
	}
	r->reads_attempted++;
	r->status = monaco_pmic_read(p, slave, reg, value);
	if (r->status == 0) {
		r->reads_completed++;
		r->valid |= valid;
	}
	return r->status;
}

static int write_value(struct monaco_pmic *p, struct monaco_pmic_shutdown_result *r,
		       enum monaco_pmic_shutdown_stage stage, unsigned int slave, uint8_t reg,
		       uint8_t value, const struct shutdown_guard *g)
{
	r->stage = stage;
	r->last_slave = slave;
	r->last_register = reg;
	if (guard(g, r) != 0) {
		return r->status;
	}
	r->writes_attempted++;
	r->status = monaco_pmic_write(p, slave, reg, value);
	if (r->status == 0) {
		r->writes_completed++;
	}
	return r->status;
}

int monaco_pmic_shutdown_configure_guarded(struct monaco_pmic *p,
					   const struct monaco_pmic_policy_snapshot *snapshot,
					   const struct monaco_pmic_boot_result *boot,
					   struct monaco_pmic_shutdown_result *r,
					   int (*check)(void *context), void *context)
{
	const struct shutdown_guard g = {check, context};

	if (r == NULL) {
		return MONACO_PMIC_INVALID;
	}
	if (r->stage != MONACO_PMIC_SHUTDOWN_FRESH) {
		return MONACO_PMIC_STATE;
	}
	*r = (struct monaco_pmic_shutdown_result){.stage = MONACO_PMIC_SHUTDOWN_VALIDATE};
	if (p == NULL || snapshot == NULL || boot == NULL || check == NULL) {
		r->status = MONACO_PMIC_INVALID;
		return r->status;
	}
	if (!monaco_pmic_runtime_ready(p, snapshot, boot)) {
		r->status = MONACO_PMIC_STATE;
		return r->status;
	}
	if (read_value(p, r, MONACO_PMIC_SHUTDOWN_READ_FORCE, 0, 0x82, &r->force, 1, &g) != 0) {
		return r->status;
	}
	if ((r->force & 8U) != 0U) {
		r->status = MONACO_PMIC_RESET_VERIFY;
		return r->status;
	}
	if (read_value(p, r, MONACO_PMIC_SHUTDOWN_READ_SLAVE2, 2, 9, &r->slave2_before, 2, &g) !=
		    0 ||
	    write_value(p, r, MONACO_PMIC_SHUTDOWN_WRITE_SLAVE2, 2, 9, r->slave2_before & 0xbfU,
			&g) != 0 ||
	    read_value(p, r, MONACO_PMIC_SHUTDOWN_READ_ENABLE, 0, 0x80, &r->enable_before, 4, &g) !=
		    0 ||
	    write_value(p, r, MONACO_PMIC_SHUTDOWN_WRITE_ENABLE, 0, 0x80, r->enable_before & 0xfeU,
			&g) != 0 ||
	    read_value(p, r, MONACO_PMIC_SHUTDOWN_READ_FEEDBACK, 0, 0x82, &r->feedback, 8, &g) !=
		    0) {
		return r->status;
	}
	if ((r->feedback & 1U) != 0U) {
		r->status = MONACO_PMIC_RESET_VERIFY;
		return r->status;
	}
	if (write_value(p, r, MONACO_PMIC_SHUTDOWN_WRITE_C9, 0, 0xc9, 0, &g) != 0 ||
	    write_value(p, r, MONACO_PMIC_SHUTDOWN_WRITE_CB, 0, 0xcb, 0x0a, &g) != 0 ||
	    write_value(p, r, MONACO_PMIC_SHUTDOWN_WRITE_CC, 0, 0xcc, 0, &g) != 0 ||
	    read_value(p, r, MONACO_PMIC_SHUTDOWN_READ_C9, 0, 0xc9, &r->observed_c9, 16, &g) != 0 ||
	    read_value(p, r, MONACO_PMIC_SHUTDOWN_READ_CC, 0, 0xcc, &r->observed_cc, 32, &g) != 0) {
		return r->status;
	}
	if (r->observed_c9 != 0 || r->observed_cc != 0) {
		r->status = MONACO_PMIC_RESET_VERIFY;
		return r->status;
	}
	if (guard(&g, r) != 0) {
		return r->status;
	}
	r->stage = MONACO_PMIC_SHUTDOWN_VERIFIED;
	return 0;
}
