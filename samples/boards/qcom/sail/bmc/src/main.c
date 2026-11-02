/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <zephyr/sys/printk.h>
#include <soc.h>
#include "bmc.h"

#ifndef BMC_BUILD_ID
#define BMC_BUILD_ID "unversioned"
#endif

/* Image identifier the MD can read from the answer registers; 0 publishes nothing. */
#ifndef BMC_IMAGE_ID
#define BMC_IMAGE_ID 0
#endif
BUILD_ASSERT((BMC_IMAGE_ID >> 24) == 0, "BMC_IMAGE_ID must fit in 24 bits");

static atomic_t log_period_ms;
static atomic_t log_local;

const char *bmc_build_id(void)
{
	return BMC_BUILD_ID;
}

static const char *const thread_names[BMC_THREADS] = {"platform", "md", "log", "shell"};
static atomic_t thread_seen[BMC_THREADS];
static atomic_t thread_runs[BMC_THREADS];
static atomic_t thread_pinned[BMC_THREADS];

void bmc_cpu_mark(enum bmc_thread thread)
{
	unsigned int key = arch_irq_lock();

	atomic_or(&thread_seen[thread], BIT(arch_curr_cpu()->id));
	arch_irq_unlock(key);
	atomic_inc(&thread_runs[thread]);
}

void bmc_cpu_report(enum bmc_thread thread, struct bmc_thread_cpus *out)
{
	out->name = thread_names[thread];
	out->pinned = (uint32_t)atomic_get(&thread_pinned[thread]);
	out->seen = (uint32_t)atomic_get(&thread_seen[thread]);
	out->runs = (uint32_t)atomic_get(&thread_runs[thread]);
}

/*
 * Platform on CPU 0, application threads on CPU 1 when this image started it
 * (it stays in reset when the fuses pair it into lockstep).
 */
uint32_t bmc_app_cpu(void)
{
#if defined(CONFIG_SMP)
	return sail_core_status(0U, 1U) == 0 ? 1U : 0U;
#else
	return 0U;
#endif
}

/* Pin a thread that is not running yet, or that is blocked. */
void bmc_cpu_place(enum bmc_thread thread, struct k_thread *tid)
{
	uint32_t cpu = thread == BMC_THREAD_PLATFORM ? 0U : bmc_app_cpu();

#if defined(CONFIG_SMP)
	if (k_thread_cpu_pin(tid, (int)cpu) != 0) {
		return;
	}
#else
	ARG_UNUSED(tid);
#endif
	atomic_set(&thread_pinned[thread], (atomic_val_t)BIT(cpu));
}

/*
 * Image word flags: bit 24 core 1 started, bit 25 the MD and log threads ran
 * and only on CPU 1, bit 26 the platform thread ran and only on CPU 0.
 */
uint32_t bmc_image_word(void)
{
	uint32_t flags = 0U;

	if (BMC_IMAGE_ID == 0) {
		return 0U;
	}
	if (bmc_app_cpu() == 1U) {
		flags |= BIT(24);
	}
	if (atomic_get(&thread_seen[BMC_THREAD_MD]) == BIT(1) &&
	    atomic_get(&thread_seen[BMC_THREAD_LOG]) == BIT(1)) {
		flags |= BIT(25);
	}
	if (atomic_get(&thread_seen[BMC_THREAD_PLATFORM]) == BIT(0)) {
		flags |= BIT(26);
	}
	return (uint32_t)BMC_IMAGE_ID | flags;
}

uint64_t bmc_uptime_ms(void)
{
	return (uint64_t)k_uptime_get();
}

void bmc_log_set(uint32_t period_ms)
{
	atomic_set(&log_period_ms, (atomic_val_t)period_ms);
}

uint32_t bmc_log_period(void)
{
	return (uint32_t)atomic_get(&log_period_ms);
}

void bmc_log_set_local(bool local)
{
	atomic_set(&log_local, local ? 1 : 0);
}

