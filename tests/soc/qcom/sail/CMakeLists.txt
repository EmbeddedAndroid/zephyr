# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(qcom_sail_boot)

set(soc_dir "${ZEPHYR_BASE}/soc/qcom/sail")
set(test_cases
  timer
  clock
  cpu_pll
  early_clock
  md_bist
  md_publication
  monaco_policy
  hbcu
  i2c
  i2c_profile
  i2c_boot
  pmic
  pmic_boot
  pmic_policy
  post_pmic_clock
  board_policy
  cold_boot
  boot_mode
  pmic_reset
)

if(NOT SAIL_TEST_CASE IN_LIST test_cases)
  message(FATAL_ERROR "Select one SAIL_TEST_CASE from tests.yaml")
endif()

target_include_directories(testbinary PRIVATE
  "${soc_dir}"
  "${soc_dir}/include"
  "${soc_dir}/monaco"
)

target_compile_definitions(testbinary PRIVATE
  SAIL_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)


target_sources(testbinary PRIVATE
  main.c
  "models/test_${SAIL_TEST_CASE}.c"
  "${soc_dir}/boot_mode.c"
  "${soc_dir}/monaco/board_policy.c"
  "${soc_dir}/monaco/clock.c"
  "${soc_dir}/monaco/cold_boot.c"
  "${soc_dir}/monaco/cpu_pll.c"
  "${soc_dir}/monaco/early_clock.c"
  "${soc_dir}/monaco/hbcu.c"
  "${soc_dir}/monaco/i2c.c"
  "${soc_dir}/monaco/i2c_boot.c"
  "${soc_dir}/monaco/i2c_profile.c"
  "${soc_dir}/monaco/md_bist.c"
  "${soc_dir}/monaco/md_publication.c"
  "${soc_dir}/monaco/monaco_policy.c"
  "${soc_dir}/monaco/pmic.c"
  "${soc_dir}/monaco/pmic_boot.c"
  "${soc_dir}/monaco/pmic_policy.c"
  "${soc_dir}/monaco/pmic_reset.c"
  "${soc_dir}/monaco/post_pmic_clock.c"
  "${soc_dir}/monaco/timer.c"
)
