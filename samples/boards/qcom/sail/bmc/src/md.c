/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Main-domain (MD) monitor: records the MD notices the platform thread hands
 * over, tracks a heartbeat word that software on the MD advances, and in
 * watchdog mode reports (and optionally resets) an MD whose heartbeat stops.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/arch/cpu.h>
#include <soc.h>
#include "md_protocol.h"
#include "bmc.h"

#define MD_POLL_MS 10
#define HB_POLL_MS 100
/* MD physical addresses are reached from SAIL at MD + 0xc0000000. */
#define MD_ALIAS   0xc0000000U
#define SAIL_LOCAL 0xf0000000U

/*
 * Blocks SAIL has read on Lemans (IQ-9075 EVK) without a fault. Others,
 * including the main-domain TLMM, APSS watchdog and MPM, and parts of
 * SAILSS_PWR_CTRL, take a synchronous external abort, which halts SAIL. No
 * block is listed for other chips.
 */
#define CHIP_LEMANS 0x6019U

static const struct {
	uint32_t start, end;
} readable[] = {
	{0xcc251000U, 0xcc253000U}, /* MD TSENS2/3 TM */
	{0xcc263000U, 0xcc264000U}, /* MD TSENS0 TM */
	{0xcc265000U, 0xcc266000U}, /* MD TSENS1 TM */
	{0xf0100000U, 0xf0144000U}, /* SAIL TLMM pad configuration */
	{0xf0305000U, 0xf0306000U}, /* SAIL/MD mailbox status words */
	{0xf1000000U, 0xf100003cU}, /* SAILSS_PWR_CTRL, first 15 words */
	{0xf1110000U, 0xf1120000U}, /* SAILSS_CC */
	{0xf1988000U, 0xf198c000U}, /* QUP SE2 (console) */
};

bool bmc_readable(uint32_t sail_addr)
{
	if (sail_chip_id_get() != CHIP_LEMANS) {
		return false;
	}
	for (size_t i = 0; i < ARRAY_SIZE(readable); i++) {
		if (sail_addr >= readable[i].start && sail_addr < readable[i].end) {
			return true;
		}
	}
	return false;
}

struct notice_msg {
	uint64_t at_ms;
	uint32_t word;
	uint32_t channel;
};

K_MSGQ_DEFINE(notice_queue, sizeof(struct notice_msg), BMC_NOTICE_HISTORY, 8);
static atomic_t dropped;

/* MD reads stop for good once a reset or shutdown is claimed: the MD may be gone. */
static struct k_spinlock io_lock;
static bool md_gone;

int bmc_read32(uint32_t sail_addr, uint32_t *value)
{
	k_spinlock_key_t key;
	int rc = 0;

	if (sail_addr >= SAIL_LOCAL) {
		*value = sys_read32(sail_addr);
		return 0;
	}
	key = k_spin_lock(&io_lock);
	if (md_gone) {
		rc = -ESHUTDOWN;
	} else {
		*value = sys_read32(sail_addr);
	}
	k_spin_unlock(&io_lock, key);
	return rc;
}

/* Called by the coordinator with its lock held: must not block. */
void bmc_md_retire(void)
{
	k_spinlock_key_t key = k_spin_lock(&io_lock);

	md_gone = true;
	k_spin_unlock(&io_lock, key);
}

void bmc_md_notice(uint32_t word, uint32_t channel)
{
	struct notice_msg m = {.at_ms = bmc_uptime_ms(), .word = word, .channel = channel};

	if (k_msgq_put(&notice_queue, &m, K_NO_WAIT) != 0) {
		atomic_inc(&dropped);
	}
}

static struct k_spinlock lock;
static struct bmc_md_status st;
static struct bmc_notice history[BMC_NOTICE_HISTORY];
static uint32_t history_next;

const char *bmc_md_state_name(enum bmc_md_state state)
{
	switch (state) {
	case BMC_MD_BOOTING:
		return "booting";
	case BMC_MD_RUNNING:
		return "running";
	case BMC_MD_HUNG:
		return "hung";
	case BMC_MD_RESETTING:
		return "resetting";
	case BMC_MD_SHUTDOWN:
		return "shutdown";
	default:
		return "unknown";
	}
}

static enum bmc_md_state state_after(enum bmc_md_state cur, enum sail_md_command cmd)
{
	switch (cmd) {
	case SAIL_MD_COMMAND_DDR_READY:
	case SAIL_MD_COMMAND_CC020000:
	case SAIL_MD_COMMAND_CC030000:
	case SAIL_MD_COMMAND_CC040000:
	case SAIL_MD_COMMAND_CC050000:
	case SAIL_MD_COMMAND_XBL_BOOT_SUCCESS:
	case SAIL_MD_COMMAND_SECURE_BOOT_NOTICE:
	case SAIL_MD_COMMAND_HYP_BOOT_NOTICE:
		return BMC_MD_BOOTING;
	case SAIL_MD_COMMAND_SOC_HR:
	case SAIL_MD_COMMAND_WARM_RESET:
		return BMC_MD_RESETTING;
	case SAIL_MD_COMMAND_GRACEFUL_SHUTDOWN:
		return BMC_MD_SHUTDOWN;
	default:
		return cur;
	}
}

