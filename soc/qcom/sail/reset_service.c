/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "reset_service_internal.h"
#include "md_protocol.h"

#include <errno.h>
#include <limits.h>
#include <zephyr/irq.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(!IS_ENABLED(CONFIG_USERSPACE));

static int thread_context(void)
{
	unsigned int key;
	bool enabled;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}
	key = arch_irq_lock();
	enabled = arch_irq_unlocked(key);
	arch_irq_unlock(key);
	return enabled ? 0 : -EWOULDBLOCK;
}

/*
 * Under SMP the worker must stay on CPU 0. Enabling the receiver routes the
 * doorbell interrupts to the CPU that does it, and the cold boot state, the
 * PMIC bus and the reset controls are only used by the worker.
 */
static bool worker_pinned(void)
{
#ifdef CONFIG_SMP
	return arch_curr_cpu()->id == 0U && k_current_get()->base.cpu_mask == BIT(0);
#else
	return true;
#endif
}

static int worker_context(const struct sail_reset_service *s)
{
	int rc = thread_context();

	if (rc != 0) {
		return rc;
	}
	if (!s->attempted) {
		return -EINVAL;
	}
	if (s->worker != k_current_get()) {
		return -EPERM;
	}
	return s->busy ? -EBUSY : 0;
}

static bool eligible(void *context, const struct monaco_cold_boot *boot)
{
	const struct sail_reset_service *s = context;

	if (s->boot != boot || s->boot_result == NULL || s->boot_mode == NULL ||
	    *s->boot_mode != SAIL_BOOT_PCINIT_DEFERRED ||
	    *s->boot_result != MONACO_COLD_PCINIT_REQUIRED ||
	    !sail_boot_timer_qualified(boot, *s->boot_result, *s->boot_mode)) {
		return false;
	}
	if (!s->pmic) {
		return !monaco_cold_boot_has_pmic(boot);
	}
	return monaco_cold_boot_has_pmic(boot) && boot->i2c.initialized && !boot->i2c.busy &&
	       boot->i2c.timer == &boot->timer && boot->pmic.io.context == boot;
}

/* Caller holds the service lock. RX callbacks have finished before this snapshot. */
static int history(struct sail_reset_service *s)
{
	struct sail_md_receive_stats *r = &s->status.receive;
	int rc = sail_md_receive_stats_get(&s->receiver, r);

	if (rc != 0) {
		return rc;
	}
	if (!s->active || r->state != SAIL_MD_RECEIVE_RUNNING || r->registered_mask != 3U ||
	    r->possibly_enabled_mask != 3U || r->last_start_error != 0 || r->last_stop_error != 0 ||
	    r->cleanup_error != 0 || r->queue_errors != 0U || r->first_queue_error != 0 ||
	    r->malformed != 0U) {
		return -EIO;
	}
	for (uint32_t i = 0U; i < SAIL_MD_RECEIVE_CHANNELS; i++) {
		if (r->overflow[i] != 0U || r->inactive[i] != 0U ||
		    r->queued[i] != r->callbacks[i] || r->queued[i] == UINT32_MAX ||
		    r->callbacks[i] == UINT32_MAX) {
			return -ENOBUFS;
		}
	}
	if (r->pending > SAIL_MD_RECEIVE_DEPTH ||
	    (uint64_t)r->queued[0] + r->queued[1] != s->status.consumed + r->pending) {
		return -EIO;
	}
	/* Only the SDI-disable notice may follow, and only after the reset reply. */
	return s->status.claimed && s->status.phase != SAIL_RESET_SERVICE_ACKED && r->pending != 0U
		       ? -EALREADY
		       : 0;
}

static int live(void *context)
{
	struct sail_reset_service *s = context;
	k_spinlock_key_t key = k_spin_lock(&s->lock);
	int rc = s->busy && s->status.claimed && s->status.retirement_attempted &&
				 s->status.retirement_error == 0 && eligible(s, s->boot)
			 ? history(s)
			 : -ESTALE;

	k_spin_unlock(&s->lock, key);
	return rc;
}

