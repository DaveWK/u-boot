# SPDX-License-Identifier: GPL-2.0+
#
# Build the SpacemiT BootROM-loadable FSBL.bin and the per-flash
# bootinfo_*.bin headers from the SPL output. Lifted from the
# vendor U-Boot tree (board/ky/x1/config.mk).
#
# build_binary_file.py resolves the inputs a JSON descriptor names (the
# SPL as ../u-boot-spl.bin, the key/ directory) relative to the
# descriptor. The descriptors are therefore staged with the SPL in a
# scratch directory of the object tree, so out-of-tree builds work and
# the source tree is never written. The images land in the object tree.

K1_FSBL_DIR := spacemit-fsbl
K1_FSBL_SRC := $(srctree)/board/$(CONFIG_SYS_VENDOR)/$(CONFIG_SYS_BOARD)/configs
K1_BOOTINFO := sd emmc spinor spinand

quiet_cmd_build_spl_platform = BUILD   $2
cmd_build_spl_platform = \
	rm -rf $(K1_FSBL_DIR) && mkdir -p $(K1_FSBL_DIR) && \
	cp -r $(K1_FSBL_SRC) $(K1_FSBL_DIR)/configs && \
	cp $3 $(K1_FSBL_DIR)/u-boot-spl.bin && \
	python3 $(srctree)/tools/build_binary_file.py \
		-c $(K1_FSBL_DIR)/configs/fsbl.json -o $2 && \
	$(foreach b,$(K1_BOOTINFO),python3 $(srctree)/tools/build_binary_file.py \
		-c $(K1_FSBL_DIR)/configs/bootinfo_$(b).json \
		-o bootinfo_$(b).bin && ) \
	rm -rf $(K1_FSBL_DIR)

MRPROPER_FILES += FSBL.bin $(foreach b,$(K1_BOOTINFO),bootinfo_$(b).bin)

# With the in-tree LPDDR4X driver the FSBL wraps the SPL alone, and is
# made in the SPL build. Without it the SPL needs SpacemiT's DDR training
# firmware appended, which binman does in the top-level build
# (u-boot-spl-ddr.bin, see the board -u-boot.dtsi), so the FSBL is made
# there, after binman.
ifneq ($(CONFIG_SPL_BUILD),)
ifeq ($(CONFIG_SPACEMIT_K1_DDR),y)
INPUTS-y += FSBL.bin

FSBL.bin: spl/u-boot-spl.bin FORCE
	$(call if_changed,build_spl_platform,$@,$<)
endif
else ifeq ($(CONFIG_SPL)$(CONFIG_SPACEMIT_K1_DDR),y)
all: FSBL.bin

FSBL.bin: .binman_stamp FORCE
	$(call if_changed,build_spl_platform,$@,u-boot-spl-ddr.bin)
endif
