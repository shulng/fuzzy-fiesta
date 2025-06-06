// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 */

#include <linux/cpu.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_opp.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include <dt-bindings/clock/qcom,apsscc-sdxlemur.h>

#include "clk-alpha-pll.h"
#include "clk-debug.h"
#include "clk-rcg.h"
#include "clk-regmap-divider.h"
#include "clk-regmap-mux.h"
#include "clk-regmap-mux-div.h"
#include "common.h"
#include "vdd-level.h"

#include "clk-pll.h"
#include "clk-regmap.h"
#include "reset.h"

#define to_clk_regmap_mux_div(_hw) \
	container_of(to_clk_regmap(_hw), struct clk_regmap_mux_div, clkr)

static DEFINE_VDD_REGULATORS(vdd_pll, VDD_NUM, 1, vdd_corner);
static DEFINE_VDD_REGS_INIT(vdd_cpu, 1);

enum apcs_mux_clk_parent {
	P_BI_TCXO,
	P_GPLL0,
	P_APCS_CPU_PLL,
};

static int cpucc_clk_set_rate_and_parent(struct clk_hw *hw, unsigned long rate,
						unsigned long prate, u8 index)
{
	struct clk_regmap_mux_div *cpuclk = to_clk_regmap_mux_div(hw);

	return mux_div_set_src_div(cpuclk, cpuclk->parent_map[index].cfg,
					cpuclk->div);
}

static int cpucc_clk_set_parent(struct clk_hw *hw, u8 index)
{
	/*
	 * Since cpucc_clk_set_rate_and_parent() is defined and set_parent()
	 * will never gets called from clk_change_rate() so return 0.
	 */
	return 0;
}

static int cpucc_clk_set_rate(struct clk_hw *hw, unsigned long rate,
						unsigned long prate)
{
	struct clk_regmap_mux_div *cpuclk = to_clk_regmap_mux_div(hw);

	/*
	 * Parent is same as the last rate.
	 * Here just configure new div.
	 */
	return mux_div_set_src_div(cpuclk, cpuclk->src, cpuclk->div);
}

static unsigned long
cpucc_calc_rate(unsigned long rate, u32 m, u32 n, u32 mode, u32 hid_div)
{
	u64 tmp = rate;

	if (hid_div) {
		tmp *= 2;
		do_div(tmp, hid_div + 1);
	}

	if (mode) {
		tmp *= m;
		do_div(tmp, n);
	}

	return tmp;
}

static int cpucc_clk_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	struct clk_hw *apcs_cpu_pll_hw;
	struct clk_rate_request parent_req = { };
	struct clk_regmap_mux_div *cpuclk = to_clk_regmap_mux_div(hw);
	int ret;

	apcs_cpu_pll_hw = clk_hw_get_parent_by_index(hw,
						       P_APCS_CPU_PLL);
	parent_req.rate = req->rate;
	parent_req.best_parent_hw = apcs_cpu_pll_hw;
	req->best_parent_hw = apcs_cpu_pll_hw;
	ret = __clk_determine_rate(req->best_parent_hw, &parent_req);
	if (ret)
		return ret;

	req->best_parent_rate = parent_req.rate;
	cpuclk->src = cpuclk->parent_map[P_APCS_CPU_PLL].cfg;
	cpuclk->div = 1;

	return 0;
}

static void cpucc_clk_list_registers(struct seq_file *f, struct clk_hw *hw)
{
	struct clk_regmap_mux_div *cpuclk = to_clk_regmap_mux_div(hw);
	int i = 0, size = 0, val;

	static struct clk_register_data data[] = {
		{"CMD_RCGR", 0x0},
		{"CFG_RCGR", 0x4},
	};

	size = ARRAY_SIZE(data);
	for (i = 0; i < size; i++) {
		regmap_read(cpuclk->clkr.regmap,
				cpuclk->reg_offset + data[i].offset, &val);
		seq_printf(f, "%20s: 0x%.8x\n", data[i].name, val);
	}
}

