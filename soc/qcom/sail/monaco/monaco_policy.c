/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "monaco_policy.h"

#define MD_STAGE    UINT32_C(0xf03050c0)
#define CMD_B0      UINT32_C(0xf03050b0)
#define CMD_AC      UINT32_C(0xf03050ac)
#define CMD_A8      UINT32_C(0xf03050a8)
#define CMD_A4      UINT32_C(0xf03050a4)
#define MODE_FUSE   UINT32_C(0xf8706100)
#define SKIP_FUSE   UINT32_C(0xc0786064)
#define WARM_COOKIE UINT32_C(0xc1ff900c)

struct reader {
	const struct monaco_policy_io *io;
	struct monaco_policy_diagnostic *d;
	uint64_t last_time;
	bool have_time;
};

static int read32(struct reader *r, uint32_t address, uint32_t *value)
{
	r->d->address = address;
	if (r->io->read32(r->io->context, address, value)) {
		return MONACO_POLICY_IO;
	}
	r->d->reads++;
	r->d->observed = *value;
	return 0;
}

static int now(struct reader *r, uint64_t *value)
{
	if (r->io->time_us(r->io->context, value)) {
		return MONACO_POLICY_IO;
	}
	if (r->have_time && *value < r->last_time) {
		return MONACO_POLICY_TIMEBASE;
	}
	r->have_time = true;
	r->last_time = *value;
	return 0;
}

static int deadline(struct reader *r, uint64_t start, uint32_t timeout, int expired)
{
	uint64_t value;
	int rc = now(r, &value);

	if (rc != 0) {
		return rc;
	}
	return value - start > timeout ? expired : 0;
}

static int delay(struct reader *r, uint32_t us)
{
	uint64_t start, end;
	int rc = now(r, &start);

	if (rc != 0) {
		return rc;
	}
	if (r->io->delay_us(r->io->context, us)) {
		return MONACO_POLICY_IO;
	}
	rc = now(r, &end);
	if (rc != 0) {
		return rc;
	}
	return end - start < us ? MONACO_POLICY_TIMEBASE : 0;
}

static int presence_and_class(struct reader *r, const struct monaco_policy_limits *l, bool *normal)
{
	int rc;
	uint32_t value;
	uint64_t start;

	rc = now(r, &start);
	if (rc != 0) {
		return rc;
	}
	rc = read32(r, MD_STAGE, &value);
	if (rc != 0) {
		return rc;
	}
	if (value == 0xcb) {
		*normal = true;
		return 0;
	}
	rc = read32(r, CMD_B0, &value);
	if (rc != 0) {
		return rc;
	}
	if (!value) {
		for (;;) {
			rc = read32(r, MD_STAGE, &value);
			if (rc != 0) {
				return rc;
			}
			r->d->presence_checks++;
			if (value) {
				break;
			}
			rc = deadline(r, start, l->presence_timeout_us, MONACO_POLICY_MISSING_MD);
			if (rc != 0) {
				return rc;
			}
			if (r->d->presence_checks >= l->max_presence_checks) {
				return MONACO_POLICY_MISSING_MD;
			}
			rc = delay(r, 1000);
			if (rc != 0) {
				return rc;
			}
			rc = read32(r, CMD_B0, &value);
			if (rc != 0) {
				return rc;
			}
			if (value) {
				break;
			}
		}
	}
	r->d->stage = MONACO_STAGE_CLASSIFY;
	/* The FE/11/11 command prefix is checked before the stage range. */
	rc = read32(r, CMD_B0, &value);
	if (rc != 0) {
		return rc;
	}
	if (value == 0xfe) {
		rc = read32(r, CMD_AC, &value);
		if (rc != 0) {
			return rc;
		}
		if (value == 0x11) {
			rc = read32(r, CMD_A8, &value);
			if (rc != 0) {
				return rc;
			}
			if (value == 0x11) {
				*normal = true;
				return 0;
			}
		}
	}
	rc = read32(r, MD_STAGE, &value);
	if (rc != 0) {
		return rc;
	}
	if (value < 0xcb) {
		*normal = false;
		return 0;
	}
	/* Sample again; the second sample is the one used. */
	rc = read32(r, MD_STAGE, &value);
	if (rc != 0) {
		return rc;
	}
	*normal = value <= 0xd0;
	return 0;
}

