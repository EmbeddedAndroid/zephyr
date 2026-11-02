.. zephyr:code-sample:: sail-reset-service
   :name: SAIL reset service

   Answer the main domain's reset notices on the Qualcomm SAIL.

Overview
********

When the main domain of a Qualcomm Monaco or Lemans SoC reboots or powers
off, its secure firmware asks SAIL to prepare the reset first and waits for
the answer. On a board with a SAIL PMIC, SAIL programs that PMIC before it
answers; on a board without one, it answers once the reset debug enables are
clear. This sample starts the reset coordinator
(:kconfig:option:`CONFIG_SAIL_RESET_SERVICE`) and polls it from its main
thread. In an SMP build (the ``ventuno_q_sail/sail_monaco/smp`` variant) it
polls from a thread pinned to CPU 0 instead, as the coordinator requires.
Its retire callback has nothing to release.

Building
********

.. zephyr-app-commands::
   :zephyr-app: samples/boards/qcom/sail/reset_service
   :board: ventuno_q_sail/sail_monaco
   :goals: build

The sample has no console output. Once it runs on the SAIL of a board with
a SAIL PMIC, a ``reboot`` on the board's application processors restarts the
whole board; without it, the board stays off after Linux prints
``reboot: Restarting system``. On a board without a SAIL PMIC the main
domain's firmware no longer waits out its timeout for the answer.
