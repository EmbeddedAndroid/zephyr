/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/arch/cpu.h>
#include <soc.h>
#include "bmc.h"

static int parse_u32(const struct shell *sh, const char *s, uint32_t *v)
{
	char *end;
	unsigned long x = strtoul(s, &end, 0);

	if (end == s || *end != '\0') {
		shell_error(sh, "bad number: %s", s);
		return -EINVAL;
	}
	*v = (uint32_t)x;
	return 0;
}

static int cmd_version(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "SAIL BMC %s boot_mode=0x%x boot_status=%d", bmc_build_id(),
		    (unsigned int)sail_boot_mode_get(), sail_boot_status());
	return 0;
}

static int cmd_md_status(const struct shell *sh, size_t argc, char **argv)
{
	struct bmc_md_status md;
	struct bmc_notice h[BMC_NOTICE_HISTORY];
	uint64_t now = bmc_uptime_ms();
	size_t n;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	bmc_md_status_get(&md);
	shell_print(sh, "uptime_ms=%llu md=%s notices=%u last=0x%08x last_age_ms=%llu", now,
		    bmc_md_state_name(md.state), md.notices, md.last_word,
		    md.notices != 0U ? now - md.last_notice_ms : 0U);
	shell_print(sh, "coordinator rc=%d md_access=%s dropped=%u", bmc_platform_status(),
		    md.retired ? "retired" : "on", md.dropped);
	if (md.hb_configured) {
		shell_print(sh, "heartbeat addr=0x%08x value=0x%08x changes=%u age_ms=%llu",
			    md.hb_addr, md.hb_value, md.hb_changes, now - md.hb_changed_ms);
	} else {
		shell_print(sh, "heartbeat off");
	}
	shell_print(sh, "wdt=%s timeout_s=%u action=%s fired=%u resets=%u last_reset_rc=%d",
		    md.wdt_enabled ? (md.wdt_armed ? "armed" : "idle") : "off", md.wdt_timeout_s,
		    md.wdt_reset ? "reset" : "log", md.wdt_fired, md.resets_requested,
		    md.last_reset_result);
	n = bmc_md_history(h, ARRAY_SIZE(h));
	for (size_t i = 0; i < n; i++) {
		shell_print(sh, "notice t=%llu ch=%u word=0x%08x", h[i].at_ms, h[i].channel,
			    h[i].word);
	}
	return 0;
}

static int cmd_md_hb(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t addr = 0U;

	ARG_UNUSED(argc);
	if (strcmp(argv[1], "off") != 0 && parse_u32(sh, argv[1], &addr) != 0) {
		return -EINVAL;
	}
	if (addr != 0U && ((addr & 3U) != 0U || addr >= 0x40000000U ||
			   !bmc_readable(addr + 0xc0000000U))) {
		shell_error(sh, "heartbeat address must be an aligned MD address SAIL can read");
		return -EINVAL;
	}
	bmc_md_heartbeat_set(addr);
	shell_print(sh, "heartbeat %s 0x%08x", addr != 0U ? "on" : "off", addr);
	return 0;
}

static int cmd_md_wdt(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t timeout = 10U;
	bool reset = false;

	if (strcmp(argv[1], "off") == 0) {
		bmc_md_wdt_set(false, 0U, false);
		shell_print(sh, "wdt off");
		return 0;
	}
	if (strcmp(argv[1], "on") != 0) {
		shell_error(sh, "usage: md wdt off | on [seconds] [reset]");
		return -EINVAL;
	}
	if (argc > 2 && parse_u32(sh, argv[2], &timeout) != 0) {
		return -EINVAL;
	}
	if (argc > 3) {
		if (strcmp(argv[3], "reset") != 0) {
			shell_error(sh, "unknown action: %s", argv[3]);
			return -EINVAL;
		}
		reset = true;
	}
	if (timeout == 0U || timeout > 3600U) {
		shell_error(sh, "timeout must be 1..3600 s");
		return -EINVAL;
	}
	bmc_md_wdt_set(true, timeout, reset);
	shell_print(sh, "wdt on timeout_s=%u action=%s (arms at the next heartbeat change)",
		    timeout, reset ? "reset" : "log");
	return 0;
}