static struct clk_regmap_ops clk_rcg2_regmap_ops = {
	.list_registers = cpucc_clk_list_registers,
};

static void clk_cpu_init(struct clk_hw *hw)
{
	struct clk_regmap *rclk = to_clk_regmap(hw);

	if (!rclk->ops)
		rclk->ops = &clk_rcg2_regmap_ops;
}

static unsigned long cpucc_clk_recalc_rate(struct clk_hw *hw,
					unsigned long prate)
{
	struct clk_regmap_mux_div *cpuclk = to_clk_regmap_mux_div(hw);
	struct clk_hw *parent;
	const char *name = clk_hw_get_name(hw);
	unsigned long parent_rate;
	u32 i, div, src = 0;
	u32 num_parents = clk_hw_get_num_parents(hw);
	int ret;

	ret = mux_div_get_src_div(cpuclk, &src, &div);
	if (ret)
		return ret;

	cpuclk->src = src;
	cpuclk->div = div;

	for (i = 0; i < num_parents; i++) {
		if (src == cpuclk->parent_map[i].cfg) {
			parent = clk_hw_get_parent_by_index(hw, i);
			parent_rate = clk_hw_get_rate(parent);
			return cpucc_calc_rate(parent_rate, 0, 0, 0, div);
		}
	}
	pr_err("%s: Can't find parent %d\n", name, src);

	return 0;
}

static int cpucc_clk_enable(struct clk_hw *hw)
{
	return clk_regmap_mux_div_ops.enable(hw);
}

static void cpucc_clk_disable(struct clk_hw *hw)
{
	clk_regmap_mux_div_ops.disable(hw);
}

static u8 cpucc_clk_get_parent(struct clk_hw *hw)
{
	return clk_regmap_mux_div_ops.get_parent(hw);
}

static const struct clk_ops cpucc_clk_ops = {
	.enable = cpucc_clk_enable,
	.disable = cpucc_clk_disable,
	.get_parent = cpucc_clk_get_parent,
	.set_rate = cpucc_clk_set_rate,
	.set_parent = cpucc_clk_set_parent,
	.set_rate_and_parent = cpucc_clk_set_rate_and_parent,
	.determine_rate = cpucc_clk_determine_rate,
	.recalc_rate = cpucc_clk_recalc_rate,
	.debug_init = clk_common_debug_init,
	.init = clk_cpu_init,
};

static struct pll_vco lucid_5lpe_vco[] = {
	{ 249600000, 2000000000, 0 },
};

/* Initial configuration for 1094.4 */
static const struct alpha_pll_config apcs_cpu_pll_config = {
	.l = 0x4E,
	.cal_l = 0x4E,
	.alpha = 0x0,
	.config_ctl_val = 0x2A9A699C,
	.config_ctl_hi_val = 0x00002261,
	.config_ctl_hi1_val = 0x20485699,
	.test_ctl_val = 0x00000000,
	.test_ctl_hi_val = 0x00000000,
	.test_ctl_hi1_val = 0x01800000,
	.user_ctl_val = 0x00000001,
	.user_ctl_hi_val = 0x00000805,
	.user_ctl_hi1_val = 0x00000000,
};

static struct clk_alpha_pll apcs_cpu_pll = {
	.offset = 0x0,
	.vco_table = lucid_5lpe_vco,
	.num_vco = ARRAY_SIZE(lucid_5lpe_vco),
	.regs = clk_alpha_pll_regs[CLK_ALPHA_PLL_TYPE_LUCID_5LPE],
	.clkr = {
		.hw.init = &(struct clk_init_data){
			.name = "apcs_cpu_pll",
			.parent_data = &(const struct clk_parent_data){
				.fw_name = "bi_tcxo_ao",
			},
			.num_parents = 1,
			.ops = &clk_alpha_pll_lucid_5lpe_ops,
		},
		.vdd_data = {
			.vdd_class = &vdd_pll,
			.num_rate_max = VDD_NUM,
			.rate_max = (unsigned long[VDD_NUM]) {
				[VDD_MIN] = 615000000,
				[VDD_LOW] = 1066000000,
				[VDD_LOW_L1] = 1500000000,
				[VDD_NOMINAL] = 1750000000},
		},
	},
};

