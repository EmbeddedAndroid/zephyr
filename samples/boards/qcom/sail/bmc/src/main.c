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

uint32_t bmc_image_word(void)
{
	return BMC_IMAGE_ID;
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

int main(void)
{
	int rc;

	printk("\nSAIL BMC %s boot_mode=0x%x boot_status=%d\n", bmc_build_id(),
	       (unsigned int)sail_boot_mode_get(), sail_boot_status());
	bmc_md_start();
	rc = bmc_platform_start();
	printk("SAIL BMC platform rc=%d\n", rc);
	bmc_log_set(CONFIG_SAMPLE_BMC_LOG_PERIOD_MS);

	for (;;) {
		uint32_t period = bmc_log_period();

		if (period != 0U) {
			log_line();
			k_msleep(period);
		} else {
			k_msleep(100);
		}
	}
	return 0;
}
