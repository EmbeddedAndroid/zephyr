.. zephyr:board:: lemans_evk_sail

Overview
********

The Qualcomm IQ-9075 EVK is built around a QCS9075 SoC of the Lemans family.
Next to its application processors the SoC has a safety island (SAIL) with
Cortex-R52 cores. SAIL boots from its own SPI-NOR flash, before the
application processors. On this board SAIL has no PMIC of its own: it is
supplied by the main-domain PMICs.

This board runs Zephyr on core 0 of the safety island. The other SAIL cores
stay parked.

Hardware
********

- Cortex-R52 core 0, entered by the boot ROM in A32 EL2
- 879 KiB of SAIL SRAM at 0x0801d400 for code and data
- GICv3 interrupt controller
- Receive mailbox from the application processors

Supported Features
==================

.. zephyr:board-supported-hw::

Before applications start, the SoC code reads the chip from the SAIL
hardware version register and takes the board properties from the
``qcom,sail-platform`` devicetree node for that chip; this board's node
describes no SAIL PMIC. On Lemans the SoC code configures two SAIL pads as the stock SAIL firmware does: the ``PSAIL_ERR_N``
pad becomes a pulled-up GPIO input and the SAIL sleep-enable output is routed
out. It then programs the SAIL clocks, with the PLLs on their defaults and
the CPU PLL moved from its boot ROM rate to 1593.6 MHz, and takes part in the
main-domain BIST, which releases the application processors. There is no SAIL
PMIC to set up, and the power sequencer programs (PCInit) are not installed.
The board enables :kconfig:option:`CONFIG_SAIL_START_WITHOUT_PCINIT`, so
applications start with ``sail_boot_mode_get()`` returning
``SAIL_BOOT_PCINIT_DEFERRED``. If any of these steps fails, the boot stops
before applications start.

The reset coordinator (:kconfig:option:`CONFIG_SAIL_RESET_SERVICE`) needs a
SAIL PMIC and does not start on this board: the main domain's reset and
shutdown notices are not answered, so a Linux reboot does not restart the
board.

The :ref:`snippet-qcom-sail-iq9075-evk` snippet adds this board's node to a
``ventuno_q_sail`` build. The resulting image runs on both boards: the chip
it finds at cold initialization selects the sequence.

There is no system timer driver: kernel timeouts and sleeps are not
available.

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

Flashing
========

The image runs from SRAM at 0x0801d400. The SAIL boot ROM loads it from the
``SAIL_HYP`` partition of the board's SPI-NOR flash, with ``SAIL_HYP_BKUP``
as the backup copy. It expects an ELF in the Qualcomm MBN format with a hash
segment, which the Zephyr build does not produce, so ``zephyr.elf`` has to
be wrapped before it is written. There is no public tool for that step yet.
Boards that enforce secure boot for SAIL only load signed images. There is
no ``west flash`` runner.

With the board in EDL mode, write the image with `qdl`_ and the SPI-NOR
partition layout of the board's firmware package:

.. code-block:: console

   qdl --storage spinor prog_firehose_ddr.elf rawprogram0.xml patch0.xml

Debugging
=========

The console goes to the RAM console, ``ram_console_buf`` in SAIL SRAM. Read
it with a JTAG debugger that can attach to the Cortex-R52 cores of the SoC.

Building
========

Build the Hello World sample with:

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: lemans_evk_sail/sail_lemans
   :goals: build

References
**********

.. target-notes::

.. _qdl: https://github.com/linux-msm/qdl
