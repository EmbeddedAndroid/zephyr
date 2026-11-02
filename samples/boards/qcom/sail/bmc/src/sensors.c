/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * SoC temperatures from the main-domain TSENS controllers, read through the
 * SAIL alias. Only the Lemans controllers are described; on other chips no
 * sensor is read.
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/util.h>
#include <soc.h>
#include "bmc.h"

#define SN_STATUS(n)   (0xa0U + 4U * (n))
#define SN_VALID       BIT(21)
#define SN_LAST_TEMP   GENMASK(11, 0)
#define CHIP_LEMANS    0x6019U

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

	if (sail_chip_id_get() != CHIP_LEMANS) {
		return 0;
	}
	for (size_t c = 0; c < ARRAY_SIZE(controllers) && n < max; c++) {
		int32_t hottest = INT32_MIN;
		uint32_t valid = 0U;

		for (uint8_t s = 0U; s < controllers[c].sensors; s++) {
			uint32_t v;
			int32_t deci;

			if (bmc_read32(controllers[c].tm + SN_STATUS(s), &v) != 0) {
				continue;
			}
			deci = sign_extend(FIELD_GET(SN_LAST_TEMP, v), 11);
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
