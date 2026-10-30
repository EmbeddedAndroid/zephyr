/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "md_bist.h"

#include <zephyr/sys/util.h>

#define HIGH   BIT(31)

#define ISOLATION_CONTROL UINT32_C(0xf1000000)
#define ISOLATION_STATUS  UINT32_C(0xf1000008)
#define POWER_CONTROL_34  UINT32_C(0xf1000034)
#define ASILD_CONTROL     UINT32_C(0xf1111700)
#define ASILB_CONTROL     UINT32_C(0xf1110700)
#define DEMET_CONTROL     UINT32_C(0xf111d020)
#define ASILD_PARTIAL     UINT32_C(0xf111101c)
#define ASILB_PARTIAL     UINT32_C(0xf1110050)
#define TCSR_CONTROL      UINT32_C(0xf0301004)
#define TCSR_CONFIG_14    UINT32_C(0xf0305014)
#define TCSR_CONFIG_10    UINT32_C(0xf0305010)
#define PHASE_READY       UINT32_C(0xf03050c4)
#define PHASE_ACK         UINT32_C(0xf03050c8)
#define PHASE1_RESULT     UINT32_C(0xc1ff900c)
#define PHASE2_RESULT     UINT32_C(0xc1ffa01c)

struct run {
	const struct md_bist_io *io;
	const struct md_bist_limits *limits;
	struct md_bist_diagnostic *diagnostic;
	uint64_t last_time;
	bool have_time;
};

int md_bist_assess_policy(const struct md_bist_policy *p)
{
	if (!p || p->mode > 3 || p->core > 3) {
		return MD_BIST_INVALID;
	}
	if (!p->snapshot_valid || !p->md_presence_known || !p->warm_cookie_valid) {
		return MD_BIST_PREREQUISITE;
	}
	/* The skip conditions are evaluated in this order. */
	if (p->md_present && p->warm_cookie == 0xf) {
		return MD_BIST_SKIP_WARM;
	}
	if (p->md_boot_word == UINT32_C(0xfe111111)) {
		return MD_BIST_SKIP_EDL;
	}
	if (p->skip || p->mode == 0 || p->mode == 3) {
		return MD_BIST_SKIP_POLICY;
	}
	/* Mode 2 is not supported. */
	if (p->mode != 1 || p->core != 0) {
		return MD_BIST_UNSUPPORTED;
	}
	if (!p->md_present) {
		return MD_BIST_PREREQUISITE;
	}
	return MD_BIST_OK;
}

bool md_bist_write_allowed(uint32_t address)
{
	/* PBL status, PS_HOLD, DCLS and GPIO registers are never written. */
	switch (address) {
	case ISOLATION_CONTROL:
	case POWER_CONTROL_34:
	case ASILD_CONTROL:
	case ASILB_CONTROL:
	case DEMET_CONTROL:
	case ASILD_PARTIAL:
	case ASILB_PARTIAL:
	case TCSR_CONTROL:
	case TCSR_CONFIG_14:
	case TCSR_CONFIG_10:
	case PHASE_ACK:
	case PHASE1_RESULT:
	case PHASE2_RESULT:
		return true;
	default:
		return false;
	}
}

static int read_register(struct run *r, uint32_t address, uint32_t *value)
{
	r->diagnostic->address = address;
	if (r->io->read32(r->io->context, address, value)) {
		return MD_BIST_IO;
	}
	r->diagnostic->observed = *value;
	return 0;
}

static int write_register(struct run *r, uint32_t address, uint32_t value)
{
	r->diagnostic->address = address;
	if (!md_bist_write_allowed(address)) {
		return MD_BIST_FORBIDDEN;
	}
	if (r->io->write32(r->io->context, address, value)) {
		return MD_BIST_IO;
	}
	r->diagnostic->completed_writes++;
	return 0;
}

