/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cores.h"
#include "sail_check.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#define FUSES          UINT32_C(0xf8706100)
#define C1_CONFIG      UINT32_C(0xf030004c)
#define CPU_CONFIG(i)  (UINT32_C(0xf0300044) + 4U * (i) + ((i) >= 2U ? 4U : 0U))
#define C1_CLOCK       UINT32_C(0xf1124020)
#define RST(c, o)      (UINT32_C(0xf1134430) + UINT32_C(0x20) * (c) + (o))
#define TOP            0U
#define DBG            4U
#define POR(n)         (8U + 4U * (n))
#define CORE(n)        (16U + 4U * (n))
#define COUNTER        UINT32_C(0xf1400000)
#define ENTRY          UINT32_C(0x0801d400)
#define C1_ENTRY       UINT32_C(0x080c0000)
#define SETTLE_TICKS   1920U

static const uint32_t addresses[] = {
	FUSES, C1_CONFIG, CPU_CONFIG(0), CPU_CONFIG(1), CPU_CONFIG(2), CPU_CONFIG(3),
	UINT32_C(0xf0300040), C1_CLOCK, COUNTER,
	RST(0, TOP), RST(0, DBG), RST(0, POR(0)), RST(0, POR(1)), RST(0, CORE(0)), RST(0, CORE(1)),
	RST(1, TOP), RST(1, DBG), RST(1, POR(0)), RST(1, POR(1)), RST(1, CORE(0)), RST(1, CORE(1)),
};

struct fake {
	uint32_t value[ARRAY_SIZE(addresses)];
	/* 1-based index of the access that fails; 0 for none. */
	unsigned int fail_at, accesses, logged, writes;
	uint32_t tick, stuck_vector;
	struct {
		char kind;
		uint32_t address, value, time;
	} log[64];
};

static uint32_t *cell(struct fake *f, uint32_t address)
{
	for (size_t i = 0; i < ARRAY_SIZE(addresses); i++) {
		if (addresses[i] == address) {
			return &f->value[i];
		}
	}
	return NULL;
}

static uint32_t *reg(struct fake *f, uint32_t address)
{
	uint32_t *p = cell(f, address);

	sail_check(p != NULL);
	return p;
}

static int access(struct fake *f, char kind, uint32_t address, uint32_t value)
{
	unsigned int n = f->accesses++;

	/* Counter polls are counted for fault injection but not logged. */
	if (address != COUNTER) {
		unsigned int l = f->logged++;

		sail_check(l < ARRAY_SIZE(f->log));
		f->log[l].kind = kind;
		f->log[l].address = address;
		f->log[l].value = value;
		f->log[l].time = *reg(f, COUNTER);
	}
	return f->fail_at == n + 1U || cell(f, address) == NULL ? -1 : 0;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;
	int rc = access(f, 'R', address, 0);

	if (rc == 0) {
		*value = *cell(f, address);
		if (address == COUNTER) {
			*cell(f, address) += f->tick;
		}
	}
	return rc;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;
	int rc = access(f, 'W', address, value);

	sail_check(sail_cores_write_allowed(address));
	f->writes++;
	if (rc == 0) {
		*cell(f, address) = value & ~(address == CPU_CONFIG(1) ? f->stuck_vector : 0U);
	}
	return rc;
}

static void reset(struct fake *f, uint32_t fuses)
{
	memset(f, 0, sizeof(*f));
	*reg(f, FUSES) = fuses;
	*reg(f, C1_CONFIG) = UINT32_C(0x806);
	*reg(f, UINT32_C(0xf0300040)) = UINT32_C(0x6);
	*reg(f, CPU_CONFIG(1)) = UINT32_C(0xffffffff);
	*reg(f, C1_CLOCK) = UINT32_C(0x80006220);
	*reg(f, RST(0, POR(1))) = 1U;
	*reg(f, RST(0, CORE(1))) = 1U;
	for (uint32_t o = 0; o <= 20U; o += 4U) {
		*reg(f, RST(1, o)) = 1U;
	}
	f->tick = 7U;
}

static const struct sail_cores_io io_for(struct fake *f)
{
	return (struct sail_cores_io){f, read32, write32};
}

/* Log index of the first write to address, or -1. */
static int write_at(const struct fake *f, uint32_t address)
{
	for (unsigned int i = 0; i < f->logged; i++) {
		if (f->log[i].kind == 'W' && f->log[i].address == address) {
			return (int)i;
		}
	}
	return -1;
}