static int cleanup(struct sail_reset_service *s)
{
	int rc = s->receiver.device != NULL ? sail_md_receive_stop(&s->receiver) : 0;

	/* Preserve the first cleanup failure even when a later explicit stop succeeds. */
	if (s->status.cleanup_error == 0) {
		s->status.cleanup_error = rc;
	}
	return rc;
}

static int failed(struct sail_reset_service *s, int error)
{
	k_spinlock_key_t key = k_spin_lock(&s->lock);

	if (s->status.error == 0) {
		s->status.error = error < 0 ? error : -EIO;
	}
	s->status.phase = SAIL_RESET_SERVICE_FAILED;
	if (s->active) {
		s->active = false;
		(void)cleanup(s);
	} else if (s->status.cleanup_error == 0) {
		s->status.cleanup_error = s->receiver.stats.cleanup_error;
	}
	s->busy = false;
	k_spin_unlock(&s->lock, key);
	return s->status.error;
}

int sail_reset_start(struct sail_reset_service *s, struct monaco_cold_boot *boot, const int *result,
		     const enum sail_boot_mode *mode, const struct device *device,
		     const struct monaco_timer_io *controls,
		     int (*reply)(void *context, uint32_t answer, uint32_t index), uint32_t entry,
		     int (*retire)(void *context), void *context)
{
	k_spinlock_key_t key;
	int rc = thread_context();

	if (rc != 0) {
		return rc;
	}
	if (boot == NULL || result == NULL || mode == NULL || device == NULL || controls == NULL ||
	    controls->read32 == NULL || controls->write32 == NULL || reply == NULL ||
	    retire == NULL) {
		return -EINVAL;
	}
	if (!worker_pinned()) {
		return -EPERM;
	}
	key = k_spin_lock(&s->lock);
	if (s->attempted || s->timer_busy) {
		rc = s->attempted ? -EALREADY : -EBUSY;
		k_spin_unlock(&s->lock, key);
		return rc;
	}
	s->attempted = true;
	s->busy = true;
	s->worker = k_current_get();
	s->boot = boot;
	s->boot_result = result;
	s->boot_mode = mode;
	s->controls = *controls;
	s->reply = reply;
	s->entry = entry;
	s->retire = retire;
	s->context = context;
	s->pmic = monaco_cold_boot_has_pmic(boot);
	if (entry != 0x0801d400U || !eligible(s, boot)) {
		k_spin_unlock(&s->lock, key);
		return failed(s, -EPERM);
	}
	rc = sail_md_receive_init(&s->receiver, device);
	if (rc == 0) {
		rc = sail_md_receive_start(&s->receiver);
	}
	if (rc == 0) {
		s->active = true;
		rc = history(s);
	}
	if (rc == 0) {
		s->status.phase = SAIL_RESET_SERVICE_LISTENING;
		s->busy = false;
	}
	k_spin_unlock(&s->lock, key);
	return rc != 0 ? failed(s, rc) : 0;
}

