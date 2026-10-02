// SPDX-License-Identifier: GPL-2.0+
/*
 * Spacemit K1 PCIe host controller driver
 *
 * Copyright (c) 2023, Spacemit Corporation.
 * Copyright (c) 2026, RISCstar Ltd.
 *
 */
#include <asm/global_data.h>
#include <asm/io.h>
#include <clk.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/ofnode.h>
#include <dm/read.h>
#include <generic-phy.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <log.h>
#include <pci.h>
#include <power-domain.h>
#include <power/regulator.h>
#include <reset.h>

#include "pcie_dw_common.h"
#include "pcie_dw_spacemit.h"

DECLARE_GLOBAL_DATA_PTR;

struct pcie_dw_spacemit {
	/* Must be first member of the struct */
	struct pcie_dw dw;
	void __iomem		*apmu_base;
	u32			apmu_offset;
	void __iomem		*phy_ahb;	/* DT "link" */
	int			port_id;

	/* reset, clock resources */
	struct clk_bulk		clks;
	struct reset_ctl_bulk	rsts;
	struct udevice		*vpcie3v3;	/* slot supply, if described */
	u32			max_link_speed;	/* 0: hardware default */
	u32			num_lanes;	/* 0: hardware default */
};

static inline u32 spacemit_pcie_readl(struct pcie_dw_spacemit *pcie, u32 offset)
{
	return readl(pcie->apmu_base + pcie->apmu_offset + offset);
}

static inline void spacemit_pcie_writel(struct pcie_dw_spacemit *pcie,
					u32 offset, u32 value)
{
	writel(value, pcie->apmu_base + pcie->apmu_offset + offset);
}

static inline u32 spacemit_pcie_phy_ahb_readl(struct pcie_dw_spacemit *pcie, u32 offset)
{
	return readl(pcie->phy_ahb + offset);
}

static void pcie_dw_configure(struct pcie_dw_spacemit *pci)
{
	void __iomem *cap;
	u32 speed = pci->max_link_speed;
	u16 lnkctl2;
	u32 lnkcap;

	cap = pci->dw.dbi_base + pcie_dw_find_capability(&pci->dw,
							 PCI_CAP_ID_EXP);

	dw_pcie_dbi_write_enable(&pci->dw, true);

	/*
	 * Do not advertise ASPM L1: as Linux's driver notes, some NVMe drives
	 * report errors with it.
	 */
	clrbits_le32(cap + PCI_EXP_LNKCAP, PCI_EXP_LNKCAP_ASPM_L1);

	/*
	 * Keep the hardware's supported speeds unless the device tree limits
	 * them with "max-link-speed", as Linux's DesignWare core does.
	 */
	if (speed) {
		lnkcap = readl(cap + PCI_EXP_LNKCAP);
		lnkcap &= ~PCI_EXP_LNKCAP_SLS;
		lnkcap |= speed;
		writel(lnkcap, cap + PCI_EXP_LNKCAP);

		lnkctl2 = readw(cap + PCI_EXP_LNKCTL2);
		lnkctl2 &= ~PCI_EXP_LNKCTL2_TLS;
		lnkctl2 |= speed;
		writew(lnkctl2, cap + PCI_EXP_LNKCTL2);
	}

	/* Link width from "num-lanes"; without it, keep the hardware's */
	dw_pcie_link_set_max_link_width(&pci->dw, pci->num_lanes);

	dw_pcie_dbi_write_enable(&pci->dw, false);
}

static int is_link_up(struct pcie_dw_spacemit *pci)
{
	u32 val;

	/*
	 * As in Linux, the link is up once both the physical layer (SMLH)
	 * and the data link layer (RDLH) report it.
	 */
	val = spacemit_pcie_phy_ahb_readl(pci, K1X_PHY_AHB_LINK_STS);

	return (val & SMLH_LINK_UP) && (val & RDLH_LINK_UP);
}

static int wait_link_up(struct pcie_dw_spacemit *pci)
{
	unsigned long timeout;

	timeout = get_timer(0) + PCIE_LINK_UP_TIMEOUT_MS;
	while (!is_link_up(pci)) {
		if (get_timer(0) > timeout) {
			dev_dbg(pci->dw.dev, "LTSSM link training timeout\n");
			return 0;
		}
	}

	return 1;
}

