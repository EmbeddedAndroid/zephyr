/*
 * Copyright (c) 2026 Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstddef>

extern "C" std::size_t cpp_pointer_size(void)
{
	return sizeof(void *);
}
