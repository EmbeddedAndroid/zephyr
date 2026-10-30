/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_NATIVE_IO_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_NATIVE_IO_H_

#include <stdint.h>

/* The caller must establish a Device mapping before using these callbacks. */
int monaco_native_read32(void *context, uint32_t address, uint32_t *value);
int monaco_native_write32(void *context, uint32_t address, uint32_t value);
/* Writes only the BIST status words, through the ordered publisher. */
int monaco_native_md_status_write32(void *context, uint32_t address, uint32_t value);
int monaco_native_read_mpidr(void *context, uint32_t *value);
int monaco_native_read_cpsr(void *context, uint32_t *value);

#endif
