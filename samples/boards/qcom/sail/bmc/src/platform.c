/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Platform thread: runs the main-domain reset coordinator, hands every notice
 * it consumes to the MD monitor, and publishes the image word in the answer
 * registers for the MD to read.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <soc.h>
#include "bmc.h"

#define POLL_MS 2
#define XBL_BOOT_SUCCESS 0xcb000003U
#define SDI_NOTICE       0xcd0a0000U
/* The MD polls for an answer for up to 1 s; keep the image word off it far longer. */
#define ANSWER_HOLD_MS 12000

static atomic_t coordinator_rc = ATOMIC_INIT(-EINPROGRESS);

int bmc_platform_status(void)
{
	return (int)atomic_get(&coordinator_rc);
}

static int retire(void *context)
{
	ARG_UNUSED(context);
	bmc_md_retire();
	return 0;
}

static void platform_thread(void *p1, void *p2, void *p3)
{
	struct sail_reset_service_notice notice;
	uint64_t seen = 0U;
	int64_t publish_at = -1;
	uint32_t published = 0U;
	int rc;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	rc = sail_reset_service_start(retire, NULL);
	atomic_set(&coordinator_rc, rc);
	while (rc >= 0) {
		bmc_cpu_mark(BMC_THREAD_PLATFORM);
		rc = sail_reset_service_poll();
		if (rc != 0 && sail_reset_service_notice_get(&notice) == 0 &&
		    notice.sequence != seen) {
			seen = notice.sequence;
			bmc_md_notice(notice.word, notice.channel);
			if (notice.word == XBL_BOOT_SUCCESS) {
				publish_at = k_uptime_get();
			} else if (notice.word == SDI_NOTICE) {
				publish_at = k_uptime_get() + ANSWER_HOLD_MS;
			}
		}
		if (rc < 0) {
			break;
		}
		if (publish_at >= 0 && k_uptime_get() >= publish_at) {
			uint32_t word = bmc_image_word();

			if (word != 0U && word != published &&
			    sail_reset_service_publish(word) == 0) {
				published = word;
			}
		}
		if (rc == 0) {
			k_msleep(POLL_MS);
		}
	}
	atomic_set(&coordinator_rc, rc);
	/* -ESHUTDOWN: the reset or shutdown was answered and the coordinator completed. */
	bmc_out("BMC platform t=%llu coordinator %s rc=%d\n", bmc_uptime_ms(),
		rc == -ESHUTDOWN ? "complete" : "stopped", rc);
}

K_THREAD_STACK_DEFINE(platform_stack, 4096);

int bmc_platform_start(void)
{
	static struct k_thread thread;

	if (sail_boot_mode_get() != SAIL_BOOT_PCINIT_DEFERRED) {
		return -EPERM;
	}
	k_thread_create(&thread, platform_stack, K_THREAD_STACK_SIZEOF(platform_stack),
			platform_thread, NULL, NULL, NULL, K_PRIO_COOP(1), 0, K_FOREVER);
	k_thread_name_set(&thread, "platform");
	bmc_cpu_place(BMC_THREAD_PLATFORM, &thread);
	k_thread_start(&thread);
	return 0;
}
