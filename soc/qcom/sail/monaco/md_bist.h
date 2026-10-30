/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_MD_BIST_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_MD_BIST_H_

#include <stdbool.h>
#include <stdint.h>

/* Two-phase main-domain boot handshake. */
enum md_bist_result {
	MD_BIST_OK = 0,
	MD_BIST_SKIP_WARM = 1,
	MD_BIST_SKIP_EDL = 2,
	MD_BIST_SKIP_POLICY = 3,
	MD_BIST_INVALID = -1,
	MD_BIST_UNSUPPORTED = -2,
	MD_BIST_PREREQUISITE = -3,
	MD_BIST_IO = -4,
	MD_BIST_TIMEOUT = -5,
	MD_BIST_TIMEBASE = -6,
	MD_BIST_FORBIDDEN = -7,
	MD_BIST_ISOLATION = -8,
	MD_BIST_CLOCK_STATE = -9,
	MD_BIST_CALLBACK = -10,
};

enum md_bist_stage {
	MD_STAGE_POLICY,
	MD_STAGE_INITIAL_ISOLATION,
	MD_STAGE_PHASE1_READY,
	MD_STAGE_PHASE1_RESULT,
	MD_STAGE_ISOLATION_ENTRY,
	MD_STAGE_CLOCKS_OFF,
	MD_STAGE_CONTROL_SEQUENCE,
	MD_STAGE_CLOCKS_ON,
	MD_STAGE_CLOCK_MODEL,
	MD_STAGE_PHASE2_READY,
	MD_STAGE_PHASE2_RESULT,
	MD_STAGE_COMPLETE,
};

/* Handshake milestones; an event does not mean the self-test passed. */
enum md_bist_event {
	MD_EVENT_BEGIN,
	MD_EVENT_PHASE1_READY,
	MD_EVENT_PHASE2_REQUEST,
	MD_EVENT_PHASE2_READY,
	MD_EVENT_COMPLETE,
};

struct md_bist_policy {
	/* Filled in by the cold-boot policy reader. */
	bool snapshot_valid;
	bool md_presence_known;
	bool md_present;
	bool warm_cookie_valid;
	uint32_t warm_cookie;  /* MD 01ff900c, unchanged by policy evaluation. */
	uint32_t md_boot_word; /* F03050b0/ac/a8/a4, assembled MSB first. */
	uint8_t mode;          /* (read32(F8706100) >> 11) & 3 */
	bool skip;             /* (read32(C0786064) >> 8) & 1 */
	uint8_t core;
};

/*
 * Access assumptions for a normal cold boot: C0000000..FFFFFFFF mapped as
 * Device memory, ordered callbacks, exclusive access and the access
 * protection left in place by the PBL. The driver does not program
 * protection or recover from synchronous aborts.
 */
enum md_bist_access_contract {
	MD_BIST_ACCESS_UNSPECIFIED = 0,
	MD_BIST_ACCESS_PBL_COLD_INHERITED_DEVICE = 1,
};

struct md_bist_prerequisites {
	enum md_bist_access_contract access_contract;
	bool clocks_initialized;
	bool timer_validated;
	bool md_clock_model_adapter_validated;
};

struct md_bist_io {
	void *context;
	/* Accesses must be ordered Device accesses with no speculative reads. */
	int (*read32)(void *context, uint32_t address, uint32_t *value);
	int (*write32)(void *context, uint32_t address, uint32_t value);
	/* Monotonic time in microseconds. */
	int (*time_us)(void *context, uint64_t *value);
	/* Returns only after the requested delay has elapsed, or on error. */
	int (*delay_us)(void *context, uint32_t delay);
	/* Invalidates the cached main-domain clock state. */
	int (*invalidate_md_clocks)(void *context);
	/* Publishes handshake progress; called before observe. */
	int (*publish)(void *context, enum md_bist_event);
	/* Optional; for logging only and must not publish handshake state. */
	int (*observe)(void *context, enum md_bist_event);
};

struct md_bist_limits {
	/* Bounds each poll loop in case the timer stops advancing. */
	uint32_t max_poll_reads;
};

struct md_bist_diagnostic {
	enum md_bist_stage stage;
	uint32_t address;
	uint32_t observed;
	uint32_t completed_writes;
	int result;
};

int md_bist_assess_policy(const struct md_bist_policy *policy);
bool md_bist_write_allowed(uint32_t address);
int md_bist_run(const struct md_bist_policy *policy,
		const struct md_bist_prerequisites *prerequisites, const struct md_bist_io *io,
		const struct md_bist_limits *limits, struct md_bist_diagnostic *diagnostic);

#endif
