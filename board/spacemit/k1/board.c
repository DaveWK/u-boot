// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024, Kongyang Liu <seashell11234455@gmail.com>
 */

#include <asm/io.h>
#include <dm/ofnode.h>
#include <init.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <log.h>
#include <power/regulator.h>
#include <soc/spacemit/k1-syscon.h>

/* APMU EMAC0/1 clock, reset and interface control (k1-syscon.h offsets) */
#define EMAC_AXI_SINGLE_ID		BIT(13)	/* one AXI ID for the MAC's master */
#define EMAC_RGMII_TX_CLK_FROM_SOC	BIT(8)	/* RGMII: 0 = TX clk from RX clk */
#define EMAC_RMII_REF_CLK_FROM_SOC	BIT(3)	/* RMII only */
#define EMAC_PHY_SEL_RGMII		BIT(2)	/* 0 = RMII, 1 = RGMII */

/* PLL3 as the ROM leaves it: programmed for 3200 MHz, not powered */
#define PLL3_SWCR1_3200MHZ		0x0050dd67
#define PLL3_SWCR3_3200MHZ		0x43eaaaab
#define PLL3_SWCR3_PWR_ON		BIT(31)
#define PLL3_SWCR2_D2_EN		BIT(1)

/* APMU CPU_Cn_CLK_CTRL */
#define CPU_CLK_FC_REQ			BIT(12)
#define CPU_CLK_HI_SRC_PLL3_D1		BIT(13)	/* 0 = pll3_d2 */
#define CPU_CLK_SRC_MASK		GENMASK(2, 0)
#define CPU_CLK_SRC_HI			7	/* cpu_cN_hi_clk */

#define VDD_CORE_1P6GHZ_UV		1050000

static void __iomem *k1_syscon_base(const char *compat)
{
	ofnode node = ofnode_by_compatible(ofnode_null(), compat);
	fdt_addr_t addr;

	if (!ofnode_valid(node))
		return NULL;
	addr = ofnode_get_addr(node);
	if (addr == FDT_ADDR_T_NONE)
		return NULL;
	return (void __iomem *)addr;
}

/*
 * Leave every enabled EMAC's APMU control word in the state its PHY mode
 * needs.  Only the clock gate and reset bits of this word are under a
 * driver's control through the clock and reset frameworks; the interface
 * select and clock-source bits are not, and their reset default is RMII.
 * An OS whose MAC driver expects the bootloader to have chosen the
 * interface (FreeBSD's smte did until it was taught otherwise) then finds
 * the PHY unclocked and unreachable on MDIO.
 *
 * For RGMII the TX clock comes from the PHY's RX clock (bit 8 clear): with
 * "TX clock from the SoC" the MAC's transmit side and its statistics block
 * have no running clock on the OrangePi R2S, the TX ring stalls after ten
 * frames and nothing reaches the wire.  Bit 3 is the RMII equivalent and
 * is cleared with it.  The single AXI ID is what both the vendor and the
 * FreeBSD driver run with.
 */
static void k1_emac_handoff(void)
{
	ofnode node;

	ofnode_for_each_compatible_node(node, "spacemit,k1-emac") {
		struct ofnode_phandle_args args;
		const char *mode;
		void __iomem *reg;
		u32 val;

		if (!ofnode_is_enabled(node))
			continue;
		mode = ofnode_read_string(node, "phy-mode");
		if (!mode)
			continue;
		if (ofnode_parse_phandle_with_args(node, "spacemit,apmu", NULL,
						   1, 0, &args))
			continue;
		reg = (void __iomem *)ofnode_get_addr(args.node);
		if (reg == (void __iomem *)FDT_ADDR_T_NONE)
			continue;
		reg += args.args[0];

		val = readl(reg);
		val |= EMAC_AXI_SINGLE_ID;
		val &= ~(EMAC_RGMII_TX_CLK_FROM_SOC | EMAC_RMII_REF_CLK_FROM_SOC);
		if (!strncmp(mode, "rgmii", 5))
			val |= EMAC_PHY_SEL_RGMII;
		else
			val &= ~EMAC_PHY_SEL_RGMII;
		writel(val, reg);
		log_debug("%s: %s, apmu+0x%x = 0x%08x\n", ofnode_get_name(node),
			  mode, args.args[0], readl(reg));
	}
}