static int update(struct run *r, uint32_t address, uint32_t mask, uint32_t value)
{
	uint32_t old;
	int rc;

	if (value & ~mask) {
		return MD_BIST_INVALID;
	}
	rc = read_register(r, address, &old);
	if (rc != 0) {
		return rc;
	}
	return write_register(r, address, (old & ~mask) | value);
}

static int now(struct run *r, uint64_t *value)
{
	if (r->io->time_us(r->io->context, value)) {
		return MD_BIST_IO;
	}
	if (r->have_time && *value < r->last_time) {
		return MD_BIST_TIMEBASE;
	}
	r->last_time = *value;
	r->have_time = true;
	return 0;
}

static int poll(struct run *r, uint32_t address, uint32_t mask, uint32_t expected,
		uint32_t timeout_us)
{
	uint64_t start, current;
	uint32_t value;
	int rc = now(r, &start);

	if (rc != 0) {
		return rc;
	}
	for (uint32_t n = 0; n < r->limits->max_poll_reads; n++) {
		rc = read_register(r, address, &value);
		if (rc != 0) {
			return rc;
		}
		if ((value & mask) == expected) {
			return 0;
		}
		rc = now(r, &current);
		if (rc != 0) {
			return rc;
		}
		/* The deadline is inclusive: fail only when elapsed > limit. */
		if (current - start > timeout_us) {
			return MD_BIST_TIMEOUT;
		}
	}
	/* The read budget ran out; the timer may still be running. */
	return MD_BIST_TIMEBASE;
}

static int delay(struct run *r, uint32_t us)
{
	uint64_t start, end;
	int rc = now(r, &start);

	if (rc != 0) {
		return rc;
	}
	if (r->io->delay_us(r->io->context, us)) {
		return MD_BIST_IO;
	}
	rc = now(r, &end);
	if (rc != 0) {
		return rc;
	}
	return end - start >= us ? 0 : MD_BIST_TIMEBASE;
}

static int observe(struct run *r, enum md_bist_event event)
{
	if (r->io->publish(r->io->context, event)) {
		return MD_BIST_CALLBACK;
	}
	if (r->io->observe && r->io->observe(r->io->context, event)) {
		return MD_BIST_CALLBACK;
	}
	return 0;
}

static int result_bit(struct run *r, uint32_t address, uint32_t bit)
{
	int rc = update(r, address, bit, bit);

	return rc ? rc : poll(r, address, bit, bit, 10000);
}

/*
 * The off and on waits test bit 31 of each register with different
 * conditions; they are not a common "clocks ready" test.
 */
static int wait_clocks_off(struct run *r, unsigned int count)
{
	uint32_t d, b, m;
	int rc;

	rc = read_register(r, ASILB_CONTROL, &b);
	if (rc != 0) {
		return rc;
	}
	rc = read_register(r, ASILD_CONTROL, &d);
	if (rc != 0) {
		return rc;
	}
	rc = read_register(r, DEMET_CONTROL, &m);
	if (rc != 0) {
		return rc;
	}
	if (!(d & b & HIGH)) {
		return 0;
	}
	for (;;) {
		if (!(m & HIGH) || count > 250) {
			break;
		}
		rc = delay(r, 1);
		if (rc != 0) {
			return rc;
		}
		count++;
		rc = read_register(r, ASILB_CONTROL, &b);
		if (rc != 0) {
			return rc;
		}
		rc = read_register(r, ASILD_CONTROL, &d);
		if (rc != 0) {
			return rc;
		}
		rc = read_register(r, DEMET_CONTROL, &m);
		if (rc != 0) {
			return rc;
		}
		if (!(d & b & HIGH)) {
			break;
		}
	}
	if (count < 250) {
		return 0;
	}
	rc = read_register(r, ASILB_CONTROL, &b);
	if (rc != 0) {
		return rc;
	}
	if (b & HIGH) {
		return MD_BIST_CLOCK_STATE;
	}
	rc = read_register(r, ASILD_CONTROL, &d);
	if (rc != 0) {
		return rc;
	}
	if (d & HIGH) {
		return MD_BIST_CLOCK_STATE;
	}
	rc = read_register(r, DEMET_CONTROL, &m);
	if (rc != 0) {
		return rc;
	}
	return m & HIGH ? MD_BIST_CLOCK_STATE : 0;
}

