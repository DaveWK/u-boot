// SPDX-License-Identifier: GPL-2.0
/*
 * SpacemiT K1 glue for the DWC3 USB 3 controller
 *
 * The K1 describes its DWC3 as one flat node ("spacemit,k1-dwc3") holding
 * the controller registers, its USB30 clock and resets and both PHYs, with
 * no glue registers of its own. The generic glue code does the rest: it
 * enables the clock, releases the resets and powers on the USB 3 PHY, then
 * binds the dwc3-generic host driver to the same node, which initialises
 * the USB 2 PHY, the core, the vbus-supply and xHCI.
 */

#include <dm.h>

#include "dwc3-generic.h"

static int dwc3_spacemit_get_ctrl_dev(struct udevice *dev, ofnode *node)
{
	*node = dev_ofnode(dev);

	return ofnode_valid(*node) ? 0 : -EINVAL;
}

static const struct dwc3_glue_ops dwc3_spacemit_k1_ops = {
	.glue_get_ctrl_dev = dwc3_spacemit_get_ctrl_dev,
};

static const struct udevice_id dwc3_spacemit_ids[] = {
	{ .compatible = "spacemit,k1-dwc3", .data = (ulong)&dwc3_spacemit_k1_ops },
	{ }
};

U_BOOT_DRIVER(dwc3_spacemit_wrapper) = {
	.name		= "dwc3-spacemit",
	.id		= UCLASS_NOP,
	.of_match	= dwc3_spacemit_ids,
	.bind		= dwc3_glue_bind,
	.probe		= dwc3_glue_probe,
	.remove		= dwc3_glue_remove,
	.plat_auto	= sizeof(struct dwc3_glue_data),
};