static int accept(struct sail_reset_service *s)
{
	const struct sail_md_receive_event *event = &s->status.event;
	int rc;

	s->status.word = (uint32_t)event->bytes[0] | (uint32_t)event->bytes[1] << 8 |
			 (uint32_t)event->bytes[2] << 16 | (uint32_t)event->bytes[3] << 24;
	if (s->status.phase == SAIL_RESET_SERVICE_ACKED) {
		return event->channel == 1U &&
			       sail_md_command_decode(s->status.word) == SAIL_MD_COMMAND_SDI_NOTICE
			       ? 0
			       : -EPROTO;
	}
	switch (sail_md_command_decode(s->status.word)) {
	case SAIL_MD_COMMAND_DDR_READY:
		return event->channel == 0U && s->status.consumed == 1U ? 0 : -EPROTO;
	case SAIL_MD_COMMAND_XBL_BOOT_SUCCESS:
		if (event->channel != 0U || s->xbl_success_seen) {
			return -EPROTO;
		}
		s->xbl_success_seen = true;
		return 0;
	case SAIL_MD_COMMAND_SECURE_BOOT_NOTICE:
		/* Sent once by the secure firmware at boot; it expects no answer. */
		if (event->channel != 1U || s->secure_boot_seen || s->status.claimed) {
			return -EPROTO;
		}
		s->secure_boot_seen = true;
		return 0;
	case SAIL_MD_COMMAND_HYP_BOOT_NOTICE:
		/* The hypervisor's counterpart, on the non-secure channel. */
		if (event->channel != 0U || s->hyp_boot_seen || s->status.claimed) {
			return -EPROTO;
		}
		s->hyp_boot_seen = true;
		return 0;
	case SAIL_MD_COMMAND_SDI_NOTICE:
		/* Also sent while the main domain boots; answered in poll. */
		return event->channel == 1U && !s->status.claimed ? 0 : -EPROTO;
	case SAIL_MD_COMMAND_SOC_HR:
	case SAIL_MD_COMMAND_WARM_RESET:
	case SAIL_MD_COMMAND_GRACEFUL_SHUTDOWN:
		if (event->channel != 1U || s->status.claimed) {
			return -EPERM;
		}
		s->status.claimed = true;
		rc = history(s);
		if (rc != 0) {
			return rc;
		}
		/* Set before calling the application: a failed retire may have partial effects. */
		s->status.retirement_attempted = true;
		s->status.phase = SAIL_RESET_SERVICE_RETIRED;
		rc = s->retire(s->context);
		s->status.retirement_error = rc < 0 ? rc : rc != 0 ? -EIO : 0;
		return rc != 0 ? s->status.retirement_error : history(s);
	default:
		return -EPROTO;
	}
}

/*
 * Caller holds the service lock: four ordered byte stores of one answer word. Only
 * the reset answer is counted per store; SDI answers are counted when complete.
 */
static int answer(struct sail_reset_service *s, uint32_t word)
{
	bool reset = word != UINT32_C(0xaa0c0000);

	for (uint32_t i = 0U; i < 4U; i++) {
		int rc;

		s->status.reply_attempted += reset;
		rc = s->reply(s->controls.context, word, i);
		if (rc != 0) {
			return rc;
		}
		s->status.reply_completed += reset;
	}
	s->status.sdi_answers += !reset;
	return 0;
}

/*
 * Without a SAIL PMIC the main domain's PS_HOLD drop does the reset or
 * power-off. An armed reset debug control would turn it into a debug reset,
 * so it is cleared and the main-domain PS_HOLD mask restored first.
 */
static int disarm_reset_debug(struct sail_reset_service *s)
{
	struct sail_reset_debug_result *d = &s->status.reset_debug;
	const struct monaco_timer_io *io = &s->controls;
	int rc = live(s);

	if (rc == 0) {
		d->reads++;
		rc = io->read32(io->context, SAIL_RESET_DEBUG_ENABLE, &d->before);
	}
	if (rc != 0) {
		return rc < 0 ? rc : -EIO;
	}
	d->valid = true;
	rc = live(s);
	if (rc != 0 || d->before == 0U) {
		return rc;
	}
	d->writes++;
	rc = io->write32(io->context, SAIL_RESET_DEBUG_ENABLE, 0U);
	if (rc == 0) {
		d->writes++;
		rc = io->write32(io->context, SAIL_MD_PS_HOLD_MASK, SAIL_MD_PS_HOLD_MASK_DEFAULT);
	}
	if (rc == 0) {
		d->reads++;
		rc = io->read32(io->context, SAIL_RESET_DEBUG_ENABLE, &d->after);
	}
	if (rc == 0) {
		d->reads++;
		rc = io->read32(io->context, SAIL_MD_PS_HOLD_MASK, &d->mask_after);
	}
	if (rc != 0) {
		return rc < 0 ? rc : -EIO;
	}
	if (d->after != 0U || (d->mask_after & 0xffffU) != SAIL_MD_PS_HOLD_MASK_DEFAULT) {
		return -EIO;
	}
	return live(s);
}