static int wait_clocks_on(struct run *r)
{
	uint32_t d, b;
	unsigned int count = 0;
	int rc;

	rc = read_register(r, ASILB_CONTROL, &b);
	if (rc != 0) {
		return rc;
	}
	rc = read_register(r, ASILD_CONTROL, &d);
	if (rc != 0) {
		return rc;
	}
	if ((d | b) & HIGH) {
		return 0;
	}
	do {
		rc = delay(r, 1);
		if (rc != 0) {
			return rc;
		}
		count++;
		rc = read_register(r, ASILB_CONTROL, &b);
		if (rc != 0) {
			return rc;
		}
		rc = read_register(r, ASILD_CONTROL, &d);
		if (rc != 0) {
			return rc;
		}
	} while (!((d | b) & HIGH) && count <= 250);
	if (count < 250) {
		return 0;
	}
	rc = read_register(r, ASILB_CONTROL, &b);
	if (rc != 0) {
		return rc;
	}
	if (!(b & HIGH)) {
		return MD_BIST_CLOCK_STATE;
	}
	rc = read_register(r, ASILD_CONTROL, &d);
	if (rc != 0) {
		return rc;
	}
	return d & HIGH ? 0 : MD_BIST_CLOCK_STATE;
}

#define STAGE(value) (r->diagnostic->stage = (value))

