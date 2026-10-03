.. SPDX-License-Identifier: GPL-2.0-or-later

OrangePi R2S and RV2
===================

These boards reuse ``spacemit_k1_defconfig`` and the Linux device trees.
The SPL matches factory TLV product names ``x1_orangepi-r2s`` and
``x1_orangepi-rv2`` to their FIT device-tree configurations.

Build profiles
--------------

Use the K1 SPL guide to obtain OpenSBI and SpacemiT DDR firmware. Select
the SPL control tree explicitly for each initial board profile::

   make O=build-r2s spacemit_k1_defconfig
   make O=build-r2s CROSS_COMPILE=riscv64-linux-gnu- \
       DEVICE_TREE=spacemit/k1-orangepi-r2s OPENSBI=/path/fw_dynamic.bin \
       BINMAN_INDIRS=/path/to/ddr-firmware

For RV2, use another output directory and
``DEVICE_TREE=spacemit/k1-orangepi-rv2``. Both FITs include Banana Pi F3,
MusePi Pro, OrangePi R2S and OrangePi RV2 configurations. The default DT
and unknown-name fallback remain MusePi Pro.

SPL uses its build-selected control DT before DDR training. Therefore
the multi-board FIT does not imply one validated SPL for all boards.
Keep the board profiles separate until that path is tested.

Initial scope and dependencies
------------------------------

The initial DDR configuration is single-rank, 2400 MT/s, for the 2 GiB
R2S and 4 GiB RV2 units. Retain the DDR-from-DT and cs-num workarounds
for this initial series. An 8 GiB dual-rank RV2 needs separate validation
and must not use this single-rank configuration as a tested profile.
DDR-workaround removal is follow-up work after initial board support.

The series depends on the K1 storage, combo-PHY, PCIe and SPL fixes,
generic K1 USB support, and Linux EEPROM/QSPI DT prerequisites. The
EEPROM is read-only in Linux; factory EEPROM writes were not tested.

The first bootable filesystem partition setting finds the existing
R2S ESP at partition 1 and RV2 ESP at partition 3. Keep their existing
bootinfo/FSBL layouts. Follow the K1 SPL guide for signed FSBL packaging;
do not write the unsigned SPL image directly into BootROM storage.

Validation state
----------------

Revised common-config profiles require fresh hardware validation.
Historical results from board-specific lane configs do not validate
the revised series. Device-tree and build checks are recorded separately
from boot, DDR size, network, USB and storage acceptance.
