/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

int sail_model_test(void);

void sail_check_failed(const char *file, int line, const char *expr)
{
	printk("\n    Check failed at %s:%d: %s\n", file, line, expr);
	ztest_test_fail();
}

ZTEST(sail_boot, test_component_model)
{
	zassert_equal(sail_model_test(), 0);
}

ZTEST_SUITE(sail_boot, NULL, NULL, NULL, NULL, NULL);