static void check_order(const struct fake *f, const uint32_t *seq, size_t n)
{
	int prev = -1;

	for (size_t i = 0; i < n; i++) {
		int at = write_at(f, seq[i]);

		sail_check(at > prev);
		prev = at;
	}
}

static void lockstep(void)
{
	static const uint32_t fuses[] = {0U, BIT(13), BIT(14), BIT(13) | BIT(14), ~0U};
	static const uint32_t masks[] = {0U, 1U, 2U, 3U, 3U};
	struct fake f;

	for (size_t i = 0; i < ARRAY_SIZE(fuses); i++) {
		struct sail_cores_io io;
		uint32_t mask = 99U;

		reset(&f, fuses[i]);
		io = io_for(&f);
		sail_check(sail_cores_lockstep(&io, &mask) == 0 && mask == masks[i]);
		sail_check(f.writes == 0U);
	}
	reset(&f, 0U);
	f.fail_at = 1U;
	{
		struct sail_cores_io io = io_for(&f);
		uint32_t mask = 99U;

		sail_check(sail_cores_lockstep(&io, &mask) == -EIO && mask == 99U);
		sail_check(sail_cores_lockstep(NULL, &mask) == -EINVAL);
		sail_check(sail_cores_lockstep(&io, NULL) == -EINVAL);
	}
}

static void cluster0_cpu1(void)
{
	static const uint32_t order[] = {CPU_CONFIG(1), RST(0, POR(1)), RST(0, CORE(1))};
	struct fake f;
	struct sail_cores_io io;

	reset(&f, 0U);
	io = io_for(&f);
	sail_check(sail_core_release(&io, 0U, 1U, ENTRY) == 0);
	sail_check(*reg(&f, CPU_CONFIG(1)) == ENTRY >> 5);
	sail_check(*reg(&f, RST(0, POR(1))) == 0U && *reg(&f, RST(0, CORE(1))) == 0U);
	sail_check(f.writes == 3U);
	check_order(&f, order, ARRAY_SIZE(order));
	/* Cluster 1 and the running core are untouched. */
	sail_check(*reg(&f, RST(1, TOP)) == 1U && *reg(&f, C1_CLOCK) == UINT32_C(0x80006220));
	sail_check(*reg(&f, CPU_CONFIG(0)) == 0U);
	/* A running core is not reprogrammed. */
	f.writes = 0U;
	sail_check(sail_core_release(&io, 0U, 1U, ENTRY) == -EALREADY && f.writes == 0U);
	/* The checker of a lockstep pair is never started. */
	reset(&f, BIT(13));
	io = io_for(&f);
	sail_check(sail_core_release(&io, 0U, 1U, ENTRY) == -ENODEV && f.writes == 0U);
	/* A vector that does not read back leaves the core in reset. */
	reset(&f, 0U);
	f.stuck_vector = BIT(5);
	io = io_for(&f);
	sail_check(sail_core_release(&io, 0U, 1U, ENTRY) == -EIO);
	sail_check(*reg(&f, RST(0, POR(1))) == 1U && *reg(&f, RST(0, CORE(1))) == 1U);
}

