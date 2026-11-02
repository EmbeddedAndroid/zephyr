/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/* SoC temperatures from the main-domain TSENS controllers, read through the SAIL alias. */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/util.h>
#include "bmc.h"

#define SN_STATUS(n)   (0xa0U + 4U * (n))
#define SN_VALID       BIT(21)
#define SN_LAST_TEMP   GENMASK(11, 0)

struct tsens {
	uint32_t tm;
	uint8_t sensors;
};

/* TM register blocks (SAIL view) and sensor counts. */
static const struct tsens controllers[] = {
	{0xcc263000U, 12U},
	{0xcc265000U, 12U},
	{0xcc251000U, 13U},
	{0xcc252000U, 13U},
};

size_t bmc_sensors_read(struct bmc_sensor *out, size_t max)
{
	size_t n = 0;

	for (size_t c = 0; c < ARRAY_SIZE(controllers) && n < max; c++) {
		int32_t hottest = INT32_MIN;
		uint32_t valid = 0U;

		for (uint8_t s = 0U; s < controllers[c].sensors; s++) {
			uint32_t v = sys_read32(controllers[c].tm + SN_STATUS(s));
			int32_t deci = sign_extend(FIELD_GET(SN_LAST_TEMP, v), 11);

			if ((v & SN_VALID) != 0U) {
				valid++;
				hottest = MAX(hottest, deci);
			}
		}
		snprintf(out[n].name, sizeof(out[n].name), "tsens%u", (unsigned int)c);
		out[n].valid = valid != 0U;
		out[n].value = valid != 0U ? hottest : 0;
		snprintf(out[n].unit, sizeof(out[n].unit), "dC");
		n++;
	}
	return n;
}