static int normal_ready(struct reader *r, const struct monaco_policy_limits *l, bool normal,
			uint32_t *final)
{
	int rc;
	uint32_t value;
	uint64_t start;

	rc = now(r, &start);
	if (rc != 0) {
		return rc;
	}
	rc = read32(r, CMD_A4, &value);
	if (rc != 0) {
		return rc;
	}
	if (value != 0x11) {
		for (;;) {
			rc = read32(r, CMD_A4, &value);
			if (rc != 0) {
				return rc;
			}
			if (value == 0x13) {
				break;
			}
			rc = deadline(r, start, l->ready_timeout_us, MONACO_POLICY_READY_TIMEOUT);
			if (rc != 0) {
				return rc;
			}
			if (r->d->ready_waits >= l->max_ready_waits) {
				return MONACO_POLICY_READY_TIMEOUT;
			}
			r->d->ready_waits++;
			rc = delay(r, 1000000);
			if (rc != 0) {
				return rc;
			}
			rc = deadline(r, start, l->ready_timeout_us, MONACO_POLICY_READY_TIMEOUT);
			if (rc != 0) {
				return rc;
			}
			rc = read32(r, CMD_A4, &value);
			if (rc != 0) {
				return rc;
			}
			if (value == 0x11) {
				break;
			}
		}
	}
	rc = read32(r, CMD_A4, final);
	if (rc != 0) {
		return rc;
	}
	/*
	 * Only a final A4 value of 0x13 on a normal boot is accepted; an earlier
	 * 0x13 sample does not count.
	 */
	return normal && *final == 0x13 ? 0 : MONACO_POLICY_BOOT_STATE;
}

static int prepare(struct reader *r, const struct monaco_policy_limits *l,
		   struct monaco_policy_snapshot *s)
{
	int rc;
	bool normal;
	uint32_t core;

	if (r->io->read_mpidr(r->io->context, &s->mpidr)) {
		return MONACO_POLICY_IO;
	}
	core = (s->mpidr & 0xff) | ((s->mpidr & 0xff00) >> 7);
	/* Only core 0 of cluster 0 is supported; Aff2 must be zero. */
	if (core || (s->mpidr & UINT32_C(0x00ff0000))) {
		return MONACO_POLICY_CORE;
	}
	s->bist.core = (uint8_t)core;
	r->d->stage = MONACO_STAGE_PRESENCE;
	rc = presence_and_class(r, l, &normal);
	if (rc != 0) {
		return rc;
	}
	s->bist.md_presence_known = true;
	s->bist.md_present = true;
	r->d->stage = MONACO_STAGE_NORMAL_READY;
	rc = normal_ready(r, l, normal, &s->normal_ready_word);
	if (rc != 0) {
		return rc;
	}
	r->d->stage = MONACO_STAGE_FUSES;
	rc = read32(r, MODE_FUSE, &s->mode_word);
	if (rc != 0) {
		return rc;
	}
	rc = read32(r, SKIP_FUSE, &s->skip_word);
	if (rc != 0) {
		return rc;
	}
	s->bist.mode = (uint8_t)((s->mode_word >> 11) & 3);
	s->bist.skip = ((s->skip_word >> 8) & 1) != 0;
	s->prepared = true;
	r->d->stage = MONACO_STAGE_PREPARED;
	return 0;
}