static const struct parent_map apcs_mux_clk_parent_map[] = {
	{ P_BI_TCXO, 0 },
	{ P_GPLL0, 1 },
	{ P_APCS_CPU_PLL, 5 },
};

static const struct clk_parent_data apss_cc_parent_data[] = {
	{ .fw_name = "bi_tcxo_ao" },
	{ .fw_name = "gpll0_out_even" },
	{ .hw = &apcs_cpu_pll.clkr.hw },
};

static struct clk_regmap_mux_div apcs_mux_clk = {
	.reg_offset = 0x0,
	.hid_width  = 5,
	.hid_shift  = 0,
	.src_width  = 3,
	.src_shift  = 8,
	.parent_map = apcs_mux_clk_parent_map,
	.clkr.hw.init = &(struct clk_init_data) {
		.name = "apcs_mux_clk",
		.parent_data = apss_cc_parent_data,
		.num_parents = 3,
		.flags = CLK_SET_RATE_PARENT,
		.ops = &cpucc_clk_ops,
	},
	.clkr.vdd_data = {
		.vdd_class = &vdd_cpu,
	},
};

static const struct of_device_id match_table[] = {
	{ .compatible = "qcom,sdxlemur-apsscc" },
	{}
};

static struct regmap_config cpu_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= 0x3C,
	.fast_io	= true,
};

static struct clk_hw *cpu_clks_hws[] = {
	[APCS_CPU_PLL] = &apcs_cpu_pll.clkr.hw,
	[APCS_MUX_CLK] = &apcs_mux_clk.clkr.hw,
};

static void cpucc_clk_get_speed_bin(struct platform_device *pdev, int *bin,
							int *version)
{
	struct resource *res;
	u32 pte_efuse, valid;
	void __iomem *base;

	*bin = 0;
	*version = 0;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "efuse");
	if (!res) {
		dev_info(&pdev->dev,
			"No speed/PVS binning available. Defaulting to 0!\n");
		return;
	}

	base = ioremap(res->start, resource_size(res));
	if (!base) {
		dev_info(&pdev->dev,
			"Unable to read efuse data. Defaulting to 0!\n");
		return;
	}

	pte_efuse = readl_relaxed(base);
	iounmap(base);

	*bin = pte_efuse & 0x7;
	valid = ((pte_efuse >> 3) & 0x1) ? ((pte_efuse >> 3) & 0x1) : 0;
	*version = (pte_efuse >> 4) & 0x3;

	dev_info(&pdev->dev, "PVS version: %d bin: %d\n", *version, *bin);
}

static int cpucc_clk_get_fmax_vdd_class(struct platform_device *pdev,
			struct clk_vdd_class_data *clk_intd, char *prop_name)
{
	struct device_node *of = pdev->dev.of_node;
	struct clk_vdd_class *vdd = clk_intd->vdd_class;
	int prop_len, i, j, ret;
	int num = vdd->num_regulators + 1;
	u32 *array;

	if (!of_find_property(of, prop_name, &prop_len)) {
		dev_err(&pdev->dev, "missing %s\n", prop_name);
		return -EINVAL;
	}

	prop_len /= sizeof(u32);
	if (prop_len % num) {
		dev_err(&pdev->dev, "bad length %d\n", prop_len);
		return -EINVAL;
	}

	prop_len /= num;
	vdd->level_votes = devm_kzalloc(&pdev->dev, prop_len * sizeof(int),
					GFP_KERNEL);
	if (!vdd->level_votes)
		return -ENOMEM;

	vdd->vdd_uv = devm_kzalloc(&pdev->dev,
				prop_len * sizeof(int) * (num - 1), GFP_KERNEL);
	if (!vdd->vdd_uv)
		return -ENOMEM;

