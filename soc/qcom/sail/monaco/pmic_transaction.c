/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pmic_transaction.h"

#include <errno.h>

struct transaction_lease {
	struct monaco_pmic_transaction *owner;
	struct monaco_cold_boot *boot;
	struct monaco_pmic saved;
	const struct monaco_pmic_boot_result *tail;
	bool (*eligible)(void *context, const struct monaco_cold_boot *boot);
	int (*check)(void *context);
	void *context;
};

static bool same_pmic(const struct monaco_pmic *a, const struct monaco_pmic *b)
{
	return a->io.context == b->io.context && a->io.transfer == b->io.transfer &&
	       a->io.delay_us == b->io.delay_us &&
	       a->transfer_timeout_us == b->transfer_timeout_us && a->state == b->state &&
	       a->version.device == b->version.device &&
	       a->version.revision == b->version.revision &&
	       a->version.firmware == b->version.firmware &&
	       a->version.firmware_revision == b->version.firmware_revision;
}

static bool eligible(const struct transaction_lease *lease)
{
	const struct monaco_cold_boot *boot = lease->boot;

	return boot->io.read32 != NULL && lease->eligible(lease->context, boot) &&
	       boot->i2c.initialized && !boot->i2c.busy && boot->i2c.timer == &boot->timer &&
	       boot->pmic.io.context == boot;
}

static int guarded(void *context)
{
	const struct transaction_lease *lease = context;
	struct monaco_pmic_transaction_result *result = &lease->owner->result;
	uint32_t fsm;
	int rc;

	result->guards++;
	if (!lease->owner->claimed || lease->owner->boot != lease->boot || !eligible(lease) ||
	    lease->tail != &lease->boot->pmic_boot ||
	    !same_pmic(&lease->saved, &lease->boot->pmic)) {
		result->guard_error = -ESTALE;
		return result->guard_error;
	}
	rc = lease->check(lease->context);
	if (rc != 0) {
		result->guard_error = rc < 0 ? rc : -EIO;
		return result->guard_error;
	}
	rc = lease->boot->io.read32(lease->boot->io.context, UINT32_C(0xf1149024), &fsm);
	if (rc != 0) {
		result->guard_error = rc < 0 ? rc : -EIO;
		return result->guard_error;
	}
	if ((fsm & UINT32_C(0xf)) >= 12U || !eligible(lease) ||
	    !same_pmic(&lease->saved, &lease->boot->pmic)) {
		result->guard_error = -ESTALE;
		return result->guard_error;
	}
	return 0;
}

int monaco_pmic_transaction_run(struct monaco_pmic_transaction *transaction,
				struct monaco_cold_boot *boot,
				bool (*admission)(void *context,
						  const struct monaco_cold_boot *boot),
				int (*guard)(void *context), void *context)
{
	struct transaction_lease lease;
	struct monaco_pmic_transaction_result *result;
	int rc;

	if (transaction == NULL || boot == NULL || admission == NULL || guard == NULL) {
		return -EINVAL;
	}
	if (transaction->claimed) {
		return -EALREADY;
	}
	transaction->claimed = true;
	transaction->boot = boot;
	result = &transaction->result;
	result->phase = MONACO_PMIC_TRANSACTION_CLAIMED;
	lease = (struct transaction_lease){transaction, boot, boot->pmic, &boot->pmic_boot,
					   admission,   guard, context};
	if (!eligible(&lease)) {
		result->error = -EPERM;
		return result->error;
	}
	rc = guarded(&lease);
	if (rc != 0) {
		result->error = rc;
		return rc;
	}
	result->phase = MONACO_PMIC_TRANSACTION_CONFIGURING;
	rc = monaco_pmic_reset_configure_readback_guarded(&boot->pmic, &boot->pmic_snapshot,
							  &boot->pmic_boot, &result->pmic, guarded,
							  &lease);
	if (rc != 0) {
		result->error = rc;
		return rc;
	}
	result->phase = MONACO_PMIC_TRANSACTION_CONFIGURED;
	rc = guarded(&lease);
	if (rc != 0) {
		result->error = rc;
		return rc;
	}
	result->time_reads++;
	rc = monaco_timer_time_us(&boot->timer, &result->before_us);
	if (rc != 0) {
		result->time_error = rc;
		result->error = rc;
		return rc;
	}
	result->phase = MONACO_PMIC_TRANSACTION_DELAYING;
	rc = monaco_timer_delay_us(&boot->timer, 50000U);
	if (rc != 0) {
		result->time_error = rc;
		result->error = rc;
		return rc;
	}
	result->time_reads++;
	rc = monaco_timer_time_us(&boot->timer, &result->after_us);
	if (rc != 0) {
		result->time_error = rc;
		result->error = rc;
		return rc;
	}
	if (result->after_us < result->before_us || result->after_us - result->before_us < 50000U) {
		result->error = -ETIME;
		return result->error;
	}
	rc = guarded(&lease);
	if (rc != 0) {
		result->error = rc;
		return rc;
	}
	result->phase = MONACO_PMIC_TRANSACTION_DELAYED;
	return 0;
}

