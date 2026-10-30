.. zephyr:code-sample:: sail-reset-service
   :name: SAIL reset service

   Answer the main domain's reset notices on the Qualcomm SAIL.

Overview
********

When the main domain of a Qualcomm Monaco SoC reboots or powers off, its
secure firmware asks SAIL to prepare the reset first and waits for the
answer. This sample starts the reset coordinator
(:kconfig:option:`CONFIG_SAIL_RESET_SERVICE`) and polls it from its main
thread. Its retire callback has nothing to release.

Building
********

.. zephyr-app-commands::
   :zephyr-app: samples/boards/qcom/sail/reset_service
   :board: ventuno_q_sail/sail_monaco
   :goals: build

The sample has no console output. Once it runs on SAIL, a ``reboot`` on the
board's application processors restarts the whole board; without it, the
board stays off after Linux prints ``reboot: Restarting system``.
