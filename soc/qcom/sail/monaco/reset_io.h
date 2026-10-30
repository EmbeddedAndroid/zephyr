/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_MONACO_RESET_IO_H_
#define ZEPHYR_SOC_QCOM_SAIL_MONACO_RESET_IO_H_

#include <stdint.h>

/* Register access for the reset service, limited to its fixed addresses. */
int monaco_reset_read32(void *context, uint32_t address, uint32_t *value);
int monaco_reset_write32(void *context, uint32_t address, uint32_t value);
int monaco_reset_reply(void *context, uint32_t answer, uint32_t index);

#endif
