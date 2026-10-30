# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(qcom_sail_md_protocol)

target_include_directories(app PRIVATE
  "${ZEPHYR_BASE}/drivers/mbox"
  "${ZEPHYR_BASE}/soc/qcom/sail"
)
target_sources(app PRIVATE
  src/main.c
  "${ZEPHYR_BASE}/soc/qcom/sail/md_protocol.c"
)