static int cmd_md_reset(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	if (argc < 2 || strcmp(argv[1], "now") != 0) {
		shell_print(sh, "md reset now: reset the main domain (not on this board)");
		return -EINVAL;
	}
	rc = bmc_md_reset("shell");

	if (rc != 0) {
		shell_error(sh, "md reset failed: %d", rc);
	}
	return rc;
}

static int cmd_sensors(const struct shell *sh, size_t argc, char **argv)
{
	struct bmc_sensor s[BMC_SENSORS_MAX];
	size_t n = bmc_sensors_read(s, ARRAY_SIZE(s));

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (size_t i = 0; i < n; i++) {
		if (s[i].valid) {
			shell_print(sh, "%-8s max %d.%d C", s[i].name, s[i].value / 10,
				    abs(s[i].value % 10));
		} else {
			shell_print(sh, "%-8s no valid sample", s[i].name);
		}
	}
	return 0;
}

static int cmd_log(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t period = 1000U;

	if (strcmp(argv[1], "off") == 0) {
		bmc_log_set(0U);
	} else if (strcmp(argv[1], "on") == 0) {
		if (argc > 2 && parse_u32(sh, argv[2], &period) != 0) {
			return -EINVAL;
		}
		if (argc > 3 && strcmp(argv[3], "local") != 0) {
			shell_error(sh, "usage: log on [ms] [local]");
			return -EINVAL;
		}
		bmc_log_set_local(argc > 3);
		if (period < 100U) {
			shell_error(sh, "period must be at least 100 ms");
			return -EINVAL;
		}
		bmc_log_set(period);
	} else {
		shell_error(sh, "usage: log on [ms] [local] | off");
		return -EINVAL;
	}
	shell_print(sh, "log period_ms=%u local=%d", bmc_log_period(), bmc_log_local());
	return 0;
}

static int cmd_peek(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t addr, count = 1U;

	if (parse_u32(sh, argv[1], &addr) != 0 ||
	    (argc > 2 && parse_u32(sh, argv[2], &count) != 0)) {
		return -EINVAL;
	}
	if ((addr & 3U) != 0U || count == 0U || count > 64U) {
		shell_error(sh, "aligned address, count 1..64");
		return -EINVAL;
	}
	for (uint32_t i = 0U; i < count; i++) {
		if (!bmc_readable(addr + 4U * i)) {
			shell_error(sh, "0x%08x is not in a block SAIL can read", addr + 4U * i);
			return -EPERM;
		}
	}
	for (uint32_t i = 0U; i < count; i++) {
		uint32_t v;
		int rc = bmc_read32(addr + 4U * i, &v);

		if (rc != 0) {
			shell_error(sh, "0x%08x: main domain reset or off (%d)", addr + 4U * i, rc);
			return rc;
		}
		shell_print(sh, "0x%08x: 0x%08x", addr + 4U * i, v);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(md_cmds,
	SHELL_CMD(status, NULL, "Main-domain state, notices, heartbeat, watchdog", cmd_md_status),
	SHELL_CMD_ARG(hb, NULL, "<md-address|off> Heartbeat word written by the MD", cmd_md_hb,
		      2, 0),
	SHELL_CMD_ARG(wdt, NULL, "off | on [seconds] [reset]", cmd_md_wdt, 2, 2),
	SHELL_CMD_ARG(reset, NULL, "now: reset the main domain", cmd_md_reset, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(md, &md_cmds, "Main domain", NULL);
SHELL_CMD_REGISTER(sensors, NULL, "SoC temperatures", cmd_sensors);
SHELL_CMD_ARG_REGISTER(log, NULL, "on [ms] [local] | off: periodic status line", cmd_log, 2, 2);
SHELL_CMD_ARG_REGISTER(peek, NULL, "<address> [count]: read device words", cmd_peek, 2, 1);
SHELL_CMD_REGISTER(version, NULL, "Build fingerprint", cmd_version);
