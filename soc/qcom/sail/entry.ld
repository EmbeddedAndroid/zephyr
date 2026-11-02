/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

KEEP(*(.sail_entry))
ASSERT(ABSOLUTE(sail_reset) == CONFIG_SAIL_ENTRY_ADDRESS, "SAIL entry must start the image SRAM");
