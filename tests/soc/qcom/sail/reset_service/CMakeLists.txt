# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sail_reset_service_model)

set(soc_dir "${ZEPHYR_BASE}/soc/qcom/sail")
# Keep the finite kernel/Device boundary model out of the Ztest runner headers.
add_library(reset_service_model OBJECT src/model.c)
target_link_libraries(reset_service_model PRIVATE test_interface)
target_include_directories(reset_service_model BEFORE PRIVATE
  stubs
  src
  "${CMAKE_CURRENT_SOURCE_DIR}/../models"
  "${soc_dir}"
  "${soc_dir}/include"
  "${soc_dir}/monaco"
)
target_compile_definitions(reset_service_model PRIVATE
  SAIL_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures"
)
target_sources(reset_service_model PRIVATE
  "${soc_dir}/reset_service.c"
  "${soc_dir}/md_receive.c"
  "${soc_dir}/md_protocol.c"
  "${soc_dir}/boot_mode.c"
  "${soc_dir}/monaco/reset_io.c"
)
foreach(component
    board_policy clock cold_boot cpu_pll early_clock hbcu i2c i2c_boot
    i2c_profile md_bist md_publication monaco_policy pmic pmic_boot pmic_policy
    pmic_reset pmic_shutdown pmic_transaction post_pmic_clock reset_prepare timer)
  target_sources(reset_service_model PRIVATE "${soc_dir}/monaco/${component}.c")
endforeach()
target_sources(testbinary PRIVATE src/main.c $<TARGET_OBJECTS:reset_service_model>)
