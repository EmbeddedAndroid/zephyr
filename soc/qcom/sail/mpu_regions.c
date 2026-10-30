/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <zephyr/arch/arm/mpu/arm_mpu.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/sys/util.h>
#include <soc.h>

static const struct arm_mpu_region mpu_regions[] = {
	MPU_REGION_ENTRY("vectors", (uintptr_t)__rom_region_start,
			 REGION_RAM_TEXT_ATTR((uintptr_t)_vector_end)),
	MPU_REGION_ENTRY("text", (uintptr_t)__text_region_start,
			 REGION_RAM_TEXT_ATTR((uintptr_t)__rodata_region_start)),
	MPU_REGION_ENTRY("rodata", (uintptr_t)__rodata_region_start,
			 REGION_RAM_RO_ATTR((uintptr_t)__rodata_region_end)),
	MPU_REGION_ENTRY("data", (uintptr_t)__rom_region_end,
			 REGION_RAM_ATTR((uintptr_t)__kernel_ram_end)),
	MPU_REGION_ENTRY("device", 0xc0000000U, REGION_DEVICE_ATTR(0xffffffffU)),
};

const struct arm_mpu_config mpu_config = {
	.num_regions = ARRAY_SIZE(mpu_regions),
	.mpu_regions = mpu_regions,
};
