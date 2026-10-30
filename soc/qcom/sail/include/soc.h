/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SOC_QCOM_SAIL_SOC_H_
#define ZEPHYR_SOC_QCOM_SAIL_SOC_H_

#define __GIC_PRESENT 0
#define __TIM_PRESENT 0

#ifndef _ASMLANGUAGE
#include <stdint.h>

enum sail_boot_mode {
	SAIL_BOOT_STOPPED = 0,
	SAIL_BOOT_NORMAL = 1,
	/* Applications run; PCInit is not done and its error result is kept.
	 * The value is a distinct pattern that is easy to spot in an SRAM dump.
	 */
	SAIL_BOOT_PCINIT_DEFERRED = 0x4f425356,
};

/** @brief Return the boot mode selected by the cold initialization. */
enum sail_boot_mode sail_boot_mode_get(void);

/**
 * @brief Return the cold initialization result.
 *
 * @return 0, or a negative cold initialization code (not an errno). In mode
 *	   SAIL_BOOT_PCINIT_DEFERRED this is always the PCInit-required code.
 */
int sail_boot_status(void);

/**
 * @brief Read the boot timer in microseconds.
 *
 * @retval 0 @p value holds the time.
 * @retval -EWOULDBLOCK Caller is in an ISR or has IRQs masked.
 * @retval -EBUSY The timer is in use or held by the reset service.
 * @return Other negative values are cold initialization or timer codes
 *	   (not errno).
 */
int sail_time_us(uint64_t *value);

/**
 * @brief Busy-wait on the boot timer.
 *
 * @retval 0 At least @p value microseconds passed.
 * @retval -EWOULDBLOCK Caller is in an ISR or has IRQs masked.
 * @retval -EBUSY The timer is in use or held by the reset service.
 * @return Other negative values are cold initialization or timer codes
 *	   (not errno).
 */
int sail_delay_us(uint32_t value);

/**
 * @brief Start the reset coordinator on the calling thread.
 *
 * Valid once per boot, only in boot mode SAIL_BOOT_PCINIT_DEFERRED. The caller
 * must own both receive channels, the PMIC, I2C and reset controls exclusively,
 * and must have set up IRQ routing. The calling thread becomes the only thread
 * that may poll or stop the service. All code and data reached from it must be
 * in SRAM.
 *
 * @p retire runs once with local IRQs locked, after a secure reset notice is
 * claimed and before any control, PMIC or answer write. It must not
 * block, must stop all application use of resources lost in the reset, and
 * must return zero only once that is done. Any failure is terminal. While the
 * service exists, sail_time_us() and sail_delay_us() work only from this
 * thread between polls, and not at all after a claim.
 *
 * @param retire Required resource retirement callback.
 * @param context Callback context; must outlive the boot, may be NULL.
 * @retval 0 Both receive channels enabled.
 * @retval -EINVAL Required callback is missing.
 * @retval -EALREADY Start was already attempted.
 * @retval -EWOULDBLOCK Caller is in an ISR or has IRQs masked.
 * @retval -EBUSY A timer call is in progress.
 * @retval -EPERM The boot state is not eligible.
 * @return Other negative values are receiver errors.
 */
int sail_reset_service_start(int (*retire)(void *context), void *context);

/**
 * @brief Handle at most one queued main-domain notice.
 *
 * Call only from the thread that started the service. DDR-ready and boot
 * notices are accepted without effect. A secure SDI-disable notice gets
 * 0xaa0c0000. A secure SOC_HR (0xcd070000) notice retires application
 * resources, prepares the reset controls and PMIC, waits 50 ms and then
 * answers 0xaa030000 as four byte stores. The main domain's secure firmware
 * waits up to 1 s for each answer before it drops PS_HOLD. After the reset
 * answer, one SDI-disable notice is answered and reception stops. Any other
 * notice, or a lost or dropped event, is a terminal error and no answer is
 * written.
 *
 * @retval 0 No queued event.
 * @retval 1 One event handled.
 * @retval -EWOULDBLOCK Caller is in an ISR or has IRQs masked.
 * @retval -EPERM Caller is not the starting thread.
 * @retval -EBUSY A call is already active.
 * @retval -ESHUTDOWN The service has stopped or failed.
 * @return Other negative values are terminal protocol, PMIC or cleanup errors.
 */
int sail_reset_service_poll(void);

/**
 * @brief Stop the service permanently without answering.
 *
 * A later call retries a failed receiver cleanup; the service is never rearmed.
 *
 * @retval 0 Receiver cleanup completed.
 * @retval -EINVAL Activation was never attempted.
 * @retval -EWOULDBLOCK Caller is in an ISR or has IRQs masked.
 * @retval -EPERM Caller is not the starting thread.
 * @retval -EBUSY A call is active.
 * @return A negative receiver cleanup error.
 */
int sail_reset_service_stop(void);

#endif
#endif
