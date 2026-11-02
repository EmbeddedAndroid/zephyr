/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <zephyr/drivers/pm_cpu_ops.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel/smp.h>
#include <zephyr/sys/util.h>
#include <soc.h>
#include "cores.h"

/* The image entry sets up EL2 for each core before it reaches the kernel. */
extern void sail_reset(void);

#define IMAGE_CLUSTER COND_CODE_1(CONFIG_SAIL_SECONDARY_CLUSTER, (1U), (0U))

static const struct sail_cores_io io = {
	.read32 = sail_cores_native_read32,
	.write32 = sail_cores_native_write32,
};

static int status[SAIL_CORE_CLUSTERS][SAIL_CORE_CPUS] = {
	{-EINPROGRESS, -EINPROGRESS},
	{-EINPROGRESS, -EINPROGRESS},
};

int pm_cpu_on(unsigned long mpidr, uintptr_t entry_point)
{
	ARG_UNUSED(entry_point);

	if (((mpidr >> 8) & 0xffU) != IMAGE_CLUSTER || (mpidr & ~0xffffUL) != 0U) {
		return -EINVAL;
	}
	return sail_core_release(&io, IMAGE_CLUSTER, mpidr & 0xffU,
				 (uint32_t)(uintptr_t)sail_reset);
}

#if CONFIG_SAIL_SECONDARY_CLUSTER_ENTRY != 0
/* Start cluster 1 only at a loaded image: eight branch instructions at the entry. */
static int start_secondary_cluster(void)
{
	const volatile uint32_t *vector =
		(const volatile uint32_t *)(uintptr_t)CONFIG_SAIL_SECONDARY_CLUSTER_ENTRY;

	if (vector[0] != UINT32_C(0xea000006)) {
		return -ENOENT;
	}
	for (size_t i = 1; i < 8; i++) {
		if ((vector[i] & UINT32_C(0xff000000)) != UINT32_C(0xea000000)) {
			return -ENOENT;
		}
	}
	return sail_core_release(&io, 1U, 0U, CONFIG_SAIL_SECONDARY_CLUSTER_ENTRY);
}
#endif

static int sail_cores_start(void)
{
	uint32_t lockstep;
	int rc = sail_cores_lockstep(&io, &lockstep);

	status[IMAGE_CLUSTER][0] = 0;
#if CONFIG_SAIL_SECONDARY_CLUSTER_ENTRY != 0
	status[1][0] = start_secondary_cluster();
#endif
	if (rc != 0) {
		status[IMAGE_CLUSTER][1] = rc;
	} else if ((lockstep & BIT(IMAGE_CLUSTER)) != 0U) {
		status[IMAGE_CLUSTER][1] = -ENODEV;
	} else {
		/* The second core is deferred in devicetree; pm_cpu_on() releases it. */
		k_smp_cpu_start(1, NULL, NULL);
		status[IMAGE_CLUSTER][1] = 0;
	}
	return 0;
}

SYS_INIT(sail_cores_start, SMP, 0);

int sail_core_status(uint32_t cluster, uint32_t cpu)
{
	if (cluster >= SAIL_CORE_CLUSTERS || cpu >= SAIL_CORE_CPUS) {
		return -EINVAL;
	}
	return status[cluster][cpu];
}
