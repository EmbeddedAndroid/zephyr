/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BMC_H_
#define BMC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#define BMC_NOTICE_HISTORY 16U

struct bmc_notice {
	uint64_t at_ms;
	uint32_t word;
	uint8_t channel;
};

enum bmc_md_state {
	BMC_MD_UNKNOWN,
	BMC_MD_BOOTING,
	BMC_MD_RUNNING,
	BMC_MD_HUNG,
	BMC_MD_RESETTING,
	BMC_MD_SHUTDOWN,
};

struct bmc_md_status {
	enum bmc_md_state state;
	uint32_t notices;
	uint32_t last_word;
	uint64_t last_notice_ms;
	bool hb_configured;
	uint32_t hb_addr;
	uint32_t hb_value;
	uint64_t hb_changed_ms;
	uint32_t hb_changes;
	bool wdt_enabled;
	bool wdt_reset;
	bool wdt_armed;
	uint32_t wdt_timeout_s;
	uint32_t wdt_fired;
	uint32_t resets_requested;
	int last_reset_result;
	uint32_t dropped;
	bool retired;
};

const char *bmc_md_state_name(enum bmc_md_state state);
void bmc_md_start(void);
void bmc_md_notice(uint32_t word, uint32_t channel);
void bmc_md_retire(void);
int bmc_read32(uint32_t sail_addr, uint32_t *value);
void bmc_md_status_get(struct bmc_md_status *status);
size_t bmc_md_history(struct bmc_notice *out, size_t max);
void bmc_md_heartbeat_set(uint32_t addr);
void bmc_md_wdt_set(bool enabled, uint32_t timeout_s, bool reset);
int bmc_md_reset(const char *how);

#define BMC_SENSORS_MAX 16U

struct bmc_sensor {
	char name[12];
	int32_t value;
	char unit[4];
	bool valid;
};

size_t bmc_sensors_read(struct bmc_sensor *out, size_t max);

uint64_t bmc_uptime_ms(void);
void bmc_log_set(uint32_t period_ms);
uint32_t bmc_log_period(void);
const char *bmc_build_id(void);
void bmc_out(const char *fmt, ...);
bool bmc_readable(uint32_t sail_addr);
bool bmc_log_local(void);
enum bmc_thread {
	BMC_THREAD_PLATFORM,
	BMC_THREAD_MD,
	BMC_THREAD_LOG,
	BMC_THREAD_SHELL,
	BMC_THREADS,
};

/* Where each thread is pinned and has run. */
struct bmc_thread_cpus {
	const char *name;
	uint32_t pinned;
	uint32_t seen;
	uint32_t runs;
};

void bmc_cpu_mark(enum bmc_thread thread);
void bmc_cpu_report(enum bmc_thread thread, struct bmc_thread_cpus *out);
void bmc_cpu_place(enum bmc_thread thread, struct k_thread *tid);
uint32_t bmc_app_cpu(void);
int bmc_platform_start(void);
int bmc_platform_status(void);
uint32_t bmc_image_word(void);
void bmc_log_set_local(bool local);

#endif
