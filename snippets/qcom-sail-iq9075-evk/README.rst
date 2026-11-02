.. _snippet-qcom-sail-iq9075-evk:

Qualcomm SAIL IQ-9075 EVK platform (qcom-sail-iq9075-evk)
#########################################################

.. code-block:: console

   west build -S qcom-sail-iq9075-evk [...]

Overview
********

A safety island (SAIL) image selects its chip at cold initialization from
the SAIL hardware version register, and takes the board properties for that
chip from a ``qcom,sail-platform`` devicetree node. This snippet adds the
IQ-9075 EVK node (Lemans, no SAIL PMIC) to a VENTUNO Q build, so one image
runs on both boards: on Monaco it sets up the VENTUNO Q SAIL PMIC, on Lemans
it ends the cold initialization after the main-domain BIST handshake.

The snippet also describes the IQ-9075 EVK SAIL console, serial engine 2 of
the Lemans SAIL QUP, and selects it as the console and shell UART. The UART
is listed in the node's ``qcom,devices``, so it is initialized only on
Lemans; on Monaco the image never touches the Lemans pad controller, clock
controller or QUP and runs without a console. As on the ``lemans_evk_sail``
board, the serial engine needs a UART protocol firmware supplied from outside
the tree, and the console is used only when the application enables
:kconfig:option:`CONFIG_SERIAL`.

Requirements
************

The ``ventuno_q_sail`` board, single-core or its ``smp`` variant.
