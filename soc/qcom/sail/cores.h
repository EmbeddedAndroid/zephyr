/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_CORES_H_
#define ZEPHYR_SOC_QCOM_SAIL_CORES_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Four Cortex-R52 cores in two clusters of two. MPIDR is cluster << 8 | cpu.
 * Each cluster has its own GIC, so a Zephyr image spans one cluster.
 */
#define SAIL_CORE_CLUSTERS 2U
#define SAIL_CORE_CPUS     2U

struct sail_cores_io {
	void *context;
	/* Ordered Device accesses; callbacks must themselves terminate. */
	int (*read32)(void *context, uint32_t address, uint32_t *value);
	int (*write32)(void *context, uint32_t address, uint32_t value);
};

/*
 * Read the boot configuration fuses: bit c of @p mask is set when cluster c
 * runs as a lockstep pair, so its second core is the redundant checker and
 * cannot be started on its own.
 */
int sail_cores_lockstep(const struct sail_cores_io *io, uint32_t *mask);

/*
 * Take one secondary core out of reset at @p entry (32-byte aligned). The first
 * core of cluster 1 also brings the cluster out of reset and gives it MPIDR
 * affinity 1. Core 0 of cluster 0 runs this code and is never touched; neither
 * are the lockstep comparators.
 *
 * @retval -EINVAL Bad core or entry.
 * @retval -ENODEV The core is the checker of a lockstep pair.
 * @retval -EALREADY The core is already out of reset; nothing was written.
 * @retval -EAGAIN Cluster 1 is still in reset; start its first core first.
 * @retval -ETIMEDOUT The counter used for the settle delays did not advance.
 * @retval -EIO An access failed or a readback did not match.
 */
int sail_core_release(const struct sail_cores_io *io, uint32_t cluster, uint32_t cpu,
		      uint32_t entry);

/* True for the registers sail_core_release() may write. */
bool sail_cores_write_allowed(uint32_t address);

int sail_cores_native_read32(void *context, uint32_t address, uint32_t *value);
int sail_cores_native_write32(void *context, uint32_t address, uint32_t value);

#endif