	clk_intd->rate_max = devm_kzalloc(&pdev->dev,
				prop_len * sizeof(unsigned long), GFP_KERNEL);
	if (!clk_intd->rate_max)
		return -ENOMEM;

	array = kmalloc_array(prop_len * num, sizeof(u32), GFP_KERNEL);
	if (!array)
		return -ENOMEM;

	ret = of_property_read_u32_array(of, prop_name, array, prop_len * num);
	if (ret)
		return -ENOMEM;

	for (i = 0; i < prop_len; i++) {
		clk_intd->rate_max[i] = array[num * i];
		for (j = 1; j < num; j++) {
			vdd->vdd_uv[(num - 1) * i + (j - 1)] =
					array[num * i + j];
		}
	}

	kfree(array);
	vdd->num_levels = prop_len;
	vdd->cur_level = prop_len;
	clk_intd->num_rate_max = prop_len;

	return 0;
}

/*
 *  Find the voltage level required for a given clock rate.
 */
static int find_vdd_level(struct clk_vdd_class_data *clk_vdd_data,
							unsigned long rate)
{
	int level;

	for (level = 0; level < clk_vdd_data->num_rate_max; level++)
		if (rate <= clk_vdd_data->rate_max[level])
			break;

	if (level == clk_vdd_data->num_rate_max) {
		pr_err("Rate %lu is greater than highest Fmax\n", rate);
		return -EINVAL;
	}

	return level;
}

static int cpucc_clk_add_opp(struct clk_regmap *clkr, struct device *dev,
							unsigned long max_rate)
{
	struct clk_vdd_class_data *clk_vdd_data = &clkr->vdd_data;
	struct clk_vdd_class *vdd = clk_vdd_data->vdd_class;
	unsigned long rate = 0;
	long ret;
	int level, uv, j = 1;

	if (IS_ERR_OR_NULL(dev)) {
		pr_err("%s: Invalid parameters\n", __func__);
		return -EINVAL;
	}

	while (1) {
		rate = clk_vdd_data->rate_max[j++];
		level = find_vdd_level(clk_vdd_data, rate);
		if (level <= 0) {
			pr_warn("clock-cpu: no corner for %lu.\n", rate);
			return -EINVAL;
		}

		uv = vdd->vdd_uv[level];
		if (uv < 0) {
			pr_warn("clock-cpu: no uv for %lu.\n", rate);
			return -EINVAL;
		}

		ret = dev_pm_opp_add(dev, rate, uv);
		if (ret) {
			pr_warn("clock-cpu: failed to add OPP for %lu\n", rate);
			return rate;
		}

		if (rate >= max_rate)
			break;
	}

	return 0;
}

static void cpucc_clk_print_opp_table(int cpu)
{
	struct dev_pm_opp *oppfmax, *oppfmin;
	unsigned long apc_fmax, apc_fmin;
	u32 max_cpuss_index = apcs_mux_clk.clkr.vdd_data.num_rate_max;

	apc_fmax = apcs_mux_clk.clkr.vdd_data.rate_max[max_cpuss_index - 1];
	apc_fmin = apcs_mux_clk.clkr.vdd_data.rate_max[1];

	oppfmax = dev_pm_opp_find_freq_exact(get_cpu_device(cpu),
					apc_fmax, true);
	oppfmin = dev_pm_opp_find_freq_exact(get_cpu_device(cpu),
					apc_fmin, true);
	pr_info("Clock_cpu:(cpu %d) OPP voltage for %lu: %ld\n", cpu, apc_fmin,
		dev_pm_opp_get_voltage(oppfmin));
	pr_info("Clock_cpu:(cpu %d) OPP voltage for %lu: %ld\n", cpu, apc_fmax,
		dev_pm_opp_get_voltage(oppfmax));

}

