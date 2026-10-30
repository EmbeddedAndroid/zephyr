# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(
  entry.S
  soc.c
  boot_mode.c
  monaco/timer.c
  monaco/md_bist.c
  monaco/md_publication.c
  monaco/clock.c
  monaco/cpu_pll.c
  monaco/early_clock.c
  monaco/hbcu.c
  monaco/pmic.c
  monaco/pmic_boot.c
  monaco/pmic_policy.c
  monaco/post_pmic_clock.c
  monaco/i2c.c
  monaco/i2c_boot.c
  monaco/i2c_profile.c
  monaco/monaco_policy.c
  monaco/board_policy.c
  monaco/cold_boot.c
  monaco/native_io.c
)
zephyr_library_include_directories(. monaco)
zephyr_library_sources_ifdef(CONFIG_ARM_MPU mpu_regions.c)
zephyr_include_directories(include)
zephyr_linker_sources(ROM_START SORT_KEY 0x000sail entry.ld)
set(SOC_LINKER_SCRIPT
  "${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_a_r/scripts/linker.ld"
  CACHE INTERNAL ""
)
