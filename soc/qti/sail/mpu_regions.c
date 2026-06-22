/*
 * MPU regions for the SAIL R52 stub.
 * SPDX-License-Identifier: Apache-2.0
 *
 * The image runs from SAIL on-chip SRAM at 0x08000000. All MMIO the
 * stub touches (TCSR SAIL<->MD shadow regs at 0x01F40000, the GENI
 * console UART around 0x00988000, APSS_INTU IPC at 0x17824000) is
 * outside SRAM, so we expose two device windows: the low peripheral
 * space [0x00000000..0x08000000) and the high peripheral space
 * [0x10000000..0xFFFFFFFF).
 */

#include <zephyr/sys/slist.h>
#include <zephyr/linker/linker-defs.h>
#include <zephyr/arch/arm/mpu/arm_mpu.h>

static const struct arm_mpu_region mpu_regions[] = {
	/* Low MMIO: TCSR (0x01F40000) + SAIL QUPv3 GENI UART (~0x00988000) */
	MPU_REGION_ENTRY("DEVICE_LOW",
			 0x00000000UL,
			 REGION_DEVICE_ATTR(0x08000000UL)),

	/* Zephyr vector table (forced to 0x08021000 via load offset) */
	MPU_REGION_ENTRY("vector",
			 (uintptr_t)_vector_start,
			 REGION_RAM_TEXT_ATTR((uintptr_t)_vector_end)),

	/* Zephyr text */
	MPU_REGION_ENTRY("SRAM_TEXT",
			 (uintptr_t)__text_region_start,
			 REGION_RAM_TEXT_ATTR((uintptr_t)__rodata_region_start)),

	/* Zephyr rodata */
	MPU_REGION_ENTRY("SRAM_RODATA",
			 (uintptr_t)__rodata_region_start,
			 REGION_RAM_RO_ATTR((uintptr_t)__rodata_region_end)),

	/* Zephyr data/bss */
	MPU_REGION_ENTRY("SRAM_DATA",
			 (uintptr_t)__rom_region_end,
			 REGION_RAM_ATTR((uintptr_t)__kernel_ram_end)),

	/* High MMIO: APSS_INTU IPC (0x17824000), GIC, etc. */
	MPU_REGION_ENTRY("DEVICE_HIGH",
			 0x10000000UL,
			 REGION_DEVICE_ATTR(0xFFFFFFFFUL)),
};

const struct arm_mpu_config mpu_config = {
	.num_regions = ARRAY_SIZE(mpu_regions),
	.mpu_regions = mpu_regions,
};
