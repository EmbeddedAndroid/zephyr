/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hbcu.h"

#define CONTROL      UINT32_C(0xf8800004)
#define MBIST_FIRST  UINT32_C(0xf8800068)
#define LBIST_FIRST  UINT32_C(0xf8800074)
#define MBIST_STATUS UINT32_C(0xf0305024)
#define LBIST_STATUS UINT32_C(0xf0305030)

struct capture {
	const struct monaco_timer_io *io;
	struct monaco_hbcu_result *result;
};

bool monaco_hbcu_write_allowed(uint32_t address)
{
	switch (address) {
	case CONTROL:
	case MBIST_STATUS:
	case MBIST_STATUS + 4:
	case MBIST_STATUS + 8:
	case LBIST_STATUS:
	case LBIST_STATUS + 4:
	case LBIST_STATUS + 8:
		return true;
	default:
		return false;
	}
}

static int read32(struct capture *c, uint32_t address, uint32_t *value)
{
	struct monaco_hbcu_result *r = c->result;

	r->last_operation = MONACO_HBCU_READ;
	r->address = address;
	r->value = 0;
	r->callback_status = c->io->read32(c->io->context, address, value);
	if (r->callback_status) {
		return MONACO_HBCU_IO;
	}
	r->completed_reads++;
	r->value = *value;
	return 0;
}

static int write32(struct capture *c, uint32_t address, uint32_t value)
{
	struct monaco_hbcu_result *r = c->result;

	r->last_operation = MONACO_HBCU_WRITE;
	r->address = address;
	r->value = value;
	r->callback_status = c->io->write32(c->io->context, address, value);
	if (r->callback_status) {
		return MONACO_HBCU_IO;
	}
	r->completed_writes++;
	return 0;
}

static int test_capture(struct capture *c, uint32_t source, uint32_t status,
			struct monaco_hbcu_test_result *t, bool inject, int failure)
{
	int rc;

	rc = read32(c, source, &t->words[0]);
	if (rc != 0) {
		return rc;
	}
	t->captured |= 1U;
	rc = read32(c, source + 4, &t->words[1]);
	if (rc != 0) {
		return rc;
	}
	t->captured |= 2U;
	rc = write32(c, status, t->words[0]);
	if (rc != 0) {
		return rc;
	}
	t->published |= 1U;
	rc = write32(c, status + 4, t->words[1]);
	if (rc != 0) {
		return rc;
	}
	t->published |= 2U;
	if (!t->words[0] || !t->words[1]) {
		/* A missing input skips this test even when injection is requested. */
		rc = write32(c, status + 8, 0);
		if (rc != 0) {
			return rc;
		}
		t->published |= 4U;
		t->outcome = MONACO_HBCU_SKIPPED;
		return 0;
	}
	rc = read32(c, source + 8, &t->words[2]);
	if (rc != 0) {
		return rc;
	}
	t->captured |= 4U;
	rc = write32(c, status + 8, t->words[2]);
	if (rc != 0) {
		return rc;
	}
	t->published |= 4U;
	if (!t->words[2] || inject) {
		t->outcome = MONACO_HBCU_FAILED;
		return failure;
	}
	t->outcome = MONACO_HBCU_PASSED;
	return 0;
}

static int capture(struct capture *c, const struct monaco_hbcu_policy *p)
{
	int rc;
	struct monaco_hbcu_result *r = c->result;
	uint32_t control;

	r->phase = MONACO_HBCU_DISABLE;
	rc = read32(c, CONTROL, &control);
	if (rc != 0) {
		return rc;
	}
	rc = write32(c, CONTROL, control & ~UINT32_C(1));
	if (rc != 0) {
		return rc;
	}
	r->disabled = true;
	r->phase = MONACO_HBCU_MBIST;
	rc = test_capture(c, MBIST_FIRST, MBIST_STATUS, &r->mbist, (p->injected_failures & 1U) != 0,
			  MONACO_HBCU_MBIST_FAILED);
	if (rc != 0) {
		return rc;
	}
	r->phase = MONACO_HBCU_LBIST;
	rc = test_capture(c, LBIST_FIRST, LBIST_STATUS, &r->lbist, (p->injected_failures & 2U) != 0,
			  MONACO_HBCU_LBIST_FAILED);
	if (rc != 0) {
		return rc;
	}
	r->phase = MONACO_HBCU_COMPLETE;
	return 0;
}

int monaco_hbcu_capture(const struct monaco_timer_io *io, const struct monaco_hbcu_policy *policy,
			struct monaco_hbcu_result *result)
{
	struct capture c = {io, result};

	if (!result) {
		return MONACO_HBCU_INVALID;
	}
	*result = (struct monaco_hbcu_result){.phase = MONACO_HBCU_INPUT};
	if (!io || !io->read32 || !io->write32 || !policy || (policy->injected_failures & ~3U)) {
		result->status = MONACO_HBCU_INVALID;
	} else if (!policy->valid) {
		result->status = MONACO_HBCU_POLICY;
	} else {
		result->status = capture(&c, policy);
	}
	return result->status;
}