static void cpucc_clk_populate_opp_table(struct platform_device *pdev)
{
	unsigned long apc_fmax;
	int cpu, final_cpu = 0;
	u32 max_cpuss_index = apcs_mux_clk.clkr.vdd_data.num_rate_max;

	apc_fmax = apcs_mux_clk.clkr.vdd_data.rate_max[max_cpuss_index - 1];

	for_each_possible_cpu(cpu) {
		final_cpu = cpu;
		WARN(cpucc_clk_add_opp(&apcs_mux_clk.clkr,
				get_cpu_device(cpu), apc_fmax),
				"Failed to add OPP levels for apcs_mux_clk\n");
	}
	cpucc_clk_print_opp_table(final_cpu);
}

static int cpucc_get_and_parse_dt_resource(struct platform_device *pdev,
						unsigned long *xo_rate)
{
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct clk *clk;
	int ret, speed_bin, version;
	char prop_name[] = "qcom,speedX-bin-vX";
	void __iomem *base;

	/* Require the RPM-XO clock to be registered before */
	clk = clk_get(dev, "bi_tcxo_ao");
	if (IS_ERR(clk)) {
		if (PTR_ERR(clk) != -EPROBE_DEFER)
			dev_err(dev, "Unable to get xo clock\n");
		return PTR_ERR(clk);
	}

	*xo_rate = clk_get_rate(clk);
	if (!*xo_rate)
		*xo_rate = 19200000;

	clk_put(clk);

	/* Require the GPLL0_OUT_EVEN clock to be registered before */
	clk = clk_get(dev, "gpll0_out_even");
	if (IS_ERR(clk)) {
		if (PTR_ERR(clk) != -EPROBE_DEFER)
			dev_err(dev, "Unable to get GPLL0 clock\n");
			return PTR_ERR(clk);
	}
	clk_put(clk);

	/* Rail Regulator for apcs_cpu_pll & cpuss mux*/
	vdd_pll.regulator[0] = devm_regulator_get(&pdev->dev, "vdd-pll");
	if (IS_ERR(vdd_pll.regulator[0])) {
		if (!(PTR_ERR(vdd_pll.regulator[0]) == -EPROBE_DEFER))
			dev_err(&pdev->dev,
				"Unable to get vdd_pll regulator\n");
		return PTR_ERR(vdd_pll.regulator[0]);
	}

	vdd_cpu.regulator[0] = devm_regulator_get(&pdev->dev, "cpu-vdd");
	if (IS_ERR(vdd_cpu.regulator[0])) {
		if (!(PTR_ERR(vdd_cpu.regulator[0]) == -EPROBE_DEFER))
			dev_err(&pdev->dev,
				"Unable to get cpu-vdd regulator\n");
		return PTR_ERR(vdd_cpu.regulator[0]);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "apcs_pll");
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base)) {
		dev_err(&pdev->dev, "Failed to map apcs_cpu_pll register base\n");
		return PTR_ERR(base);
	}

	cpu_regmap_config.name = "apcs_pll";
	apcs_cpu_pll.clkr.regmap = devm_regmap_init_mmio(dev, base,
							&cpu_regmap_config);
	if (IS_ERR(apcs_cpu_pll.clkr.regmap)) {
		dev_err(&pdev->dev, "Couldn't get regmap for apcs_cpu_pll\n");
		return PTR_ERR(apcs_cpu_pll.clkr.regmap);
	}
	apcs_cpu_pll.clkr.dev = &pdev->dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "apcs_cmd");
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base)) {
		dev_err(&pdev->dev, "Failed to map apcs_cmd register base\n");
		return PTR_ERR(base);
	}

	cpu_regmap_config.name = "apcs_cmd";
	apcs_mux_clk.clkr.regmap = devm_regmap_init_mmio(dev, base,
							&cpu_regmap_config);
	if (IS_ERR(apcs_mux_clk.clkr.regmap)) {
		dev_err(&pdev->dev, "Couldn't get regmap for apcs_cmd\n");
		return PTR_ERR(apcs_mux_clk.clkr.regmap);
	}
	apcs_mux_clk.clkr.dev = &pdev->dev;

	/* Get speed bin information */
	cpucc_clk_get_speed_bin(pdev, &speed_bin, &version);

	snprintf(prop_name, ARRAY_SIZE(prop_name),
			"qcom,speed%d-bin-v%d", speed_bin, version);

	ret = cpucc_clk_get_fmax_vdd_class(pdev,
	      (struct clk_vdd_class_data *)&apcs_mux_clk.clkr.vdd_data, prop_name);
	if (ret) {
		dev_err(&pdev->dev,
		"Can't get speed bin for apcs_mux_clk. Falling back to zero\n");
		ret = cpucc_clk_get_fmax_vdd_class(pdev,
				&apcs_mux_clk.clkr.vdd_data, "qcom,speed0-bin-v0");
		if (ret) {
			dev_err(&pdev->dev,
			"Unable to get speed bin for apcs_mux_clk freq-corner mapping info\n");
			return ret;
		}
	}

	return 0;
}

