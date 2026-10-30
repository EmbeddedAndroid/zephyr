# Copyright (c) 2026 Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(unittest_host)

# Unit-test toolchains skip CMake's automatic ABI detection.
include(CheckTypeSize)
check_type_size("void *" native_pointer_size BUILTIN_TYPES_ONLY LANGUAGE C)
set(expected_pointer_size "${native_pointer_size}")
if(NOT CMAKE_HOST_APPLE AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86_64|amd64|AMD64)$")
  if(M64_MODE)
    set(expected_pointer_size 8)
  else()
    set(expected_pointer_size 4)
  endif()
endif()

target_compile_definitions(testbinary PRIVATE EXPECTED_POINTER_SIZE=${expected_pointer_size})
target_sources(testbinary PRIVATE
  main.c
  pointer_size.cpp
)
