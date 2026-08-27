# SPDX-License-Identifier: GPL-2.0+

ifdef CONFIG_SPL_BUILD
obj-$(CONFIG_SPACEMIT_K1_DDR) += ddr_init.o lpddr4_silicon_init.o ddr_freq.o
obj-$(CONFIG_SPACEMIT_K1_DDR_QOS) += ddr_qos.o
else
obj-$(CONFIG_SPACEMIT_K1_DDR_FREQ_SWITCH) += ddr_freq.o
endif