/* Shutdown requires the crash-debug control to be zero; it is never written here. */
static int shutdown_guarded(void *context)
{
	const struct transaction_lease *lease = context;
	struct monaco_pmic_transaction_result *result = &lease->owner->result;
	struct monaco_pmic_shutdown_control_result *control = &result->shutdown_control;
	uint32_t value;
	int rc;

	rc = guarded(context);
	if (rc != 0) {
		return rc;
	}
	control->reads_attempted++;
	control->valid = false;
	rc = lease->boot->io.read32(lease->boot->io.context, UINT32_C(0xf1149000), &value);
	control->read_status = rc;
	if (rc != 0) {
		result->guard_error = rc < 0 ? rc : -EIO;
		return result->guard_error;
	}
	control->reads_completed++;
	control->value = value;
	control->valid = true;
	/* The read callback may change service state, so check again. */
	rc = guarded(context);
	if (rc != 0) {
		return rc;
	}
	if (value != 0U) {
		result->guard_error = -ESTALE;
		return result->guard_error;
	}
	return 0;
}

int monaco_pmic_shutdown_transaction_run(struct monaco_pmic_transaction *transaction,
					 struct monaco_cold_boot *boot,
					 bool (*admission)(void *context,
							   const struct monaco_cold_boot *boot),
					 int (*guard)(void *context), void *context)
{
	struct transaction_lease lease;
	struct monaco_pmic_transaction_result *result;
	int rc;

	if (transaction == NULL || boot == NULL || admission == NULL || guard == NULL) {
		return -EINVAL;
	}
	if (transaction->claimed) {
		return -EALREADY;
	}
	transaction->claimed = true;
	transaction->boot = boot;
	result = &transaction->result;
	result->phase = MONACO_PMIC_TRANSACTION_CLAIMED;
	lease = (struct transaction_lease){transaction, boot, boot->pmic, &boot->pmic_boot,
					   admission,   guard, context};
	if (!eligible(&lease)) {
		result->error = -EPERM;
		return result->error;
	}
	rc = shutdown_guarded(&lease);
	if (rc != 0) {
		result->error = rc;
		return rc;
	}
	result->phase = MONACO_PMIC_TRANSACTION_CONFIGURING;
	rc = monaco_pmic_shutdown_configure_guarded(&boot->pmic, &boot->pmic_snapshot,
						    &boot->pmic_boot, &result->shutdown,
						    shutdown_guarded, &lease);
	if (rc != 0) {
		result->error = rc;
		return rc;
	}
	rc = shutdown_guarded(&lease);
	if (rc != 0) {
		result->error = rc;
		return rc;
	}
	result->phase = MONACO_PMIC_TRANSACTION_SHUTDOWN_VERIFIED;
	return 0;
}
