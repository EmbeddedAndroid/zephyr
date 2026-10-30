.. zephyr:board:: ventuno_q_sail

Overview
********

The Arduino VENTUNO Q is built around the Qualcomm QCS8275 (Monaco). Next
to its application processors the SoC has a safety island (SAIL) with
Cortex-R52 cores. SAIL boots from its own SPI-NOR flash, independently of
the application processors, and takes part in their power-up and reset.

This board runs Zephyr on core 0 of the safety island. The other SAIL cores
stay parked.

Hardware
********

- Cortex-R52 core 0, entered by the boot ROM in A32 EL2
- 879 KiB of SAIL SRAM at 0x0801d400 for code and data
- GICv3 interrupt controller
- I2C controller to the SAIL PMICs, used by the boot sequence

Supported Features
==================

.. zephyr:board-supported-hw::

Before the kernel starts, the SoC code runs the cold initialization of the
safety island: timer, main-domain BIST, clocks and PMICs. The next step,
installing the power sequencer programs (PCInit), is not implemented. The
board enables :kconfig:option:`CONFIG_SAIL_START_WITHOUT_PCINIT`, so
applications start at that point and ``sail_boot_mode_get()`` returns
``SAIL_BOOT_PCINIT_DEFERRED``. Any other initialization failure stops the
boot.

There is no system timer driver: kernel timeouts and sleeps are not
available.

Connections and IOs
===================

SAIL has no user-facing connectors or UART on this board. It reaches its
PMICs through its own I2C controller and receives messages from the
application processors through the receive mailbox.

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
it with a JTAG debugger that can attach to the Cortex-R52 cores of the SoC,
at the address of ``ram_console_buf`` in ``zephyr.map``.

Building
========

Build the Hello World sample with:

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: ventuno_q_sail/sail_monaco
   :goals: build

The :zephyr:code-sample:`sail-reset-service` sample answers the main
domain's reset notices, which lets the board reboot under Linux.

References
**********

.. target-notes::

.. _qdl: https://github.com/linux-msm/qdl