static void record_notice(const struct notice_msg *ev)
{
	uint32_t word = ev->word;
	uint64_t now = ev->at_ms;
	enum sail_md_command cmd = sail_md_command_decode(word);
	k_spinlock_key_t key = k_spin_lock(&lock);

	history[history_next % BMC_NOTICE_HISTORY] = (struct bmc_notice){
		.at_ms = now, .word = word, .channel = (uint8_t)ev->channel};
	history_next++;
	st.notices++;
	st.last_word = word;
	st.last_notice_ms = now;
	st.state = state_after(st.state, cmd);
	if (st.state == BMC_MD_RESETTING || st.state == BMC_MD_SHUTDOWN) {
		/* An orderly reset or shutdown is not a hang. */
		st.wdt_armed = false;
	}
	k_spin_unlock(&lock, key);
	bmc_out("BMC notice t=%llu ch=%u word=0x%08x\n", now, ev->channel, word);
}

static void heartbeat_poll(uint64_t now)
{
	bool fire = false, do_reset = false;
	uint32_t timeout_s = 0U, age_ms = 0U;
	k_spinlock_key_t key = k_spin_lock(&lock);

	uint32_t v;

	if (st.hb_configured && bmc_read32(st.hb_addr + MD_ALIAS, &v) == 0) {
		if (v != st.hb_value) {
			st.hb_value = v;
			st.hb_changed_ms = now;
			st.hb_changes++;
			if (st.hb_changes > 1U) {
				st.state = BMC_MD_RUNNING;
				st.wdt_armed = st.wdt_enabled;
			}
		} else if (st.wdt_enabled && st.wdt_armed &&
			   now - st.hb_changed_ms >= (uint64_t)st.wdt_timeout_s * 1000U) {
			st.wdt_armed = false;
			st.wdt_fired++;
			st.state = BMC_MD_HUNG;
			fire = true;
			do_reset = st.wdt_reset;
			timeout_s = st.wdt_timeout_s;
			age_ms = (uint32_t)(now - st.hb_changed_ms);
		}
	}
	k_spin_unlock(&lock, key);

	if (fire) {
		bmc_out("BMC MDWDT t=%llu heartbeat lost age_ms=%u timeout_s=%u action=%s\n", now,
		       age_ms, timeout_s, do_reset ? "reset" : "log");
		if (do_reset) {
			int rc = bmc_md_reset("auto");

			bmc_out("BMC MDWDT reset rc=%d\n", rc);
		}
	}
}

static void md_thread(void *a, void *b, void *c)
{
	struct notice_msg ev;
	uint32_t ticks = 0U;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		bmc_cpu_mark(BMC_THREAD_MD);
		while (k_msgq_get(&notice_queue, &ev, K_NO_WAIT) == 0) {
			record_notice(&ev);
		}
		if (++ticks >= HB_POLL_MS / MD_POLL_MS) {
			ticks = 0U;
			heartbeat_poll(bmc_uptime_ms());
		}
		k_msleep(MD_POLL_MS);
	}
}

K_THREAD_STACK_DEFINE(md_stack, 2048);
static struct k_thread md_tid;

void bmc_md_start(void)
{
	k_thread_create(&md_tid, md_stack, K_THREAD_STACK_SIZEOF(md_stack), md_thread, NULL,
			NULL, NULL, K_PRIO_PREEMPT(2), 0, K_FOREVER);
	k_thread_name_set(&md_tid, "md");
	bmc_cpu_place(BMC_THREAD_MD, &md_tid);
	k_thread_start(&md_tid);
}

void bmc_md_status_get(struct bmc_md_status *out)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	*out = st;
	k_spin_unlock(&lock, key);
	out->dropped = (uint32_t)atomic_get(&dropped);
	key = k_spin_lock(&io_lock);
	out->retired = md_gone;
	k_spin_unlock(&io_lock, key);
}

size_t bmc_md_history(struct bmc_notice *out, size_t max)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t count = MIN(history_next, BMC_NOTICE_HISTORY);
	uint32_t first = history_next - count;
	size_t n = 0;

	for (uint32_t i = 0U; i < count && n < max; i++) {
		out[n++] = history[(first + i) % BMC_NOTICE_HISTORY];
	}
	k_spin_unlock(&lock, key);
	return n;
}

void bmc_md_heartbeat_set(uint32_t addr)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	st.hb_configured = addr != 0U;
	st.hb_addr = addr;
	st.hb_changes = 0U;
	st.hb_changed_ms = bmc_uptime_ms();
	st.wdt_armed = false;
	if (st.hb_configured && bmc_read32(addr + MD_ALIAS, &st.hb_value) != 0) {
		st.hb_value = 0U;
	}
	k_spin_unlock(&lock, key);
}

void bmc_md_wdt_set(bool enabled, uint32_t timeout_s, bool reset)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	st.wdt_enabled = enabled;
	st.wdt_timeout_s = timeout_s;
	st.wdt_reset = reset;
	/* Armed by the next heartbeat change, never by a stale value. */
	st.wdt_armed = false;
	k_spin_unlock(&lock, key);
}

/*
 * No reset path is available to SAIL on the IQ-9075 EVK: it has no SAIL PMIC,
 * and the main domain's MPM PS_HOLD register gives SAIL an external abort.
 */
int bmc_md_reset(const char *how)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	st.resets_requested++;
	st.last_reset_result = -ENOTSUP;
	k_spin_unlock(&lock, key);
	bmc_out("BMC md reset t=%llu reason=%s: no main-domain reset path on this board\n",
		bmc_uptime_ms(), how);
	return -ENOTSUP;
}
