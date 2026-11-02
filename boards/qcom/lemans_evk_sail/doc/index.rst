.. zephyr:board:: lemans_evk_sail

Overview
********

The Qualcomm IQ-9075 EVK is built around a QCS9075 SoC of the Lemans family.
Next to its application processors the SoC has a safety island (SAIL) with
Cortex-R52 cores. SAIL boots from its own SPI-NOR flash, before the
application processors. On this board SAIL has no PMIC of its own: it is
supplied by the main-domain PMICs.

This board runs Zephyr on core 0 of the safety island; the other SAIL cores
stay parked. The multi-core variants below run Zephyr on every core the
fuses allow.

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

The reset coordinator (:kconfig:option:`CONFIG_SAIL_RESET_SERVICE`) answers
the main domain's reset and shutdown notices at once on this board, after
clearing any reset debug enables; there is no SAIL PMIC to prepare. The
main domain's PS_HOLD drop then resets or powers off the board through the
main PMICs. A Linux ``reboot`` restarts the board only when the main
domain's firmware programs a PMIC hard reset for it: after a PMIC warm
reset the board stays silent, with or without the answer.

The :ref:`snippet-qcom-sail-iq9075-evk` snippet adds this board's node to a
``ventuno_q_sail`` build. The resulting image runs on both boards: the chip
it finds at cold initialization selects the sequence.

Single-core images have no system timer driver: kernel timeouts and sleeps
are not available.

Multi-core images
=================

The four SAIL cores form two clusters of two, and each cluster has its own
GIC, so one Zephyr image spans one cluster. Two board variants build the
images of a multi-core system:

- ``lemans_evk_sail/sail_lemans/smp``: the cold-initialized image on cluster 0, an SMP
  image of both cluster-0 cores. Its SRAM ends at 0x080c0000.
- ``lemans_evk_sail/sail_lemans/cluster1``: an SMP image of the two cluster-1 cores at
  0x080c0000, without cold initialization or main-domain services.

Both images must be loaded together in the SAIL image. After its cold
initialization the cluster-0 image starts cluster 1 at
:kconfig:option:`CONFIG_SAIL_SECONDARY_CLUSTER_ENTRY` when an entry vector
is found there. Each image starts the second core of its cluster unless the
boot fuses pair the cluster into lockstep; ``sail_core_status()`` reports
the result for each core. The images share the SRAM page at 0x080f8000.

Both variants use the per-core generic timer as the system timer. A reset
coordinator in the cluster-0 image must run on a thread pinned to CPU 0.

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