static void cluster1(void)
{
	static const uint32_t order[] = {
		C1_CONFIG, CPU_CONFIG(2), C1_CLOCK, RST(1, TOP), RST(1, DBG),
		RST(1, POR(0)), RST(1, CORE(0)),
	};
	struct fake f;
	struct sail_cores_io io;

	reset(&f, 0U);
	io = io_for(&f);
	/* Its second core needs the cluster out of reset first. */
	sail_check(sail_core_release(&io, 1U, 1U, C1_ENTRY) == -EAGAIN && f.writes == 0U);
	sail_check(sail_core_release(&io, 1U, 0U, C1_ENTRY) == 0);
	check_order(&f, order, ARRAY_SIZE(order));
	sail_check(f.writes == ARRAY_SIZE(order));
	sail_check(*reg(&f, C1_CONFIG) == UINT32_C(0x806));
	sail_check(*reg(&f, CPU_CONFIG(2)) == C1_ENTRY >> 5);
	sail_check(*reg(&f, C1_CLOCK) == UINT32_C(0x80006221));
	sail_check(*reg(&f, RST(1, POR(1))) == 1U && *reg(&f, RST(1, CORE(1))) == 1U);
	/* 100 us at 19.2 MHz before the clock, and again before the core resets. */
	sail_check(f.log[write_at(&f, C1_CLOCK)].time - f.log[write_at(&f, CPU_CONFIG(2))].time >=
		   SETTLE_TICKS);
	sail_check(f.log[write_at(&f, RST(1, POR(0)))].time -
			   f.log[write_at(&f, RST(1, DBG))].time >=
		   SETTLE_TICKS);
	f.writes = 0U;
	f.logged = 0U;
	sail_check(sail_core_release(&io, 1U, 1U, C1_ENTRY) == 0);
	sail_check(*reg(&f, CPU_CONFIG(3)) == C1_ENTRY >> 5);
	sail_check(*reg(&f, RST(1, POR(1))) == 0U && *reg(&f, RST(1, CORE(1))) == 0U);
	sail_check(f.log[write_at(&f, RST(1, POR(1)))].time -
			   f.log[write_at(&f, CPU_CONFIG(3))].time >=
		   SETTLE_TICKS);
	sail_check(f.writes == 3U);
	/* Existing affinity fields are replaced, other configuration bits kept. */
	reset(&f, 0U);
	*reg(&f, C1_CONFIG) = UINT32_C(0xfffffff9);
	io = io_for(&f);
	sail_check(sail_core_release(&io, 1U, 0U, C1_ENTRY) == 0);
	sail_check(*reg(&f, C1_CONFIG) == UINT32_C(0xfff80807));
	/* Cluster-1 lockstep forbids its second core only. */
	reset(&f, BIT(14));
	io = io_for(&f);
	sail_check(sail_core_release(&io, 1U, 0U, C1_ENTRY) == 0);
	f.writes = 0U;
	sail_check(sail_core_release(&io, 1U, 1U, C1_ENTRY) == -ENODEV && f.writes == 0U);
	/* A stopped counter fails before the cluster leaves reset. */
	reset(&f, 0U);
	f.tick = 0U;
	io = io_for(&f);
	sail_check(sail_core_release(&io, 1U, 0U, C1_ENTRY) == -ETIMEDOUT);
	sail_check(*reg(&f, RST(1, TOP)) == 1U && *reg(&f, C1_CLOCK) == UINT32_C(0x80006220));
}

static void faults(void)
{
	unsigned int total;
	struct fake f;
	struct sail_cores_io io;

	reset(&f, 0U);
	io = io_for(&f);
	sail_check(sail_core_release(&io, 1U, 0U, C1_ENTRY) == 0);
	total = f.accesses;
	for (unsigned int i = 1U; i <= total; i++) {
		reset(&f, 0U);
		f.fail_at = i;
		io = io_for(&f);
		sail_check(sail_core_release(&io, 1U, 0U, C1_ENTRY) == -EIO);
		/* Nothing after the failing access, and the core stays in reset. */
		sail_check(f.accesses == i && *reg(&f, RST(1, CORE(0))) == 1U);
	}
}

static void arguments(void)
{
	static const struct {
		uint32_t cluster, cpu, entry;
	} bad[] = {
		{0U, 0U, ENTRY}, {2U, 0U, ENTRY}, {1U, 2U, ENTRY}, {0U, 1U, 0U},
		{0U, 1U, ENTRY + 4U}, {1U, 0U, ENTRY + 16U},
	};
	struct fake f;
	struct sail_cores_io io;

	reset(&f, 0U);
	io = io_for(&f);
	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		sail_check(sail_core_release(&io, bad[i].cluster, bad[i].cpu, bad[i].entry) ==
			   -EINVAL);
	}
	sail_check(sail_core_release(NULL, 0U, 1U, ENTRY) == -EINVAL);
	io.write32 = NULL;
	sail_check(sail_core_release(&io, 0U, 1U, ENTRY) == -EINVAL);
	sail_check(f.accesses == 0U);
}

static void allowlist(void)
{
	/* The running core, the cluster-0 resets it depends on and the comparators. */
	static const uint32_t never[] = {
		UINT32_C(0xf0300040), UINT32_C(0xf0300044), RST(0, TOP), RST(0, DBG),
		RST(0, POR(0)), RST(0, CORE(0)), UINT32_C(0xf1124018), FUSES, COUNTER,
	};

	for (size_t i = 0; i < ARRAY_SIZE(never); i++) {
		sail_check(!sail_cores_write_allowed(never[i]));
	}
	for (uint32_t a = UINT32_C(0xf0300058); a <= UINT32_C(0xf0300074); a += 4U) {
		sail_check(!sail_cores_write_allowed(a));
	}
}

int sail_model_test(void)
{
	lockstep();
	cluster0_cpu1();
	cluster1();
	faults();
	arguments();
	allowlist();
	printf("PASS: core release order, settle delays, lockstep, faults, allowlist\n");
	return 0;
}