static int cpucc_driver_probe(struct platform_device *pdev)
{
	struct clk_hw_onecell_data *data;
	struct device *dev = &pdev->dev;
	int i, ret, cpu;
	unsigned long xo_rate;
	u32 l_val;

	ret = cpucc_get_and_parse_dt_resource(pdev, &xo_rate);
	if (ret < 0)
		return ret;

	l_val = apcs_cpu_pll_config.l;
	clk_lucid_5lpe_pll_configure(&apcs_cpu_pll, apcs_cpu_pll.clkr.regmap,
							&apcs_cpu_pll_config);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->num = ARRAY_SIZE(cpu_clks_hws);

	/* Register clocks with clock framework */
	for (i = 0; i < ARRAY_SIZE(cpu_clks_hws); i++) {
		ret = devm_clk_hw_register(dev, cpu_clks_hws[i]);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register clock\n");
			return ret;
		}
		data->hws[i] = cpu_clks_hws[i];
		devm_clk_regmap_list_node(dev, to_clk_regmap(cpu_clks_hws[i]));
	}

	ret = of_clk_add_hw_provider(dev->of_node, of_clk_hw_onecell_get, data);
	if (ret) {
		dev_err(&pdev->dev, "CPU clock driver registration failed\n");
		return ret;
	}

	/* Set to boot frequency */
	ret = clk_set_rate(apcs_mux_clk.clkr.hw.clk, l_val * xo_rate);
	if (ret) {
		dev_err(&pdev->dev, "Unable to set init rate on apcs_mux_clk\n");
		return ret;
	}

	/*
	 * We don't want the CPU clocks to be turned off at late init
	 * if CPUFREQ or HOTPLUG configs are disabled. So, bump up the
	 * refcount of these clocks. Any cpufreq/hotplug manager can assume
	 * that the clocks have already been prepared and enabled by the time
	 * they take over.
	 */
	get_online_cpus();
	for_each_online_cpu(cpu)
		WARN(clk_prepare_enable(apcs_mux_clk.clkr.hw.clk),
			"Unable to turn on CPU clock\n");
	put_online_cpus();

	cpucc_clk_populate_opp_table(pdev);

	dev_info(dev, "CPU clock Driver probed successfully\n");

	return 0;
}

static struct platform_driver cpu_clk_driver = {
	.probe = cpucc_driver_probe,
	.driver = {
		.name = "qcom-cpu-sdxlemur",
		.of_match_table = match_table,
	},
};

static int __init cpu_clk_init(void)
{
	return platform_driver_register(&cpu_clk_driver);
}
subsys_initcall(cpu_clk_init);

static void __exit cpu_clk_exit(void)
{
	platform_driver_unregister(&cpu_clk_driver);
}
module_exit(cpu_clk_exit);

MODULE_DESCRIPTION("SDXLEMUR CPU clock Driver");
MODULE_LICENSE("GPL v2");
