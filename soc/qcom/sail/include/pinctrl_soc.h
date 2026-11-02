/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_PINCTRL_SOC_H_
#define ZEPHYR_SOC_QCOM_SAIL_PINCTRL_SOC_H_

#include <stdint.h>
#include <zephyr/devicetree.h>

typedef struct pinctrl_soc_pin {
	uint8_t pad;
	uint8_t func;
	uint8_t drive_ma;
	uint8_t pull;
} pinctrl_soc_pin_t;

#define SAIL_TLMM_PULL_NONE 0U
#define SAIL_TLMM_PULL_DOWN 1U
#define SAIL_TLMM_PULL_UP   3U

#define SAIL_TLMM_PULL(node_id)                                                                    \
	(DT_PROP(node_id, bias_pull_up)     ? SAIL_TLMM_PULL_UP                                   \
	 : DT_PROP(node_id, bias_pull_down) ? SAIL_TLMM_PULL_DOWN                                 \
					    : SAIL_TLMM_PULL_NONE)

#define SAIL_TLMM_DT_PIN(node_id)                                                                  \
	{.pad = DT_PROP_BY_IDX(node_id, pinmux, 0),                                                \
	 .func = DT_PROP_BY_IDX(node_id, pinmux, 1),                                               \
	 .drive_ma = DT_PROP(node_id, drive_strength),                                             \
	 .pull = SAIL_TLMM_PULL(node_id)},

#define Z_PINCTRL_STATE_PIN_INIT(node_id, prop, idx)                                               \
	SAIL_TLMM_DT_PIN(DT_PROP_BY_IDX(node_id, prop, idx))

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)                                                   \
	{DT_FOREACH_PROP_ELEM(node_id, prop, Z_PINCTRL_STATE_PIN_INIT)}

#endif