static int pcie_dw_spacemit_pcie_link_up(struct pcie_dw_spacemit *pci)
{
	u32 reg;

	/* DW pre link configurations */
	pcie_dw_configure(pci);

	/* Initiate link training */
	reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg |= LTSSM_EN;
	reg &= ~APP_HOLD_PHY_RST;
	spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

	/* Check that link was established */
	if (!wait_link_up(pci))
		return 0;

	/*
	 * Link can be established in Gen 1, still need to wait
	 * until MAC negotiation is completed
	 */
	udelay(100);

	return 1;
}

/*
 * Reset the controller, so that nothing left over from an earlier boot
 * stage or a warm reboot (a trained link, a stale LTSSM state) survives.
 */
static void spacemit_pcie_toggle_soft_reset(struct pcie_dw_spacemit *pci)
{
	u32 reg;

	reg = spacemit_pcie_readl(pci, PCIE_CTRL_LOGIC);
	spacemit_pcie_writel(pci, PCIE_CTRL_LOGIC, reg | PCIE_SOFT_RESET);
	/* Read back so that the write has landed before the delay */
	spacemit_pcie_readl(pci, PCIE_CTRL_LOGIC);
	mdelay(2);
	spacemit_pcie_writel(pci, PCIE_CTRL_LOGIC, reg & ~PCIE_SOFT_RESET);
}

static int pcie_set_mode(struct pcie_dw_spacemit *pci,
			 enum dw_pcie_device_mode mode)
{
	u32 reg;

	switch (mode) {
	case DW_PCIE_RC_TYPE:
		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg |= DEVICE_TYPE_RC | PCIE_AUX_PWR_DET;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		reg = spacemit_pcie_readl(pci, PCIE_CTRL_LOGIC);
		reg |= PCIE_IGNORE_PERSTN;
		spacemit_pcie_writel(pci, PCIE_CTRL_LOGIC, reg);
		break;
	case DW_PCIE_EP_TYPE:
		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg &= ~DEVICE_TYPE_RC;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);
		break;
	default:
		dev_err(pci->dw.dev, "INVALID device type %d\n", mode);
		return -EINVAL;
	}

	return 0;
}

static int spacemit_pcie_host_init(struct pcie_dw_spacemit *pci)
{
	u32 reg;

	mdelay(100);
	/* set Perst# gpio high state*/
	reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg &= ~PCIE_RC_PERST;
	spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

	return 0;
}

static int pcie_dw_init_id(struct pcie_dw_spacemit *pci)
{
	dw_pcie_dbi_write_enable(&pci->dw, true);
	writew(SPACEMIT_PCIE_VENDOR_ID, pci->dw.dbi_base + PCI_VENDOR_ID);
	writew(SPACEMIT_PCIE_DEVICE_ID, pci->dw.dbi_base + PCI_DEVICE_ID);
	dw_pcie_dbi_write_enable(&pci->dw, false);

	return 0;
}

/*
 * The binding describes the 3.3 V slot supply on the root port node. Some
 * board device trees also, or only, put it on the host bridge node, which
 * is where Linux's driver looks for it.
 */
static int spacemit_pcie_get_vpcie3v3(struct udevice *dev, ofnode port,
				      struct udevice **supplyp)
{
	ofnode supply;

	supply = ofnode_parse_phandle(port, "vpcie3v3-supply", 0);
	if (!ofnode_valid(supply))
		supply = ofnode_parse_phandle(dev_ofnode(dev),
					      "vpcie3v3-supply", 0);
	if (!ofnode_valid(supply)) {
		*supplyp = NULL;
		return 0;
	}

	return uclass_get_device_by_ofnode(UCLASS_REGULATOR, supply, supplyp);
}