bool bmc_log_local(void)
{
	return atomic_get(&log_local) != 0;
}

/* Asynchronous output goes through the shell so the prompt is redrawn after it. */
void bmc_out(const char *fmt, ...)
{
	const struct shell *sh = shell_backend_uart_get_ptr();
	va_list ap;

	va_start(ap, fmt);
	if (sh != NULL && sh->ctx->state == SHELL_STATE_ACTIVE) {
		shell_vfprintf(sh, SHELL_NORMAL, fmt, ap);
	} else {
		vprintk(fmt, ap);
	}
	va_end(ap);
}

/*
 * One machine-parseable line: key=value pairs, temperatures in deci-degrees C.
 * In local mode nothing outside SAIL is read.
 */
static void log_line(void)
{
	char line[256];
	struct bmc_md_status md;
	uint64_t now = bmc_uptime_ms();
	size_t len;

	bmc_md_status_get(&md);
	len = snprintf(line, sizeof(line), "BMC t=%llu md=%s notices=%u last=0x%08x hb=%s", now,
		       bmc_md_state_name(md.state), md.notices, md.last_word,
		       md.hb_configured ? "on" : "off");
	if (md.hb_configured && len < sizeof(line)) {
		len += snprintf(line + len, sizeof(line) - len, " hb_val=0x%08x hb_age_ms=%llu",
				md.hb_value, now - md.hb_changed_ms);
	}
	if (len < sizeof(line)) {
		len += snprintf(line + len, sizeof(line) - len, " wdt=%s",
				md.wdt_enabled ? (md.wdt_armed ? "armed" : "idle") : "off");
	}
	if (!bmc_log_local()) {
		struct bmc_sensor s[BMC_SENSORS_MAX];
		size_t n = bmc_sensors_read(s, ARRAY_SIZE(s));

		for (size_t i = 0; i < n && len < sizeof(line); i++) {
			if (s[i].valid) {
				len += snprintf(line + len, sizeof(line) - len, " %s=%d", s[i].name,
						s[i].value);
			} else {
				len += snprintf(line + len, sizeof(line) - len, " %s=na",
						s[i].name);
			}
		}
	}
	bmc_out("%s\n", line);
}

static void log_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		uint32_t period = bmc_log_period();

		bmc_cpu_mark(BMC_THREAD_LOG);
		if (period != 0U) {
			log_line();
			k_msleep(period);
		} else {
			k_msleep(100);
		}
	}
}

K_THREAD_STACK_DEFINE(log_stack, 4096);

/* The shell starts its thread itself; pin it once it blocks for input. */
static void place_shell(void)
{
	const struct shell *sh = shell_backend_uart_get_ptr();

	for (int i = 0; i < 100; i++) {
		if (sh == NULL || sh->ctx->tid == NULL) {
			return;
		}
		bmc_cpu_place(BMC_THREAD_SHELL, sh->thread);
		if (atomic_get(&thread_pinned[BMC_THREAD_SHELL]) != 0) {
			return;
		}
		k_msleep(10);
	}
}

int main(void)
{
	static struct k_thread log_tid;
	int rc;

	printk("\nSAIL BMC %s chip=0x%04x boot_mode=0x%x boot_status=%d app_cpu=%u\n",
	       bmc_build_id(), sail_chip_id_get(), (unsigned int)sail_boot_mode_get(),
	       sail_boot_status(), bmc_app_cpu());
	rc = bmc_platform_start();
	printk("SAIL BMC platform rc=%d\n", rc);
	bmc_md_start();
	bmc_log_set(CONFIG_SAMPLE_BMC_LOG_PERIOD_MS);
	k_thread_create(&log_tid, log_stack, K_THREAD_STACK_SIZEOF(log_stack), log_thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(4), 0, K_FOREVER);
	k_thread_name_set(&log_tid, "log");
	bmc_cpu_place(BMC_THREAD_LOG, &log_tid);
	k_thread_start(&log_tid);
	place_shell();
	return 0;
}
