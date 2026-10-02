// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024, Kongyang Liu <seashell11234455@gmail.com>
 */

#include <clk.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <dm/uclass.h>
#include <init.h>
#include <log.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <power/regulator.h>

#define K1_NUM_CLUSTERS		2

/*
 * The P1 PMIC reads back the voltage selector it was given, not its output,
 * and moving the clusters onto a faster clock while the core rail is still
 * low hangs the SoC. Give a raised rail this long on top of its declared ramp
 * before the first switch.
 */
#define K1_CPU_SUPPLY_SETTLE_US	2000

struct k1_cluster {
	struct udevice *cpu;		/* first CPU of the cluster */
	struct clk clk;
	struct udevice *supply;
	ulong hz;
	int uv;
};

static const char *k1_supply_name(struct udevice *supply)
{
	struct dm_regulator_uclass_plat *plat = dev_get_uclass_plat(supply);

	return plat->name;
}

/* The fastest operating point of @table that @supply may provide */
static int k1_cpu_find_opp(ofnode table, struct udevice *supply,
			   ulong *hz, int *uv)
{
	struct dm_regulator_uclass_plat *plat = dev_get_uclass_plat(supply);
	u64 best = 0;
	ofnode opp;

	if (!ofnode_device_is_compatible(table, "operating-points-v2"))
		return -EINVAL;

	ofnode_for_each_subnode(opp, table) {
		u64 rate;
		u32 volt;

		if (!ofnode_is_enabled(opp) ||
		    ofnode_read_bool(opp, "turbo-mode"))
			continue;
		if (ofnode_read_u64(opp, "opp-hz", &rate) ||
		    ofnode_read_u32_index(opp, "opp-microvolt", 0, &volt))
			continue;
		if (plat->max_uV != -ENODATA && volt > plat->max_uV)
			continue;
		if (rate <= best)
			continue;

		best = rate;
		*uv = volt;
	}
	if (!best)
		return -ENOENT;

	*hz = best;

	return 0;
}

/*
 * One entry per distinct CPU clock, each with the operating point it is to
 * run at and the supply that point needs.
 */
static int k1_cpu_get_clusters(struct k1_cluster *cl, int *num)
{
	struct udevice *cpu;
	struct uclass *uc;
	int n = 0, i, ret;

	uclass_id_foreach_dev(UCLASS_CPU, cpu, uc) {
		struct k1_cluster *c;
		struct clk clk;
		ofnode table;

		ret = clk_get_by_index(cpu, 0, &clk);
		if (ret) {
			log_info("CPU: no clock for %s (%d), staying on the boot clock\n",
				 cpu->name, ret);
			return ret;
		}
		for (i = 0; i < n; i++)
			if (cl[i].clk.dev == clk.dev && cl[i].clk.id == clk.id)
				break;
		if (i < n)
			continue;
		if (n == K1_NUM_CLUSTERS)
			return -E2BIG;

		c = &cl[n];
		c->cpu = cpu;
		c->clk = clk;

		ret = device_get_supply_regulator(cpu, "cpu-supply",
						  &c->supply);
		if (ret) {
			log_info("CPU: no cpu-supply for %s (%d), staying on the boot clock\n",
				 cpu->name, ret);
			return ret;
		}

		table = ofnode_parse_phandle(dev_ofnode(cpu),
					     "operating-points-v2", 0);
		ret = k1_cpu_find_opp(table, c->supply, &c->hz, &c->uv);
		if (ret) {
			log_info("CPU: no operating point for %s (%d), staying on the boot clock\n",
				 cpu->name, ret);
			return ret;
		}
		n++;
	}
	if (!n)
		return -ENODEV;

	*num = n;

	return 0;
}

/*
 * Raise every CPU supply to the highest voltage that the operating points
 * of the clusters on it need, and confirm it by reading it back. Nothing is
 * lowered.
 */
static int k1_cpu_raise_supplies(struct k1_cluster *cl, int num)
{
	bool raised = false;
	int i, j, ret;

	for (i = 0; i < num; i++) {
		struct udevice *supply = cl[i].supply;
		int uv = cl[i].uv;
		int cur;

		for (j = 0; j < num; j++)
			if (cl[j].supply == supply)
				uv = max(uv, cl[j].uv);

		cur = regulator_get_value(supply);
		if (cur >= uv)
			continue;

		ret = regulator_set_value(supply, uv);
		if (ret) {
			log_warning("CPU: cannot set %s to %d uV (%d), staying on the boot clock\n",
				    k1_supply_name(supply), uv, ret);
			return ret;
		}
		cur = regulator_get_value(supply);
		if (cur < uv) {
			log_warning("CPU: %s reads %d uV, not %d uV, staying on the boot clock\n",
				    k1_supply_name(supply), cur, uv);
			return -EIO;
		}
		raised = true;
	}
	if (raised)
		udelay(K1_CPU_SUPPLY_SETTLE_US);

	return 0;
}

/*
 * Run the CPU clusters at the fastest operating point the device tree gives
 * them. The BootROM leaves them on a slow PLL1 output with the core rail low
 * (PLL3, which the faster points run from, is not even powered); those points
 * need the rail raised first. No clock is touched unless every cluster has a
 * clock, an operating point and a supply that reads back at least the
 * voltage that point needs.
 */
static void k1_cpu_set_opp(void)
{
	struct k1_cluster cl[K1_NUM_CLUSTERS];
	int num, i;
	ulong ret;

	if (k1_cpu_get_clusters(cl, &num))
		return;
	if (k1_cpu_raise_supplies(cl, num))
		return;

	for (i = 0; i < num; i++) {
		ret = clk_set_rate(&cl[i].clk, cl[i].hz);
		if (IS_ERR_VALUE(ret)) {
			log_warning("CPU: cannot set the %s cluster clock to %lu Hz (%d)\n",
				    cl[i].cpu->name, cl[i].hz, (int)ret);
			continue;
		}
		log_info("CPU: cluster %d at %lu MHz, %s at %d uV\n", i,
			 clk_get_rate(&cl[i].clk) / 1000000,
			 k1_supply_name(cl[i].supply),
			 regulator_get_value(cl[i].supply));
	}
}

int board_init(void)
{
	k1_cpu_set_opp();

	return 0;
}