static int pcie_dw_spacemit_probe(struct udevice *dev)
{
	struct pcie_dw_spacemit *pci = dev_get_priv(dev);
	struct udevice *ctlr = pci_get_controller(dev);
	struct pci_controller *hose = dev_get_uclass_priv(ctlr);
	struct phy phy0 = {0};
	ofnode port;
	int ret;
	u32 reg;

	spacemit_pcie_toggle_soft_reset(pci);

	/* enable pcie clk and deassert resets */
	ret = clk_enable_bulk(&pci->clks);
	if (ret) {
		dev_err(dev, "failed to enable clocks: %d\n", ret);
		return ret;
	}

	ret = reset_deassert_bulk(&pci->rsts);
	if (ret) {
		dev_err(dev, "failed to deassert resets: %d\n", ret);
		clk_disable_bulk(&pci->clks);
		return ret;
	}

	reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg &= ~LTSSM_EN;
	spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

	/* set Perst# (fundamental reset) gpio low state*/
	reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg |= PCIE_RC_PERST;
	spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

	/* The PHY is described on the root port child node */
	port = ofnode_first_subnode(dev_ofnode(dev));
	if (!ofnode_valid(port)) {
		dev_err(dev, "failed to find PCIe port node\n");
		return -ENODEV;
	}

	ret = spacemit_pcie_get_vpcie3v3(dev, port, &pci->vpcie3v3);
	if (ret) {
		/* e.g. an always-on fixed supply with no driver built in */
		dev_warn(dev, "vpcie3v3 supply not available: %d\n", ret);
		pci->vpcie3v3 = NULL;
	}

	ret = generic_phy_get_by_index_nodev(port, 0, &phy0);
	if (!ret) {
		/*
		 * Hold the PHY interface in reset through PHY init; it is
		 * released together with LTSSM_EN when link training starts.
		 */
		reg = spacemit_pcie_readl(pci, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg |= DEVICE_TYPE_RC | APP_HOLD_PHY_RST | PCIE_PHY_LANE_CTRL_MASK;
		reg &= ~GLOBAL_PHY_RST;
		spacemit_pcie_writel(pci, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		ret = generic_phy_init(&phy0);
		if (ret) {
			dev_err(dev, "failed to init PHY: %d\n", ret);
			return ret;
		}

		ret = generic_phy_power_on(&phy0);
		if (ret) {
			dev_err(dev, "failed to power on PHY: %d\n", ret);
			return ret;
		}
	} else {
		dev_err(dev, "failed to get pcie-phy: %d\n", ret);
		return ret;
	}

	pci->dw.first_busno = dev_seq(dev);
	pci->dw.dev = dev;

	pcie_set_mode(pci, DW_PCIE_RC_TYPE);

	/* Power the slot; PERST# stays asserted for 100 ms after this */
	if (pci->vpcie3v3) {
		ret = regulator_set_enable_if_allowed(pci->vpcie3v3, true);
		if (ret) {
			dev_err(dev, "failed to enable vpcie3v3 supply: %d\n",
				ret);
			return ret;
		}
	}

	spacemit_pcie_host_init(pci);
	pcie_dw_setup_host(&pci->dw);
	pcie_dw_init_id(pci);

	/*
	 * An empty slot, or a device that does not train, is not an error:
	 * the root port is still usable and the config accessors keep
	 * requests for the buses behind it off the wire.
	 */
	if (!pcie_dw_spacemit_pcie_link_up(pci))
		printf("PCIE-%d: Link down\n", dev_seq(dev));
	else
		printf("PCIE-%d: Link up (Gen%d-x%d, Bus%d)\n", dev_seq(dev),
		       pcie_dw_get_link_speed(&pci->dw),
		       pcie_dw_get_link_width(&pci->dw),
		       hose->first_busno);

	ret = pcie_dw_prog_outbound_atu_unroll(&pci->dw, PCIE_ATU_REGION_INDEX0,
					       PCIE_ATU_TYPE_MEM,
					       pci->dw.mem.phys_start,
					       pci->dw.mem.bus_start,
					       pci->dw.mem.size);
	if (ret)
		return -EIO;

	return 0;
}

static int pcie_dw_spacemit_of_to_plat(struct udevice *dev)
{
	int ret = 0;
	struct pcie_dw_spacemit *pcie = dev_get_priv(dev);
	struct ofnode_phandle_args args;
	fdt_addr_t dbi_addr;

	/* Get the controller base address */
	pcie->dw.dbi_base = (void *)dev_read_addr_name(dev, "dbi");
	if ((fdt_addr_t)pcie->dw.dbi_base == FDT_ADDR_T_NONE)
		return -EINVAL;

	dbi_addr = (fdt_addr_t)pcie->dw.dbi_base;

	/* Get the config space base address and size */
	pcie->dw.cfg_base = (void *)dev_read_addr_size_name(dev, "config",
							 &pcie->dw.cfg_size);
	if ((fdt_addr_t)pcie->dw.cfg_base == FDT_ADDR_T_NONE)
		return -EINVAL;

	/* Get the iATU base address and size */
	pcie->dw.atu_base = (void *)dev_read_addr_name(dev, "atu");
	if ((fdt_addr_t)pcie->dw.atu_base == FDT_ADDR_T_NONE)
		return -EINVAL;

	/* Get the PHY AHB base address (called "link" in Linux DTS) */
	pcie->phy_ahb = (void *)dev_read_addr_name(dev, "link");
	if ((fdt_addr_t)pcie->phy_ahb == FDT_ADDR_T_NONE) {
		pcie->phy_ahb = (void *)dev_read_addr_name(dev, "phy_ahb");
		if ((fdt_addr_t)pcie->phy_ahb == FDT_ADDR_T_NONE)
			return -EINVAL;
	}

	/* Get APMU regmap from spacemit,apmu property */
	ret = dev_read_phandle_with_args(dev, "spacemit,apmu", NULL, 1, 0,
					 &args);
	if (ret)
		return ret;

	pcie->apmu_base = (void __iomem *)ofnode_get_addr(args.node);
	if (!pcie->apmu_base)
		return -EINVAL;
	pcie->apmu_offset = args.args[0];

	pcie->max_link_speed = dev_read_u32_default(dev, "max-link-speed", 0);
	if (pcie->max_link_speed > LINK_SPEED_GEN_4) {
		dev_warn(dev, "invalid max-link-speed %u, ignored\n",
			 pcie->max_link_speed);
		pcie->max_link_speed = 0;
	}

	pcie->num_lanes = dev_read_u32_default(dev, "num-lanes", 0);

	/* Derive port ID from DBI base address */
	pcie->port_id = (dbi_addr - SPACEMIT_PCIE_DBI_BASE) / SPACEMIT_PCIE_DBI_STRIDE;

	ret = clk_get_bulk(dev, &pcie->clks);
	if (ret) {
		dev_warn(dev, "failed to get clocks: %d\n", ret);
		return ret;
	}

	ret = reset_get_bulk(dev, &pcie->rsts);
	if (ret) {
		dev_warn(dev, "failed to get resets: %d\n", ret);
		return ret;
	}

	return 0;
}

/*
 * Nothing answers behind the root port while the link is down, so do not
 * send config requests there; Linux's DesignWare core does the same.
 */
static int pcie_dw_spacemit_read_config(const struct udevice *bus,
					pci_dev_t bdf, uint offset,
					ulong *valuep, enum pci_size_t size)
{
	struct pcie_dw_spacemit *pci = dev_get_priv(bus);

	if (PCI_BUS(bdf) != pci->dw.first_busno && !is_link_up(pci)) {
		*valuep = pci_get_ff(size);
		return 0;
	}

	return pcie_dw_read_config(bus, bdf, offset, valuep, size);
}

static int pcie_dw_spacemit_write_config(struct udevice *bus, pci_dev_t bdf,
					 uint offset, ulong value,
					 enum pci_size_t size)
{
	struct pcie_dw_spacemit *pci = dev_get_priv(bus);

	if (PCI_BUS(bdf) != pci->dw.first_busno && !is_link_up(pci))
		return 0;

	return pcie_dw_write_config(bus, bdf, offset, value, size);
}

static const struct dm_pci_ops pcie_dw_spacemit_ops = {
	.read_config	= pcie_dw_spacemit_read_config,
	.write_config	= pcie_dw_spacemit_write_config,
};

static const struct udevice_id pcie_dw_spacemit_ids[] = {
	{ .compatible = "spacemit,k1-pcie" },
	{ }
};

U_BOOT_DRIVER(pcie_dw_spacemit) = {
	.name			= "pcie_dw_spacemit",
	.id			= UCLASS_PCI,
	.of_match		= pcie_dw_spacemit_ids,
	.ops			= &pcie_dw_spacemit_ops,
	.of_to_plat	= pcie_dw_spacemit_of_to_plat,
	.probe			= pcie_dw_spacemit_probe,
	.priv_auto	= sizeof(struct pcie_dw_spacemit),
};
