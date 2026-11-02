/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <zephyr/arch/arm/mpu/arm_mpu.h>
#include <zephyr/devicetree.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/sys/util.h>
#include <soc.h>

/* SRAM outside this image that it shares with the image on the other cluster. */
#define SHARED_BASE DT_REG_ADDR(DT_NODELABEL(sail_cluster_shared))
#define SHARED_SIZE DT_REG_SIZE(DT_NODELABEL(sail_cluster_shared))

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
#if DT_NODE_EXISTS(DT_NODELABEL(sail_cluster_shared))
	MPU_REGION_ENTRY("shared", SHARED_BASE, REGION_DEVICE_ATTR(SHARED_BASE + SHARED_SIZE)),
#endif
};

const struct arm_mpu_config mpu_config = {
	.num_regions = ARRAY_SIZE(mpu_regions),
	.mpu_regions = mpu_regions,
};