static int k1_cpu_cluster_switch(void __iomem *ctrl)
{
	u32 val;

	val = readl(ctrl);
	val &= ~(CPU_CLK_SRC_MASK | CPU_CLK_HI_SRC_PLL3_D1);
	val |= CPU_CLK_SRC_HI;
	writel(val, ctrl);
	writel(val | CPU_CLK_FC_REQ, ctrl);

	return readl_poll_timeout(ctrl, val, !(val & CPU_CLK_FC_REQ), 10000);
}

/*
 * Bring both CPU clusters to 1.6 GHz.
 *
 * Out of the ROM the clusters run from pll1_d4_614p4 (about 300 MHz at the
 * core after the fabric dividers), PLL3 is programmed for 3200 MHz but not
 * powered, and the core rail sits at 0.90 V.  1.6 GHz is PLL3/2 through
 * the cluster mux, and the vendor operating point for it is 1.05 V.
 *
 * The rail is raised first and the switch is skipped if that is not
 * possible (no vdd_core regulator described, or its readback disagrees):
 * changing the clock at 0.90 V hangs the SoC immediately.  Every step
 * checks the state it expects and stops without touching the clock if the
 * registers are not as the ROM leaves them, so an unfamiliar PLL setting
 * is left alone rather than reprogrammed blind.
 */
static void k1_cpu_1p6ghz(void)
{
	void __iomem *apbs, *mpmu, *apmu;
	struct udevice *vdd_core;
	u32 swcr1, swcr3, val;
	int ret;

	apbs = k1_syscon_base("spacemit,k1-pll");
	mpmu = k1_syscon_base("spacemit,k1-syscon-mpmu");
	apmu = k1_syscon_base("spacemit,k1-syscon-apmu");
	if (!apbs || !mpmu || !apmu)
		return;

	if ((readl(apmu + APMU_CPU_C0_CLK_CTRL) & CPU_CLK_SRC_MASK) ==
	    CPU_CLK_SRC_HI) {
		log_info("CPU: clusters already on PLL3\n");
		return;
	}

	swcr1 = readl(apbs + APBS_PLL3_SWCR1);
	swcr3 = readl(apbs + APBS_PLL3_SWCR3);
	if (swcr1 != PLL3_SWCR1_3200MHZ ||
	    (swcr3 & ~PLL3_SWCR3_PWR_ON) != PLL3_SWCR3_3200MHZ) {
		log_warning("CPU: PLL3 not at the expected 3200 MHz setting (0x%08x/0x%08x), leaving the clock alone\n",
			    swcr1, swcr3);
		return;
	}

	ret = regulator_get_by_platname("vdd_core", &vdd_core);
	if (ret) {
		log_info("CPU: no vdd_core regulator (%d), staying on the boot clock\n",
			 ret);
		return;
	}
	ret = regulator_set_value(vdd_core, VDD_CORE_1P6GHZ_UV);
	if (ret) {
		log_warning("CPU: cannot set vdd_core (%d), staying on the boot clock\n",
			    ret);
		return;
	}
	ret = regulator_get_value(vdd_core);
	if (ret != VDD_CORE_1P6GHZ_UV) {
		log_warning("CPU: vdd_core reads %d uV, not %d, staying on the boot clock\n",
			    ret, VDD_CORE_1P6GHZ_UV);
		return;
	}
	/* rail settle: the ramp is 5 mV/us, the PMIC readback is only the selector */
	mdelay(2);

	writel(PLL3_SWCR2_D2_EN, apbs + APBS_PLL3_SWCR2);
	writel(swcr3 | PLL3_SWCR3_PWR_ON, apbs + APBS_PLL3_SWCR3);
	ret = readl_poll_timeout(mpmu + MPMU_POSR, val, val & POSR_PLL3_LOCK,
				 100000);
	if (ret) {
		log_warning("CPU: PLL3 did not lock, staying on the boot clock\n");
		return;
	}

	if (k1_cpu_cluster_switch(apmu + APMU_CPU_C0_CLK_CTRL) ||
	    k1_cpu_cluster_switch(apmu + APMU_CPU_C1_CLK_CTRL)) {
		log_warning("CPU: cluster frequency change did not complete\n");
		return;
	}

	log_info("CPU: clusters at 1.6 GHz (PLL3/2), vdd_core %d uV\n",
		 VDD_CORE_1P6GHZ_UV);
}

int board_init(void)
{
	k1_emac_handoff();
	k1_cpu_1p6ghz();

	return 0;
}
