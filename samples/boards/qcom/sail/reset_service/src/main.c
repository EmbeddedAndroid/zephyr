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

static void serve(void)
{
	int rc = sail_reset_service_start(retire_resources, NULL);

	while (rc >= 0) {
		rc = sail_reset_service_poll();
		k_yield();
	}
	/* The service is not restarted once it has completed or failed. */
	for (;;) {
		k_yield();
	}
}

#if defined(CONFIG_SMP)
/* The coordinator needs a worker that stays on CPU 0. */
static void worker(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	serve();
}

K_THREAD_STACK_DEFINE(worker_stack, 4096);
static struct k_thread worker_thread;
#endif

int main(void)
{
	if (sail_boot_mode_get() != SAIL_BOOT_PCINIT_DEFERRED) {
		return sail_boot_status();
	}
#if defined(CONFIG_SMP)
	k_thread_create(&worker_thread, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack), worker,
			NULL, NULL, NULL, CONFIG_MAIN_THREAD_PRIORITY, 0, K_FOREVER);
	k_thread_cpu_pin(&worker_thread, 0);
	k_thread_start(&worker_thread);
	return 0;
#else
	serve();
#endif
}
