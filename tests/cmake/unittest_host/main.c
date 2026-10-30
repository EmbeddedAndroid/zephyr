/*
 * Copyright (c) 2026 Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>

#include <zephyr/ztest.h>

size_t cpp_pointer_size(void);

ZTEST(unittest_host, test_c_abi)
{
	zassert_equal(sizeof(void *), EXPECTED_POINTER_SIZE);
}

ZTEST(unittest_host, test_cpp_abi)
{
	zassert_equal(cpp_pointer_size(), EXPECTED_POINTER_SIZE);
}

ZTEST_SUITE(unittest_host, NULL, NULL, NULL, NULL, NULL);
