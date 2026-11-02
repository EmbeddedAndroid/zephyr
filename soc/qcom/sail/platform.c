/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "platform.h"

const struct sail_platform *sail_platform_find(const struct sail_platforms *platforms,
					       uint16_t chip_id)
{
	if (platforms == NULL || platforms->entries == NULL) {
		return NULL;
	}
	const struct sail_platform *found = NULL;

	for (size_t i = 0; i < platforms->count; i++) {
		if (platforms->entries[i].chip_id != chip_id) {
			continue;
		}
		/* Two boards on one chip could only be told apart by probing. */
		if (found != NULL) {
			return NULL;
		}
		found = &platforms->entries[i];
	}
	return found;
}
