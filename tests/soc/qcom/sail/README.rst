Qualcomm SAIL cold-init models
##############################

These unit tests run the SAIL boot helpers against models of their
registers, timer and PMICs, through the same callbacks the hardware
backend uses. They check the access order, the bounds on every wait, the
handling of failed accesses and the state left behind after a failure.

The early clock model compares the register accesses of the helpers with
the expected sequence in ``fixtures/early-clock.trace``. ``R`` (read), ``W``
(write) and ``B`` lines hold an address and a value in hexadecimal.

Run them with::

   west twister -p unit_testing -T tests/soc/qcom/sail