int sail_reset_poll(struct sail_reset_service *s)
{
	k_spinlock_key_t key;
	int rc = worker_context(s);

	if (rc != 0) {
		return rc;
	}
	key = k_spin_lock(&s->lock);
	if (!s->active) {
		rc = s->status.error != 0 ? s->status.error : -ESHUTDOWN;
		k_spin_unlock(&s->lock, key);
		return rc;
	}
	s->busy = true;
	rc = eligible(s, s->boot) && !s->timer_busy ? history(s) : -ESTALE;
	if (rc == 0) {
		rc = sail_md_receive_get(&s->receiver, &s->status.event);
	}
	if (rc == -ENOMSG) {
		s->busy = false;
		k_spin_unlock(&s->lock, key);
		return 0;
	}
	if (rc == 0) {
		s->status.consumed++;
		rc = history(s);
	}
	if (rc == 0) {
		rc = accept(s);
	}
	/* accept() admits this only before a claim or right after the reset answer. */
	if (rc == 0 && sail_md_command_decode(s->status.word) == SAIL_MD_COMMAND_SDI_NOTICE) {
		rc = answer(s, UINT32_C(0xaa0c0000));
		if (rc == 0 && s->status.phase == SAIL_RESET_SERVICE_ACKED) {
			s->active = false;
			rc = cleanup(s);
			if (rc == 0) {
				s->status.phase = SAIL_RESET_SERVICE_COMPLETE;
			}
		}
		if (rc == 0) {
			s->busy = false;
			k_spin_unlock(&s->lock, key);
			return 1;
		}
	}
	k_spin_unlock(&s->lock, key);
	if (rc != 0) {
		return failed(s, rc);
	}
	if (!s->status.claimed) {
		s->busy = false;
		return 1;
	}

	/* IRQs remain enabled throughout PMIC work for either accepted command. */
	if (!s->pmic) {
		s->status.phase = SAIL_RESET_SERVICE_PREPARING;
		rc = disarm_reset_debug(s);
		if (rc != 0) {
			return failed(s, rc);
		}
	} else if (s->status.word == UINT32_C(0xcd050000)) {
		s->status.phase = SAIL_RESET_SERVICE_PMIC;
		rc = monaco_pmic_warm_transaction_run(&s->transaction, s->boot, eligible, live, s);
		s->status.transaction = s->transaction.result;
		if (rc != 0) {
			return failed(s, rc);
		}
		if (s->status.transaction.phase != MONACO_PMIC_TRANSACTION_WARM_VERIFIED ||
		    s->status.transaction.pmic.stage != MONACO_PMIC_RESET_VERIFIED) {
			return failed(s, -EIO);
		}
	} else if (s->status.word == UINT32_C(0xcd060000)) {
		s->status.phase = SAIL_RESET_SERVICE_PMIC;
		rc = monaco_pmic_shutdown_transaction_run(&s->transaction, s->boot, eligible, live,
							  s);
		s->status.transaction = s->transaction.result;
		if (rc != 0) {
			return failed(s, rc);
		}
		if (s->status.transaction.phase != MONACO_PMIC_TRANSACTION_SHUTDOWN_VERIFIED ||
		    s->status.transaction.shutdown.stage != MONACO_PMIC_SHUTDOWN_VERIFIED) {
			return failed(s, -EIO);
		}
	} else {
		/* IRQs remain enabled during control preparation, PMIC I/O and delay. */
		s->status.phase = SAIL_RESET_SERVICE_PREPARING;
		rc = monaco_reset_prepare(&s->controls, s->entry, &s->status.preparation, live, s);
		if (rc == 0) {
			s->status.phase = SAIL_RESET_SERVICE_PMIC;
			rc = monaco_pmic_transaction_run(&s->transaction, s->boot, eligible, live,
							 s);
		}
		s->status.transaction = s->transaction.result;
		if (rc != 0) {
			return failed(s, rc);
		}
		if (s->status.transaction.phase != MONACO_PMIC_TRANSACTION_DELAYED ||
		    s->status.transaction.pmic.stage != MONACO_PMIC_RESET_VERIFIED) {
			return failed(s, -EIO);
		}
	}

	key = k_spin_lock(&s->lock);
	rc = eligible(s, s->boot) ? history(s) : -ESTALE;
	if (rc == 0) {
		s->status.phase = SAIL_RESET_SERVICE_REPLY;
		/* The stock firmware answers a warm reset with 0xaa070000. */
		rc = answer(s, s->status.word == UINT32_C(0xcd050000) ? UINT32_C(0xaa070000)
								    : UINT32_C(0xaa030000));
	}
	if (rc == 0) {
		/* The MD may follow with its SDI-disable notice; keep receiving for it. */
		s->status.phase = SAIL_RESET_SERVICE_ACKED;
		s->busy = false;
	}
	k_spin_unlock(&s->lock, key);
	return rc != 0 ? failed(s, rc) : 1;
}

