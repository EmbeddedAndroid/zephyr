/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cores.h"

#include <errno.h>
#include <stddef.h>
#include <zephyr/sys/util.h>

#define BIT32(n)             (UINT32_C(1) << (n))
#define CLUSTER_CONFIG(c)    (UINT32_C(0xf0300040) + UINT32_C(0xc) * (c))
#define CPU_CONFIG(c, n)     (CLUSTER_CONFIG(c) + 4U + 4U * (n))
#define BOOT_FUSES           UINT32_C(0xf8706100)
#define LOCKSTEP_SHIFT       13U
#define CLUSTER1_CLOCK       UINT32_C(0xf1124020)
#define RESETS(c)            (UINT32_C(0xf1134430) + UINT32_C(0x20) * (c))
#define TOP_RESET(c)         (RESETS(c))
#define DEBUG_RESET(c)       (RESETS(c) + 4U)
#define POWER_ON_RESET(c, n) (RESETS(c) + 8U + 4U * (n))
#define CORE_RESET(c, n)     (RESETS(c) + 16U + 4U * (n))
#define COUNTER_LOW          UINT32_C(0xf1400000)

#define RESET_ASSERTED   BIT32(0)
#define CLOCK_ENABLE     BIT32(0)
#define AFFINITY_FIELDS  (UINT32_C(0xff) << 11 | UINT32_C(0xff) << 3)
#define AFFINITY_1       (UINT32_C(1) << 11)
#define INIT_REGISTERS   BIT32(2)
#define RAM_PROTECTION   BIT32(1)
#define VECTOR_SHIFT     5U
#define SETTLE_US        100U
/* 19.2 MHz counter; enough reads for several milliseconds at bus speed. */
#define TICKS_PER_5US    96U
#define COUNTER_READS    200000U


static int read(const struct sail_cores_io *io, uint32_t address, uint32_t *value)
{
	return io->read32(io->context, address, value) == 0 ? 0 : -EIO;
}

static int write(const struct sail_cores_io *io, uint32_t address, uint32_t value)
{
	return io->write32(io->context, address, value) == 0 ? 0 : -EIO;
}

static int deassert(const struct sail_cores_io *io, uint32_t address)
{
	uint32_t value;
	int rc = read(io, address, &value);

	return rc != 0 ? rc : write(io, address, value & ~RESET_ASSERTED);
}

static int settle(const struct sail_cores_io *io)
{
	uint32_t start, now;
	int rc = read(io, COUNTER_LOW, &start);

	for (uint32_t i = 0; rc == 0 && i < COUNTER_READS; i++) {
		rc = read(io, COUNTER_LOW, &now);
		if (rc == 0 && now - start >= SETTLE_US * TICKS_PER_5US / 5U) {
			return 0;
		}
	}
	return rc != 0 ? rc : -ETIMEDOUT;
}

int sail_cores_lockstep(const struct sail_cores_io *io, uint32_t *mask)
{
	uint32_t fuses;
	int rc;

	if (io == NULL || io->read32 == NULL || mask == NULL) {
		return -EINVAL;
	}
	rc = read(io, BOOT_FUSES, &fuses);
	if (rc == 0) {
		*mask = (fuses >> LOCKSTEP_SHIFT) & 3U;
	}
	return rc;
}

static int start_cluster1(const struct sail_cores_io *io)
{
	uint32_t value;
	int rc = settle(io);

	if (rc == 0) {
		rc = read(io, CLUSTER1_CLOCK, &value);
	}
	if (rc == 0) {
		rc = write(io, CLUSTER1_CLOCK, value | CLOCK_ENABLE);
	}
	if (rc == 0) {
		rc = deassert(io, TOP_RESET(1));
	}
	if (rc == 0) {
		rc = deassert(io, DEBUG_RESET(1));
	}
	return rc == 0 ? settle(io) : rc;
}

int sail_core_release(const struct sail_cores_io *io, uint32_t cluster, uint32_t cpu,
		      uint32_t entry)
{
	uint32_t lockstep, value;
	int rc;

	if (io == NULL || io->read32 == NULL || io->write32 == NULL ||
	    cluster >= SAIL_CORE_CLUSTERS || cpu >= SAIL_CORE_CPUS || (cluster | cpu) == 0U ||
	    entry == 0U || (entry & (BIT32(VECTOR_SHIFT) - 1U)) != 0U) {
		return -EINVAL;
	}
	rc = sail_cores_lockstep(io, &lockstep);
	if (rc != 0) {
		return rc;
	}
	if (cpu == 1U && (lockstep & BIT32(cluster)) != 0U) {
		return -ENODEV;
	}
	rc = read(io, CORE_RESET(cluster, cpu), &value);
	if (rc != 0) {
		return rc;
	}
	if ((value & RESET_ASSERTED) == 0U) {
		return -EALREADY;
	}
	if (cluster == 1U && cpu == 1U) {
		rc = read(io, TOP_RESET(1), &value);
		if (rc != 0) {
			return rc;
		}
		if ((value & RESET_ASSERTED) != 0U) {
			return -EAGAIN;
		}
	}
	if (cluster == 1U && cpu == 0U) {
		rc = read(io, CLUSTER_CONFIG(1), &value);
		if (rc == 0) {
			value = (value & ~AFFINITY_FIELDS) | AFFINITY_1 | INIT_REGISTERS |
				RAM_PROTECTION;
			rc = write(io, CLUSTER_CONFIG(1), value);
		}
		if (rc != 0) {
			return rc;
		}
	}
	/* Halt, endianness, Thumb exceptions, cache invalidation and TCM boot all clear. */
	rc = write(io, CPU_CONFIG(cluster, cpu), entry >> VECTOR_SHIFT);
	if (rc == 0) {
		rc = read(io, CPU_CONFIG(cluster, cpu), &value);
	}
	if (rc == 0 && value != entry >> VECTOR_SHIFT) {
		rc = -EIO;
	}
	if (rc == 0 && cluster == 1U) {
		rc = cpu == 0U ? start_cluster1(io) : settle(io);
	}
	if (rc == 0) {
		rc = deassert(io, POWER_ON_RESET(cluster, cpu));
	}
	return rc == 0 ? deassert(io, CORE_RESET(cluster, cpu)) : rc;
}

bool sail_cores_write_allowed(uint32_t address)
{
	static const uint32_t allowed[] = {
		CLUSTER_CONFIG(1),
		CPU_CONFIG(0, 1),
		CPU_CONFIG(1, 0),
		CPU_CONFIG(1, 1),
		CLUSTER1_CLOCK,
		TOP_RESET(1),
		DEBUG_RESET(1),
		POWER_ON_RESET(0, 1),
		POWER_ON_RESET(1, 0),
		POWER_ON_RESET(1, 1),
		CORE_RESET(0, 1),
		CORE_RESET(1, 0),
		CORE_RESET(1, 1),
	};

	for (size_t i = 0; i < ARRAY_SIZE(allowed); i++) {
		if (address == allowed[i]) {
			return true;
		}
	}
	return false;
}
