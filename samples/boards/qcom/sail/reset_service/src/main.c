/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <soc.h>

static int retire_resources(void *context)
{
	ARG_UNUSED(context);

	/* This sample keeps no resources that a reset would invalidate. */
	return 0;
}

int main(void)
{
	int rc;

	if (sail_boot_mode_get() != SAIL_BOOT_PCINIT_DEFERRED) {
		return sail_boot_status();
	}

	rc = sail_reset_service_start(retire_resources, NULL);

	while (rc >= 0) {
		rc = sail_reset_service_poll();
		k_yield();
	}
	/* The service is not restarted once it has completed or failed. */
	for (;;) {
		k_yield();
	}
}
