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

Requirements
************

The ``ventuno_q_sail`` board.
