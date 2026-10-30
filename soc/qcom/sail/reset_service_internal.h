/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_SOC_QCOM_SAIL_RESET_SERVICE_INTERNAL_H_
#define ZEPHYR_SOC_QCOM_SAIL_RESET_SERVICE_INTERNAL_H_

#include "boot_mode.h"
#include "reset_service.h"

/* Owned by soc.c next to the cold boot state; not for application use. */
struct sail_reset_service {
	struct sail_md_receive receiver;
	struct sail_reset_service_status status;
	struct monaco_pmic_transaction transaction;
	struct monaco_cold_boot *boot;
	const int *boot_result;
	const enum sail_boot_mode *boot_mode;
	struct monaco_timer_io controls;
	int (*reply)(void *context, uint32_t answer, uint32_t index);
	int (*retire)(void *context);
	void *context;
	k_tid_t worker;
	uint32_t entry;
	bool attempted;
	bool busy;
	bool active;
	bool xbl_success_seen;
	bool secure_boot_seen;
	bool hyp_boot_seen;
	bool timer_busy;
};

int sail_reset_start(struct sail_reset_service *service, struct monaco_cold_boot *boot,
		     const int *result, const enum sail_boot_mode *mode,
		     const struct device *device, const struct monaco_timer_io *controls,
		     int (*reply)(void *context, uint32_t answer, uint32_t index), uint32_t entry,
		     int (*retire)(void *context), void *context);
int sail_reset_poll(struct sail_reset_service *service);
int sail_reset_stop(struct sail_reset_service *service);
int sail_reset_status_get(struct sail_reset_service *service,
			  struct sail_reset_service_status *status);
int sail_reset_timer_enter(struct sail_reset_service *service);
void sail_reset_timer_leave(struct sail_reset_service *service);

#endif
