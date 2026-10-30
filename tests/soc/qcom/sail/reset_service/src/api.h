/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RESET_MODEL_API_H
#define RESET_MODEL_API_H
#include "sail_check.h"
#include <zephyr/toolchain.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#define CONFIG_SMP       0
#define CONFIG_USERSPACE 0
#define IS_ENABLED(x)    (x)
#define BIT(n)           (1U << (n))
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#define K_NO_WAIT 0
extern unsigned int test_irq_depth;
extern bool test_isr;
extern uintptr_t test_thread;
typedef uintptr_t k_tid_t;
static inline bool k_is_in_isr(void)
{
	return test_isr;
}
static inline k_tid_t k_current_get(void)
{
	return test_thread;
}
static inline unsigned int irq_lock(void)
{
	unsigned int key = test_irq_depth;

	test_irq_depth++;
	return key;
}
static inline void irq_unlock(unsigned int key)
{
	test_irq_depth = key;
}
static inline bool arch_irq_unlocked(unsigned int key)
{
	return key == 0;
}
struct device {
	bool ready;
};
static inline bool device_is_ready(const struct device *d)
{
	return d->ready;
}
struct k_msgq {
	char *data;
	uint32_t bytes, depth, used, head;
};
static inline void k_msgq_init(struct k_msgq *q, char *data, uint32_t bytes, uint32_t depth)
{
	*q = (struct k_msgq){data, bytes, depth, 0, 0};
}
static inline uint32_t k_msgq_num_used_get(const struct k_msgq *q)
{
	return q->used;
}
static inline int k_msgq_put(struct k_msgq *q, const void *data, int timeout)
{
	(void)timeout;
	if (q->used == q->depth) {
		return -ENOMSG;
	}
	memcpy(q->data + ((q->head + q->used) % q->depth) * q->bytes, data, q->bytes);
	q->used++;
	return 0;
}
static inline int k_msgq_get(struct k_msgq *q, void *data, int timeout)
{
	(void)timeout;
	if (!q->used) {
		return -ENOMSG;
	}
	memcpy(data, q->data + q->head * q->bytes, q->bytes);
	q->head = (q->head + 1) % q->depth;
	q->used--;
	return 0;
}
struct mbox_msg {
	const void *data;
	size_t size;
};
typedef void (*mbox_callback_t)(const struct device *, uint32_t, void *, struct mbox_msg *);
int mbox_register_callback(const struct device *device, uint32_t channel, mbox_callback_t callback,
			   void *context);
int mbox_set_enabled(const struct device *device, uint32_t channel, bool enabled);
int mbox_mtu_get(const struct device *device);
uint32_t mbox_max_channels_get(const struct device *device);
uint32_t sys_read32(uintptr_t address);
void sys_write32(uint32_t value, uintptr_t address);
void barrier_dsync_fence_full(void);
#endif