static int active_mode1(struct run *r)
{
	uint32_t value;
	unsigned int initial_delay = 0;
	int rc;

	STAGE(MD_STAGE_INITIAL_ISOLATION);
	rc = poll(r, ISOLATION_STATUS, BIT(3), 0, 1000000);
	if (rc != 0) {
		return rc;
	}
	rc = observe(r, MD_EVENT_BEGIN);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_PHASE1_READY);
	rc = poll(r, PHASE_READY, 3, 1, 1000000);
	if (rc != 0) {
		return rc;
	}
	rc = observe(r, MD_EVENT_PHASE1_READY);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_PHASE1_RESULT);
	rc = result_bit(r, PHASE1_RESULT, 1);
	if (rc != 0) {
		return rc;
	}
	rc = result_bit(r, PHASE1_RESULT, 2);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, PHASE_ACK, 3, 1);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_ISOLATION_ENTRY);
	rc = update(r, ISOLATION_CONTROL, 2, 2);
	if (rc != 0) {
		return rc;
	}
	rc = read_register(r, ISOLATION_STATUS, &value);
	if (rc != 0) {
		return rc;
	}
	if (!(value & BIT(3))) {
		rc = delay(r, 1);
		if (rc != 0) {
			return rc;
		}
		initial_delay = 1;
		/* Sample twice after the delay; the second sample is the one used. */
		rc = read_register(r, ISOLATION_STATUS, &value);
		if (rc != 0) {
			return rc;
		}
		rc = read_register(r, ISOLATION_STATUS, &value);
		if (rc != 0) {
			return rc;
		}
		if (!(value & BIT(3))) {
			return MD_BIST_ISOLATION;
		}
	} else {
		/* Read again so the diagnostic holds the latest status. */
		rc = read_register(r, ISOLATION_STATUS, &value);
		if (rc != 0) {
			return rc;
		}
	}
	rc = update(r, ISOLATION_CONTROL, 2, 0);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_CLOCKS_OFF);
	rc = update(r, ASILD_CONTROL, 1, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, ASILB_CONTROL, 1, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, DEMET_CONTROL, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = wait_clocks_off(r, initial_delay);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_CONTROL_SEQUENCE);
	rc = update(r, ASILD_PARTIAL, 4, 4);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, ASILB_PARTIAL, 4, 4);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, TCSR_CONTROL, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = delay(r, 1);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, TCSR_CONFIG_14, 3, 0);
	if (rc != 0) {
		return rc;
	}
	/* Each field is cleared with its own read-modify-write, in this order. */
	rc = update(r, TCSR_CONFIG_10, 3, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, TCSR_CONFIG_10, 4, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, TCSR_CONFIG_10, 8, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, TCSR_CONTROL, 1, 0);
	if (rc != 0) {
		return rc;
	}
	rc = delay(r, 250);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, ISOLATION_CONTROL, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = delay(r, 1);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, ISOLATION_CONTROL, 1, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, ASILD_PARTIAL, 4, 0);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, ASILB_PARTIAL, 4, 0);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_CLOCKS_ON);
	rc = update(r, ASILD_CONTROL, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, ASILB_CONTROL, 1, 1);
	if (rc != 0) {
		return rc;
	}
	rc = wait_clocks_on(r);
	if (rc != 0) {
		return rc;
	}
	rc = write_register(r, POWER_CONTROL_34, 0x3ff);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_CLOCK_MODEL);
	if (r->io->invalidate_md_clocks(r->io->context)) {
		return MD_BIST_CALLBACK;
	}
	rc = observe(r, MD_EVENT_PHASE2_REQUEST);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_PHASE2_READY);
	rc = poll(r, PHASE_READY, 0xc, 4, 1000000);
	if (rc != 0) {
		return rc;
	}
	/* Settle 25 ms after phase 2 is ready; a failed poll returns without it. */
	rc = delay(r, 25000);
	if (rc != 0) {
		return rc;
	}
	rc = observe(r, MD_EVENT_PHASE2_READY);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_PHASE2_RESULT);
	rc = result_bit(r, PHASE2_RESULT, 1);
	if (rc != 0) {
		return rc;
	}
	rc = result_bit(r, PHASE2_RESULT, 2);
	if (rc != 0) {
		return rc;
	}
	rc = update(r, PHASE_ACK, 0xc, 4);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_CLOCK_MODEL);
	if (r->io->invalidate_md_clocks(r->io->context)) {
		return MD_BIST_CALLBACK;
	}
	/* Completion is published only after every step above has succeeded. */
	rc = observe(r, MD_EVENT_COMPLETE);
	if (rc != 0) {
		return rc;
	}
	STAGE(MD_STAGE_COMPLETE);
	return 0;
}

#undef STAGE

int md_bist_run(const struct md_bist_policy *policy,
		const struct md_bist_prerequisites *prerequisites, const struct md_bist_io *io,
		const struct md_bist_limits *limits, struct md_bist_diagnostic *diagnostic)
{
	struct run r = {.io = io, .limits = limits, .diagnostic = diagnostic};
	int rc;

	if (!diagnostic) {
		return MD_BIST_INVALID;
	}
	*diagnostic = (struct md_bist_diagnostic){.stage = MD_STAGE_POLICY};
	rc = md_bist_assess_policy(policy);
	if (rc != 0) {
		goto done;
	}
	if (!prerequisites ||
	    prerequisites->access_contract != MD_BIST_ACCESS_PBL_COLD_INHERITED_DEVICE ||
	    !prerequisites->clocks_initialized || !prerequisites->timer_validated ||
	    !prerequisites->md_clock_model_adapter_validated) {
		rc = MD_BIST_PREREQUISITE;
		goto done;
	}
	if (!io || !io->read32 || !io->write32 || !io->time_us || !io->delay_us ||
	    !io->invalidate_md_clocks || !io->publish || !limits || !limits->max_poll_reads) {
		rc = MD_BIST_INVALID;
		goto done;
	}
	rc = active_mode1(&r);
done:
	diagnostic->result = rc;
	return rc;
}
