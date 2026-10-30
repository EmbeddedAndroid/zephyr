/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_MONACO_POLICY_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_MONACO_POLICY_H_

#include "md_bist.h"

enum monaco_policy_result {
	MONACO_POLICY_READY = 0,
	MONACO_POLICY_WARM = 1,
	MONACO_POLICY_SKIP = 2,
	MONACO_POLICY_INVALID = -1,
	MONACO_POLICY_IO = -2,
	MONACO_POLICY_TIMEBASE = -3,
	MONACO_POLICY_MISSING_MD = -4,
	MONACO_POLICY_READY_TIMEOUT = -5,
	MONACO_POLICY_BOOT_STATE = -6,
	MONACO_POLICY_EDL = -7,
	MONACO_POLICY_MODE2 = -8,
	MONACO_POLICY_CORE = -9,
};

enum monaco_policy_stage {
	MONACO_STAGE_CORE,
	MONACO_STAGE_PRESENCE,
	MONACO_STAGE_CLASSIFY,
	MONACO_STAGE_NORMAL_READY,
	MONACO_STAGE_FUSES,
	MONACO_STAGE_PREPARED,
	MONACO_STAGE_WARM,
	MONACO_STAGE_COMMAND,
	MONACO_STAGE_COMPLETE,
};

struct monaco_policy_io {
	void *context;
	/* Ordered Device reads; the policy reader never writes. */
	int (*read32)(void *context, uint32_t address, uint32_t *value);
	/* Returns the MPIDR of the calling core. */
	int (*read_mpidr)(void *context, uint32_t *value);
	/* Running microsecond clock; both callbacks must return in bounded time. */
	int (*time_us)(void *context, uint64_t *value);
	int (*delay_us)(void *context, uint32_t value);
};

struct monaco_policy_limits {
	uint32_t presence_timeout_us;
	uint32_t ready_timeout_us;
	uint32_t max_presence_checks; /* 1..101. */
	uint32_t max_ready_waits;     /* Bounds the 1 s ready wait loop. */
};

struct monaco_policy_diagnostic {
	enum monaco_policy_stage stage;
	uint32_t address, observed, reads, presence_checks, ready_waits;
	int result;
};

/* Filled by prepare and consumed once by finish; valid flags mark successful reads. */
struct monaco_policy_snapshot {
	bool prepared, consumed;
	uint32_t mpidr, mode_word, skip_word;
	uint32_t normal_ready_word, warm_cookie;
	uint32_t command_parts[4]; /* B0, AC, A8, A4 as read, unmasked. */
	uint8_t command_parts_valid;
	bool warm_cookie_valid;
	struct md_bist_policy bist;
};

/*
 * Check the core, wait for the main domain and sample the BIST fuses.
 * Call before clock bring-up. Returns 0 or a negative MONACO_POLICY_ error.
 */
int monaco_policy_prepare(const struct monaco_policy_io *io,
			  const struct monaco_policy_limits *limits,
			  struct monaco_policy_snapshot *snapshot,
			  struct monaco_policy_diagnostic *diagnostic);
/*
 * Read the warm cookie and boot command and decide whether the BIST runs.
 * Call once per prepared snapshot, after clock bring-up.
 *
 * @retval MONACO_POLICY_READY the BIST should run; snapshot->bist is valid.
 * @retval MONACO_POLICY_WARM or MONACO_POLICY_SKIP the BIST is not needed.
 * @retval negative MONACO_POLICY_ error.
 */
int monaco_policy_finish(const struct monaco_policy_io *io, struct monaco_policy_snapshot *snapshot,
			 struct monaco_policy_diagnostic *diagnostic);

#endif
