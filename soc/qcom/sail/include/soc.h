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
 * @brief Return the chip the image runs on.
 *
 * @return Chip ID (bits 31:16 of the SAIL hardware version register) of the
 *	   chip the cold initialization selected, or 0 if it selected none.
 */
uint16_t sail_chip_id_get(void);

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
 * must own both receive channels, the PMIC, I2C and reset controls exclusively
 * (on a board without a SAIL PMIC: the receive channels and reset controls),
 * and must have set up IRQ routing. The calling thread becomes the only thread
 * that may poll or stop the service. Under SMP that thread must be pinned to
 * CPU 0 only (k_thread_cpu_pin() before it starts). All code and data reached
 * from it must be in SRAM.
 *
 * @p retire runs once with the service spinlock held (local IRQs locked),
 * after a secure reset or shutdown notice is claimed and before any control,
 * PMIC or answer write. It must not block, must stop all application use of
 * resources lost in the reset, and must return zero only once that is done.
 * Any failure is terminal. While the service exists, sail_time_us() and
 * sail_delay_us() work only from this thread between polls, and not at all
 * after a claim.
 *
 * @param retire Required resource retirement callback.
 * @param context Callback context; must outlive the boot, may be NULL.
 * @retval 0 Both receive channels enabled.
 * @retval -EINVAL Required callback is missing.
 * @retval -EALREADY Start was already attempted.
 * @retval -EWOULDBLOCK Caller is in an ISR or has IRQs masked.
 * @retval -EBUSY A timer call is in progress.
 * @retval -EPERM The boot state is not eligible, or under SMP the caller is
 *	   not pinned to CPU 0.
 * @return Other negative values are receiver errors.
 */
int sail_reset_service_start(int (*retire)(void *context), void *context);

/**
 * @brief Handle at most one queued main-domain notice.
 *
 * Call only from the thread that started the service. DDR-ready and boot
 * notices are accepted without effect. A secure SDI-disable notice gets
 * 0xaa0c0000. A secure SOC_HR (0xcd070000) notice retires application
 * resources, prepares the reset controls and the PMIC reset policy, waits 50 ms
 * and answers 0xaa030000 as four byte stores. A secure graceful shutdown
 * (0xcd060000) notice retires resources, prepares the PMIC for power-off and
 * answers at once; the reset controls are not touched. A secure warm reset
 * (0xcd050000) notice retires resources, sets the PMIC warm reset policy
 * (C9=01, CB=0a, CC=00, read back) and answers 0xaa070000 at once, as the
 * stock SAIL firmware does. On a board without a SAIL PMIC all three notices
 * retire resources, clear the reset debug enables and
 * restore the main-domain PS_HOLD mask if the debug enables are set (both read
 * back), and answer at once; the main domain's PS_HOLD then resets or powers
 * off the board through the main PMIC. The main domain's
 * secure firmware waits up to 1 s for each answer before it drops PS_HOLD.
 * After the reset answer, one SDI-disable notice is answered and reception
 * stops. Any other notice, or a lost or dropped event, is a terminal error and
 * no answer is written.
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

/** A main-domain notice the reset coordinator consumed. */
struct sail_reset_service_notice {
	/** Count of notices consumed so far, this one included. */
	uint64_t sequence;
	/** Notice word as the main domain wrote it. */
	uint32_t word;
	/** Receive channel: 0 non-secure, 1 secure. */
	uint32_t channel;
};

/**
 * @brief Return the last notice the coordinator consumed.
 *
 * Call only from the thread that started the service. Each poll that returns
 * a non-zero value has consumed one notice; the sequence tells repeated reads
 * of the same notice apart. The notice is reported whether or not the
 * coordinator accepted it, also after the service stopped.
 *
 * @param notice Filled with the notice.
 * @retval 0 @p notice holds the last consumed notice.
 * @retval -ENOMSG No notice consumed yet.
 * @retval -EINVAL @p notice is NULL, or the service was never started.
 * @retval -EWOULDBLOCK Caller is in an ISR or has IRQs masked.
 * @retval -EPERM Caller is not the starting thread.
 * @retval -EBUSY A call is already active.
 */
int sail_reset_service_notice_get(struct sail_reset_service_notice *notice);

/**
 * @brief Publish a word in the SAIL-to-main-domain answer registers.
 *
 * Lets the main domain read an image identifier or state from the registers
 * that carry the coordinator's answers, written the same way (four byte
 * stores, low byte first, no doorbell). Call only from the thread that
 * started the service, while it listens and before any reset or shutdown
 * notice is claimed. The next answer overwrites the word; publishing over an
 * answer the main domain is still waiting for (it polls up to 1 s) loses
 * that answer, so the caller must leave a margin after one.
 *
 * @param word Word to publish; not zero and not 0xaaXXXXXX, the answer form.
 * @retval 0 Published.
 * @retval -EINVAL Bad word, or the service was never started.
 * @retval -EPERM The service is not listening, a notice was claimed, or the
 *	   caller is not the starting thread.
 * @retval -EWOULDBLOCK Caller is in an ISR or has IRQs masked.
 * @retval -EBUSY A call is already active.
 */
int sail_reset_service_publish(uint32_t word);

#if defined(CONFIG_SMP)
/**
 * @brief Report how a core was started at boot.
 *
 * The image starts the second core of its own cluster when the fuses do not
 * pair it into lockstep, and the cluster-0 image starts cluster 1 when a
 * cluster-1 image is loaded at CONFIG_SAIL_SECONDARY_CLUSTER_ENTRY.
 *
 * @param cluster Cluster index, 0 or 1.
 * @param cpu Core index in the cluster, 0 or 1.
 * @retval 0 The core was started (or is the running image's first core).
 * @retval -EINPROGRESS This image has not started, and does not start, the core.
 * @retval -ENODEV The core is the checker of a lockstep pair.
 * @retval -ENOENT No cluster-1 image is loaded at its entry.
 * @retval -EINVAL Bad core index.
 * @return Other negative values are release errors.
 */
int sail_core_status(uint32_t cluster, uint32_t cpu);
#endif

#endif
#endif