int sail_reset_stop(struct sail_reset_service *s)
{
	int rc = worker_context(s);
	k_spinlock_key_t key;

	if (rc != 0) {
		return rc;
	}
	key = k_spin_lock(&s->lock);
	if (s->active) {
		s->status.error = -ESHUTDOWN;
		s->status.phase = SAIL_RESET_SERVICE_FAILED;
	}
	s->active = false;
	rc = cleanup(s);
	k_spin_unlock(&s->lock, key);
	return rc;
}

int sail_reset_status_get(struct sail_reset_service *s, struct sail_reset_service_status *out)
{
	int rc = worker_context(s);
	k_spinlock_key_t key;

	if (rc != 0 || out == NULL) {
		return rc != 0 ? rc : -EINVAL;
	}
	key = k_spin_lock(&s->lock);
	if (s->receiver.device != NULL) {
		rc = sail_md_receive_stats_get(&s->receiver, &s->status.receive);
	}
	if (rc == 0) {
		*out = s->status;
	}
	k_spin_unlock(&s->lock, key);
	return rc;
}

int sail_reset_notice_get(struct sail_reset_service *s, struct sail_reset_service_notice *out)
{
	int rc = worker_context(s);
	k_spinlock_key_t key;

	if (rc != 0 || out == NULL) {
		return rc != 0 ? rc : -EINVAL;
	}
	key = k_spin_lock(&s->lock);
	if (s->status.consumed == 0U) {
		rc = -ENOMSG;
	} else {
		const uint8_t *b = s->status.event.bytes;

		out->sequence = s->status.consumed;
		out->word = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
			    (uint32_t)b[3] << 24;
		out->channel = s->status.event.channel;
	}
	k_spin_unlock(&s->lock, key);
	return rc;
}

int sail_reset_publish(struct sail_reset_service *s, uint32_t word,
		       int (*write)(void *context, uint32_t word, uint32_t index))
{
	int rc = worker_context(s);
	k_spinlock_key_t key;

	if (rc != 0 || write == NULL) {
		return rc != 0 ? rc : -EINVAL;
	}
	/* The main domain waits for 0xaaXX0000 answers; never imitate one. */
	if (word == 0U || (word >> 24) == 0xaaU) {
		return -EINVAL;
	}
	key = k_spin_lock(&s->lock);
	if (!s->active || s->status.phase != SAIL_RESET_SERVICE_LISTENING ||
	    s->status.claimed) {
		rc = -EPERM;
	}
	for (uint32_t i = 0U; rc == 0 && i < 4U; i++) {
		rc = write(s->controls.context, word, i);
	}
	k_spin_unlock(&s->lock, key);
	return rc;
}

int sail_reset_timer_enter(struct sail_reset_service *s)
{
	int rc = thread_context();
	k_spinlock_key_t key;

	if (rc != 0) {
		return rc;
	}
	key = k_spin_lock(&s->lock);
	if (s->timer_busy ||
	    (s->attempted && (s->worker != k_current_get() || s->busy || s->status.claimed))) {
		rc = -EBUSY;
	} else {
		s->timer_busy = true;
	}
	k_spin_unlock(&s->lock, key);
	return rc;
}

void sail_reset_timer_leave(struct sail_reset_service *s)
{
	k_spinlock_key_t key = k_spin_lock(&s->lock);

	s->timer_busy = false;
	k_spin_unlock(&s->lock, key);
}
