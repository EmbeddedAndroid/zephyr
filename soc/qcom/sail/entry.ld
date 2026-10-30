/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

KEEP(*(.sail_entry))
ASSERT(ABSOLUTE(sail_reset) == 0x0801d400, "SAIL entry must be at 0x0801d400");
