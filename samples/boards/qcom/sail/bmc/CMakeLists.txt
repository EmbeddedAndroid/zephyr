# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sail_bmc)
target_sources(app PRIVATE src/main.c src/md.c src/sensors.c src/shell.c)
# The MD receiver and protocol decoder are SoC interfaces without a public header.
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/soc/qcom/sail)
if(DEFINED BMC_BUILD_ID)
  target_compile_definitions(app PRIVATE BMC_BUILD_ID="${BMC_BUILD_ID}")
endif()
