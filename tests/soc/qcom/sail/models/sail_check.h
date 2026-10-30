/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SAIL_CHECK_H
#define SAIL_CHECK_H

/* Fails the running ztest case from any call depth, including model callbacks. */
void sail_check_failed(const char *file, int line, const char *expr);

#define sail_check(expr)                                                                           \
	do {                                                                                       \
		if (!(expr)) {                                                                     \
			sail_check_failed(__FILE__, __LINE__, #expr);                              \
		}                                                                                  \
	} while (0)

#endif /* SAIL_CHECK_H */
