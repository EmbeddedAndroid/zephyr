.. zephyr:code-sample:: sail-bmc
   :name: SAIL BMC

   A board-management style shell on the Qualcomm SAIL UART.

Overview
********

The safety island (SAIL) of a Qualcomm SoC keeps running beside the main
domain (MD), the application processors running Linux. This sample turns it
into a small board management controller with a shell on the SAIL UART:

- ``md status``: MD state derived from the notices the MD sends SAIL during
  boot, reset and shutdown, the notice history, the heartbeat and the
  watchdog state.
- ``md hb <md-address|off>``: watch a 32-bit MD register as a heartbeat; any
  change of its value counts as a beat.
- ``md wdt on [seconds] [reset] | off``: report (and, where the board has a
  reset path, reset) an MD whose heartbeat stopped for the given time. The
  watchdog arms at the first beat after it is enabled, and an MD reset or
  shutdown notice disarms it.
- ``md reset now``: reset the MD. Not available on the IQ-9075 EVK.
- ``sensors``: hottest valid sensor of each MD TSENS controller.
- ``log on [ms] [local] | off``: a periodic ``key=value`` status line;
  ``local`` reads nothing outside SAIL.
- ``peek <address> [count]``: read registers in blocks SAIL can read.

Asynchronous lines (status, notices, watchdog) are printed through the shell,
which redraws the prompt after them.

Platform thread
***************

A platform thread runs the SoC's main-domain reset coordinator
(:kconfig:option:`CONFIG_SAIL_RESET_SERVICE`), so the MD's reboot, warm reset
and power-off notices are answered as with the ``reset_service`` sample. It
hands every notice the coordinator consumes to the MD monitor, which is how
``md status`` sees them. Once a reset or shutdown notice is claimed, the
sample stops reading MD registers (heartbeat, sensors, ``peek``), as the MD
may be gone.

Built with ``-DBMC_IMAGE_ID=<24-bit value>``, the platform thread also
publishes that value in the SAIL-to-MD answer registers after XBL's boot
success notice, and again 12 s after each SDI-disable answer, so software on
the MD can tell which image runs. ``md status`` shows the coordinator result.

Heartbeat from Linux
********************

SAIL cannot read most MD blocks: the TLMM, the APSS watchdog and the MPM give
it a synchronous external abort. It can read the MD TSENS controllers. A Linux
agent that rewrites a passive thermal trip point makes the TSENS driver
reprogram that sensor's threshold register, which SAIL then sees change. On
the IQ-9075 EVK, thermal zone 0 is TSENS2 sensor 0, whose threshold register
is at MD ``0x0c251020``:

.. code-block:: console

   linux# python3 bmc-agent.py 0 1        # toggles trip 0 by 1 m°C per second
   bmc> md hb 0x0c251020
   bmc> md wdt on 10

Building
********

The Lemans SAIL serial engine has no protocol firmware after the boot ROM,
so the console stays silent unless the driver loads one. The firmware is not
part of the tree. Point :kconfig:option:`CONFIG_UART_QCOM_GENI_SE_FW_FILE` at a
QUP firmware image (``qupv3fw.elf`` format) that carries a UART protocol, kept
outside the tree:

.. zephyr-app-commands::
   :zephyr-app: samples/boards/qcom/sail/bmc
   :board: lemans_evk_sail/sail_lemans
   :gen-args: -DCONFIG_UART_QCOM_GENI_SE_FW=y -DCONFIG_UART_QCOM_GENI_SE_FW_FILE=\"/path/to/qupv3fw.elf\"
   :goals: build

Without the firmware the sample still builds, but the UART fails to
initialize and nothing is printed.