int monaco_policy_prepare(const struct monaco_policy_io *io,
			  const struct monaco_policy_limits *limits,
			  struct monaco_policy_snapshot *snapshot,
			  struct monaco_policy_diagnostic *diagnostic)
{
	struct reader r = {.io = io, .d = diagnostic};
	/* Clear any earlier snapshot, even when the arguments are invalid. */
	if (snapshot) {
		*snapshot = (struct monaco_policy_snapshot){0};
	}
	if (!diagnostic || !snapshot) {
		return MONACO_POLICY_INVALID;
	}
	*diagnostic = (struct monaco_policy_diagnostic){.stage = MONACO_STAGE_CORE};
	if (!io || !io->read32 || !io->read_mpidr || !io->time_us || !io->delay_us || !limits ||
	    !limits->presence_timeout_us || !limits->ready_timeout_us ||
	    !limits->max_presence_checks || limits->max_presence_checks > 101 ||
	    !limits->max_ready_waits) {
		diagnostic->result = MONACO_POLICY_INVALID;
	} else {
		diagnostic->result = prepare(&r, limits, snapshot);
	}
	return diagnostic->result;
}

static int finish(struct reader *r, struct monaco_policy_snapshot *s)
{
	int rc;
	static const uint32_t address[] = {CMD_B0, CMD_AC, CMD_A8, CMD_A4};
	uint32_t word = 0;
	unsigned int sampled = 0;

	s->consumed = true;
	r->d->stage = MONACO_STAGE_WARM;
	rc = read32(r, WARM_COOKIE, &s->warm_cookie);
	if (rc != 0) {
		return rc;
	}
	s->warm_cookie_valid = true;
	s->bist.warm_cookie = s->warm_cookie;
	s->bist.warm_cookie_valid = true;
	if (s->warm_cookie == 0xf) {
		return MONACO_POLICY_WARM;
	}
	r->d->stage = MONACO_STAGE_COMMAND;
	/* Stop at the first mismatch; compare full 32-bit values. */
	for (unsigned int n = 0; n < 4; n++) {
		rc = read32(r, address[n], &s->command_parts[n]);
		if (rc != 0) {
			return rc;
		}
		s->command_parts_valid |= (uint8_t)(1U << n);
		sampled++;
		if (s->command_parts[n] != (n ? 0x11U : 0xfeU)) {
			break;
		}
	}
	if (sampled == 4 && s->command_parts[3] == 0x11) {
		s->bist.md_boot_word = UINT32_C(0xfe111111);
		return MONACO_POLICY_EDL;
	}
	/*
	 * Fuses were sampled by prepare, before clock bring-up. Checks run in the
	 * order warm, EDL, skip/mode; skip takes precedence over mode 2.
	 */
	if (s->bist.skip || s->bist.mode == 0 || s->bist.mode == 3) {
		return MONACO_POLICY_SKIP;
	}
	if (s->bist.mode == 2) {
		return MONACO_POLICY_MODE2;
	}
	/*
	 * Read the remaining command bytes only when the BIST will run, never on
	 * the warm, EDL or skip paths.
	 */
	for (unsigned int n = sampled; n < 4; n++) {
		rc = read32(r, address[n], &s->command_parts[n]);
		if (rc != 0) {
			return rc;
		}
		s->command_parts_valid |= (uint8_t)(1U << n);
	}
	for (unsigned int n = 0; n < 4; n++) {
		if (s->command_parts[n] > 0xff) {
			return MONACO_POLICY_BOOT_STATE;
		}
		word = (word << 8) | s->command_parts[n];
	}
	s->bist.md_boot_word = word;
	if (s->command_parts[3] != 0x13) {
		return MONACO_POLICY_BOOT_STATE;
	}
	s->bist.snapshot_valid = true;
	r->d->stage = MONACO_STAGE_COMPLETE;
	return 0;
}

int monaco_policy_finish(const struct monaco_policy_io *io, struct monaco_policy_snapshot *snapshot,
			 struct monaco_policy_diagnostic *diagnostic)
{
	struct reader r = {.io = io, .d = diagnostic};

	if (!diagnostic) {
		return MONACO_POLICY_INVALID;
	}
	*diagnostic = (struct monaco_policy_diagnostic){.stage = MONACO_STAGE_WARM};
	if (!io || !io->read32 || !snapshot || !snapshot->prepared || snapshot->consumed) {
		diagnostic->result = MONACO_POLICY_INVALID;
	} else {
		diagnostic->result = finish(&r, snapshot);
	}
	return diagnostic->result;
}
