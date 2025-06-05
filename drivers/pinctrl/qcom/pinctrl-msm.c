// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013, Sony Mobile Communications AB.
 * Copyright (c) 2013-2020, The Linux Foundation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/slab.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/reboot.h>
#include <linux/pm.h>
#include <linux/log2.h>
#include <linux/bitmap.h>

#include <linux/soc/qcom/irq.h>

#include <linux/soc/qcom/irq.h>

#include "../core.h"
#include "../pinconf.h"
#include "pinctrl-msm.h"
#include "../pinctrl-utils.h"

/*zte_pm +++++*/
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define GPIO_SNP_SIZE 100
#endif
/*zte_pm -----*/

#define MAX_NR_GPIO 300
#define MAX_NR_TILES 4
#define PS_HOLD_OFFSET 0x820
#define QUP_MASK       GENMASK(5, 0)

/**
 * struct msm_pinctrl - state for a pinctrl-msm device
 * @dev:            device handle.
 * @pctrl:          pinctrl handle.
 * @chip:           gpiochip handle.
 * @restart_nb:     restart notifier block.
 * @irq:            parent irq for the TLMM irq_chip.
 * @n_dir_conns:    The number of pins directly connected to GIC.
 * @lock:           Spinlock to protect register resources as well
 *                  as msm_pinctrl data structures.
 * @enabled_irqs:   Bitmap of currently enabled irqs.
 * @dual_edge_irqs: Bitmap of irqs that need sw emulated dual edge
 *                  detection.
 * @skip_wake_irqs: Skip IRQs that are handled by wakeup interrupt contrroller
 * @soc;            Reference to soc_data of platform specific data.
 * @regs:           Base addresses for the TLMM tiles.
 */
struct msm_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctrl;
	struct gpio_chip chip;
	struct pinctrl_desc desc;
	struct notifier_block restart_nb;

	struct irq_chip irq_chip;
	int irq;
	int n_dir_conns;

	raw_spinlock_t lock;

	DECLARE_BITMAP(dual_edge_irqs, MAX_NR_GPIO);
	DECLARE_BITMAP(enabled_irqs, MAX_NR_GPIO);
	DECLARE_BITMAP(skip_wake_irqs, MAX_NR_GPIO);

	const struct msm_pinctrl_soc_data *soc;
	void __iomem *regs[MAX_NR_TILES];
};

static struct msm_pinctrl *msm_pinctrl_data;

#ifdef CONFIG_DEBUG_FS
static void __iomem *reassign_pctrl_reg(
		const struct msm_pinctrl_soc_data *soc,
				u32 gpio_id)
{
#ifdef CONFIG_FRAGMENTED_GPIO_ADDRESS_SPACE
	const struct msm_pingroup *g;
	int i = 0;

	g = &soc->groups[gpio_id];

	 /* Return base tile for invalid gpios */
	if (gpio_id >= soc->ngpios)
		return msm_pinctrl_data->regs;

	/* Return if pin base address is known in advance */
	if (soc->pin_base[gpio_id])
		return soc->pin_base[gpio_id];

	/* We dont know tile address of pin, find it */
	while (i < soc->n_tile) {
		if (g->tile_base == soc->tile_offsets[i])
			break;
		i++;
	}
	pr_debug("Base tile is %d for gpio %d\n", i, gpio_id);
	soc->pin_base[gpio_id] = msm_pinctrl_data->per_tile_regs[i];
	return soc->pin_base[gpio_id];
#else
	return msm_pinctrl_data->regs;
#endif
}
#endif

#define MSM_ACCESSOR(name) \
static u32 msm_readl_##name(struct msm_pinctrl *pctrl, \
			    const struct msm_pingroup *g) \
{ \
	return readl(pctrl->regs[g->tile] + g->name##_reg); \
} \
static void msm_writel_##name(u32 val, struct msm_pinctrl *pctrl, \
			      const struct msm_pingroup *g) \
{ \
	writel(val, pctrl->regs[g->tile] + g->name##_reg); \
}

MSM_ACCESSOR(ctl)
MSM_ACCESSOR(io)
MSM_ACCESSOR(intr_cfg)
MSM_ACCESSOR(intr_status)
MSM_ACCESSOR(intr_target)

static int msm_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->soc->ngroups;
}

static const char *msm_get_group_name(struct pinctrl_dev *pctldev,
					  unsigned group)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->soc->groups[group].name;
}

static int msm_get_group_pins(struct pinctrl_dev *pctldev,
				  unsigned group,
				  const unsigned **pins,
				  unsigned *num_pins)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	*pins = pctrl->soc->groups[group].pins;
	*num_pins = pctrl->soc->groups[group].npins;
	return 0;
}

static const struct pinctrl_ops msm_pinctrl_ops = {
	.get_groups_count	= msm_get_groups_count,
	.get_group_name		= msm_get_group_name,
	.get_group_pins		= msm_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_group,
	.dt_free_map		= pinctrl_utils_free_map,
};

static int msm_pinmux_request(struct pinctrl_dev *pctldev, unsigned offset)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	struct gpio_chip *chip = &pctrl->chip;

	return gpiochip_line_is_valid(chip, offset) ? 0 : -EINVAL;
}

static int msm_get_functions_count(struct pinctrl_dev *pctldev)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->soc->nfunctions;
}

static const char *msm_get_function_name(struct pinctrl_dev *pctldev,
					 unsigned function)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	return pctrl->soc->functions[function].name;
}

static int msm_get_function_groups(struct pinctrl_dev *pctldev,
				   unsigned function,
				   const char * const **groups,
				   unsigned * const num_groups)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);

	*groups = pctrl->soc->functions[function].groups;
	*num_groups = pctrl->soc->functions[function].ngroups;
	return 0;
}

static int msm_pinmux_set_mux(struct pinctrl_dev *pctldev,
				  unsigned function,
				  unsigned group)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	const struct msm_pingroup *g;
	unsigned long flags;
	u32 val, mask;
	int i;

	g = &pctrl->soc->groups[group];
	mask = GENMASK(g->mux_bit + order_base_2(g->nfuncs) - 1, g->mux_bit);

	for (i = 0; i < g->nfuncs; i++) {
		if (g->funcs[i] == function)
			break;
	}

	if (WARN_ON(i == g->nfuncs))
		return -EINVAL;

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_ctl(pctrl, g);
	val &= ~mask;
	val |= i << g->mux_bit;
	/* Check if egpio present and enable that feature */
	if (val & BIT(g->egpio_present))
		val |= BIT(g->egpio_enable);

	msm_writel_ctl(val, pctrl, g);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int msm_pinmux_request_gpio(struct pinctrl_dev *pctldev,
				   struct pinctrl_gpio_range *range,
				   unsigned offset)
{
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	const struct msm_pingroup *g = &pctrl->soc->groups[offset];

	/* No funcs? Probably ACPI so can't do anything here */
	if (!g->nfuncs)
		return 0;

	/* For now assume function 0 is GPIO because it always is */
	return msm_pinmux_set_mux(pctldev, g->funcs[0], offset);
}

static const struct pinmux_ops msm_pinmux_ops = {
	.request		= msm_pinmux_request,
	.get_functions_count	= msm_get_functions_count,
	.get_function_name	= msm_get_function_name,
	.get_function_groups	= msm_get_function_groups,
	.gpio_request_enable	= msm_pinmux_request_gpio,
	.set_mux		= msm_pinmux_set_mux,
};

static int msm_config_reg(struct msm_pinctrl *pctrl,
			  const struct msm_pingroup *g,
			  unsigned param,
			  unsigned *mask,
			  unsigned *bit)
{
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
	case PIN_CONFIG_BIAS_PULL_DOWN:
	case PIN_CONFIG_BIAS_BUS_HOLD:
	case PIN_CONFIG_BIAS_PULL_UP:
		*bit = g->pull_bit;
		*mask = 3;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		*bit = g->drv_bit;
		*mask = 7;
		break;
	case PIN_CONFIG_OUTPUT:
	case PIN_CONFIG_INPUT_ENABLE:
		*bit = g->oe_bit;
		*mask = 1;
		break;
	default:
		return -ENOTSUPP;
	}

	return 0;
}

#define MSM_NO_PULL		0
#define MSM_PULL_DOWN		1
#define MSM_KEEPER		2
#define MSM_PULL_UP_NO_KEEPER	2
#define MSM_PULL_UP		3

static unsigned msm_regval_to_drive(u32 val)
{
	return (val + 1) * 2;
}

static int msm_config_group_get(struct pinctrl_dev *pctldev,
				unsigned int group,
				unsigned long *config)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	unsigned param = pinconf_to_config_param(*config);
	unsigned mask;
	unsigned arg;
	unsigned bit;
	int ret;
	u32 val;

	g = &pctrl->soc->groups[group];

	ret = msm_config_reg(pctrl, g, param, &mask, &bit);
	if (ret < 0)
		return ret;

	val = msm_readl_ctl(pctrl, g);
	arg = (val >> bit) & mask;

	/* Convert register value to pinconf value */
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		if (arg != MSM_NO_PULL)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (arg != MSM_PULL_DOWN)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_BIAS_BUS_HOLD:
		if (pctrl->soc->pull_no_keeper)
			return -ENOTSUPP;

		if (arg != MSM_KEEPER)
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if (pctrl->soc->pull_no_keeper)
			arg = arg == MSM_PULL_UP_NO_KEEPER;
		else
			arg = arg == MSM_PULL_UP;
		if (!arg)
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		arg = msm_regval_to_drive(arg);
		break;
	case PIN_CONFIG_OUTPUT:
		/* Pin is not output */
		if (!arg)
			return -EINVAL;

		val = msm_readl_io(pctrl, g);
		arg = !!(val & BIT(g->in_bit));
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		/* Pin is output */
		if (arg)
			return -EINVAL;
		arg = 1;
		break;
	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int msm_config_group_set(struct pinctrl_dev *pctldev,
				unsigned group,
				unsigned long *configs,
				unsigned num_configs)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctldev);
	unsigned long flags;
	unsigned param;
	unsigned mask;
	unsigned arg;
	unsigned bit;
	int ret;
	u32 val;
	int i;

	g = &pctrl->soc->groups[group];

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		ret = msm_config_reg(pctrl, g, param, &mask, &bit);
		if (ret < 0)
			return ret;

		/* Convert pinconf values to register values */
		switch (param) {
		case PIN_CONFIG_BIAS_DISABLE:
			arg = MSM_NO_PULL;
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			arg = MSM_PULL_DOWN;
			break;
		case PIN_CONFIG_BIAS_BUS_HOLD:
			if (pctrl->soc->pull_no_keeper)
				return -ENOTSUPP;

			arg = MSM_KEEPER;
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			if (pctrl->soc->pull_no_keeper)
				arg = MSM_PULL_UP_NO_KEEPER;
			else
				arg = MSM_PULL_UP;
			break;
		case PIN_CONFIG_DRIVE_STRENGTH:
			/* Check for invalid values */
			if (arg > 16 || arg < 2 || (arg % 2) != 0)
				arg = -1;
			else
				arg = (arg / 2) - 1;
			break;
		case PIN_CONFIG_OUTPUT:
			/* set output value */
			raw_spin_lock_irqsave(&pctrl->lock, flags);
			val = msm_readl_io(pctrl, g);
			if (arg)
				val |= BIT(g->out_bit);
			else
				val &= ~BIT(g->out_bit);
			msm_writel_io(val, pctrl, g);
			raw_spin_unlock_irqrestore(&pctrl->lock, flags);

			/* enable output */
			arg = 1;
			break;
		case PIN_CONFIG_INPUT_ENABLE:
			/* disable output */
			arg = 0;
			break;
		default:
			dev_err(pctrl->dev, "Unsupported config parameter: %x\n",
				param);
			return -EINVAL;
		}

		/* Range-check user-supplied value */
		if (arg & ~mask) {
			dev_err(pctrl->dev, "config %x: %x is invalid\n", param, arg);
			return -EINVAL;
		}

		raw_spin_lock_irqsave(&pctrl->lock, flags);
		val = msm_readl_ctl(pctrl, g);
		val &= ~(mask << bit);
		val |= arg << bit;
		msm_writel_ctl(val, pctrl, g);
		raw_spin_unlock_irqrestore(&pctrl->lock, flags);
	}

	return 0;
}

static const struct pinconf_ops msm_pinconf_ops = {
	.is_generic		= true,
	.pin_config_group_get	= msm_config_group_get,
	.pin_config_group_set	= msm_config_group_set,
};

static int msm_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[offset];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_ctl(pctrl, g);
	val &= ~BIT(g->oe_bit);
	msm_writel_ctl(val, pctrl, g);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int msm_gpio_direction_output(struct gpio_chip *chip, unsigned offset, int value)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[offset];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_io(pctrl, g);
	if (value)
		val |= BIT(g->out_bit);
	else
		val &= ~BIT(g->out_bit);
	msm_writel_io(val, pctrl, g);

	val = msm_readl_ctl(pctrl, g);
	val |= BIT(g->oe_bit);
	msm_writel_ctl(val, pctrl, g);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int msm_gpio_get_direction(struct gpio_chip *chip, unsigned int offset)
{
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	u32 val;

	g = &pctrl->soc->groups[offset];

	val = msm_readl_ctl(pctrl, g);

	/* 0 = output, 1 = input */
	return val & BIT(g->oe_bit) ? 0 : 1;
}

static int msm_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	u32 val;

	g = &pctrl->soc->groups[offset];

	val = msm_readl_io(pctrl, g);
	return !!(val & BIT(g->in_bit));
}

static void msm_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned long flags;
	u32 val;

	g = &pctrl->soc->groups[offset];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_io(pctrl, g);
	if (value)
		val |= BIT(g->out_bit);
	else
		val &= ~BIT(g->out_bit);
	msm_writel_io(val, pctrl, g);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

#ifdef CONFIG_DEBUG_FS

/*zte_pm add for zte_gpio debug, begin*/
#define FUNC_MASK 7
#define DRV_MASK 7
#define PULL_MASK 3
#define LEVEL_MASK 1

#define OWNER_NOT_DEFINE 123

unsigned gpio_func_show_pm(int *id, struct gpio_chip *chip)
{
	unsigned func = 0;
	u32 ctl_reg = 0;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	ctl_reg = readl_relaxed(base + g->ctl_reg);
	func = (ctl_reg >> g->mux_bit) & FUNC_MASK;
	return func;
}

unsigned gpio_direction_show_pm(int *id, struct gpio_chip *chip)
{
	int is_out = 0;
	u32 ctl_reg = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	g = &pctrl->soc->groups[*id];
	ctl_reg = readl_relaxed(base + g->ctl_reg);
	is_out = !!(ctl_reg & BIT(g->oe_bit));

	return is_out;
}

unsigned gpio_direction_store_pm(int *id, struct gpio_chip *chip, u32 value)
{
	unsigned long flags = 0;
	u32 val = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	raw_spin_lock_irqsave(&pctrl->lock, flags);
	val = readl_relaxed(base + g->ctl_reg);
	if (value) {
		val |= BIT(g->oe_bit);
	} else {
		val &= ~BIT(g->oe_bit);
	}
	writel_relaxed(val, base + g->ctl_reg);
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

unsigned gpio_pull_show_pm(int *id, struct gpio_chip *chip)
{
	int pull = 0;
	u32 ctl_reg = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	ctl_reg = readl_relaxed(base + g->ctl_reg);
	pull = (ctl_reg >> g->pull_bit) & PULL_MASK;

	return pull;
}

unsigned gpio_drv_show_pm(int *id, struct gpio_chip *chip)
{
	int drive = 0;
	u32 ctl_reg = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	ctl_reg = readl_relaxed(base + g->ctl_reg);
	drive = (ctl_reg >> g->drv_bit) & DRV_MASK;

	return drive;
}


unsigned gpio_level_show_pm(int *id, struct gpio_chip *chip)
{
	int level = 0, is_out = 0;
	u32 ctl_reg = 0, io_reg = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	ctl_reg = readl_relaxed(base + g->ctl_reg);
	io_reg = readl_relaxed(base + g->io_reg);

	is_out = !!(ctl_reg & BIT(g->oe_bit));
	if (is_out)
		level = (io_reg >> g->out_bit) & LEVEL_MASK;
	else
		level = (io_reg >> g->in_bit) & LEVEL_MASK;

	return level;
}

unsigned gpio_level_store_pm(int *id, struct gpio_chip *chip, u32 value)
{
	u32 val = 0;
	/*u32 io_reg;*/
	unsigned long flags = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	raw_spin_lock_irqsave(&pctrl->lock, flags);
	val = readl_relaxed(base + g->io_reg);
	if (value)
		val |= BIT(g->out_bit);
	else
		val &= ~BIT(g->out_bit);
	writel_relaxed(val, base + g->io_reg);
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

unsigned gpio_pull_store_pm(int *id, struct gpio_chip *chip, u32 values)
{
	u32 ctl_reg = 0, pull = 0;
	unsigned long flags = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	ctl_reg = readl_relaxed(base + g->ctl_reg);
	pull = values;
	if (pull  > MSM_PULL_UP)
		pull  = MSM_PULL_UP;

	ctl_reg &= ~(PULL_MASK << g->pull_bit);
	ctl_reg |= pull << g->pull_bit;

	writel_relaxed(ctl_reg, base + g->ctl_reg);
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

unsigned gpio_func_store_pm(int *id, struct gpio_chip *chip, u32 values)
{
	u32 ctl_reg = 0, func = 0;
	unsigned long flags = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	ctl_reg = readl_relaxed(base + g->ctl_reg);
	func = values;
	if (func > FUNC_MASK)
		func  = FUNC_MASK;

	ctl_reg &= ~(FUNC_MASK << g->mux_bit);
	ctl_reg |= func << g->mux_bit;

	writel_relaxed(ctl_reg, base + g->ctl_reg);
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

unsigned gpio_int_owner_show_pm(int *id, struct gpio_chip *chip)
{
	return OWNER_NOT_DEFINE;
}

unsigned gpio_int_enable_show_pm(int *id, struct gpio_chip *chip)
{
	int enable = 0;
	u32 intr_cfg_reg = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	intr_cfg_reg = readl_relaxed(base + g->intr_cfg_reg);
	enable = (intr_cfg_reg >> g->intr_enable_bit) & LEVEL_MASK;

	return enable;
}

unsigned gpio_int_dect_show_pm(int *id, struct gpio_chip *chip)
{
	int dect = 0;
	u32 intr_cfg_reg = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	intr_cfg_reg = readl_relaxed(base + g->intr_cfg_reg);
	dect = (intr_cfg_reg >> g->intr_detection_bit) & PULL_MASK;

	return dect;
}

unsigned gpio_drv_store_pm(int *id, struct gpio_chip *chip, u32 values)
{
	int drive = 0;
	u32 ctl_reg = 0;
	unsigned long flags = 0;

	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	const struct msm_pingroup *g;
	void __iomem *base;

	g = &pctrl->soc->groups[*id];
	base = reassign_pctrl_reg(pctrl->soc, *id);

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	ctl_reg = readl_relaxed(base + g->ctl_reg);

	drive = values;
	if (drive > DRV_MASK)
		drive = DRV_MASK;

	ctl_reg &= ~(DRV_MASK << g->drv_bit);
	ctl_reg |= drive << g->drv_bit;

	writel_relaxed(ctl_reg, base + g->ctl_reg);
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static bool skip_special_gpios(int i)
{
#if defined(CONFIG_ARCH_LAHAINA)
	if ((i == 0) || (i == 1) || (i == 2) || (i == 3)
			|| (i == 44) || (i == 45) || (i == 46) || (i == 47)
			) {
		pr_info("msm_dump_gpios skip gpio %d to avoid system crash\n", i);
		return true;
	} else {
		return false;
	}
#else
	return false;
#endif
}

static struct gpio_chip *chip_debug;

int msm_dump_gpios(struct seq_file *m, int curr_len, char *gpio_buffer)
{

	unsigned int i = 0, func_sel = 0, pull = 0, drv = 0, len = 0, direction = 0, level = 0;
	char list_gpio[100];
	char *title_msg = "------------ MSM GPIO -------------";

	if (m) {
		seq_printf(m, "%s\n", title_msg);
	} else {
		pr_info("%s\n", title_msg);
		curr_len += snprintf(gpio_buffer + curr_len, GPIO_SNP_SIZE,
			"%s\n", title_msg);
	}

	for (i = 0; i < chip_debug->ngpio; i++) {
		memset(list_gpio, 0, sizeof(list_gpio));
		len = 0;
		if (skip_special_gpios(i))
			continue;
		len += snprintf(list_gpio + len, GPIO_SNP_SIZE, "GPIO[%3d]: ", i);

		func_sel = gpio_func_show_pm(&i, chip_debug);
		len += snprintf(list_gpio + len, GPIO_SNP_SIZE - len, "[FS]0x%x, ", func_sel);

		level = gpio_level_show_pm(&i, chip_debug);
		direction = gpio_direction_show_pm(&i, chip_debug);
		if (direction)
			len += snprintf(list_gpio + len, GPIO_SNP_SIZE - len,
							"[DIR]OUT, [VAL]%s ", level ? "HIGH" : " LOW");
		else
			len += snprintf(list_gpio + len, GPIO_SNP_SIZE - len,
							"[DIR] IN, [VAL]%s ", level ? "HIGH" : " LOW");

		pull = gpio_pull_show_pm(&i, chip_debug);
		switch (pull) {
		case 0x0:
			len += snprintf(list_gpio + len, GPIO_SNP_SIZE - len, "[PULL]NO, ");
			break;
		case 0x1:
			len += snprintf(list_gpio + len, GPIO_SNP_SIZE - len, "[PULL]PD, ");
			break;
		case 0x2:
			len += snprintf(list_gpio + len, GPIO_SNP_SIZE - len, "[PULL]KP, ");
			break;
		case 0x3:
			len += snprintf(list_gpio + len, GPIO_SNP_SIZE - len, "[PULL]PU, ");
			break;
		default:
			break;
		}

		drv = gpio_drv_show_pm(&i, chip_debug);
		len += snprintf(list_gpio + len, GPIO_SNP_SIZE - len, "[DRV]%2dmA, ", 2*(drv+1));

		/*zte need show*/
		/*
		extern unsigned gpio_int_enable_show_pm(int *id, struct gpio_chip *chip);
		extern unsigned gpio_int_owner_show_pm(int *id, struct gpio_chip *chip);
		extern unsigned gpio_int_dect_show_pm(int *id, struct gpio_chip *chip);
		*/

		list_gpio[99] = '\0';
		if (m) {
			seq_printf(m, "%s\n", list_gpio);
		} else {
			pr_info("%s\n", list_gpio);
			curr_len += snprintf(gpio_buffer + curr_len, GPIO_SNP_SIZE, "%s\n", list_gpio);
		}
	}

	return curr_len;
}

static int gpio_func_get(void *data, u64 *val)
{
	*val = gpio_func_show_pm(data, chip_debug);
	return 0;
}

static int gpio_func_set(void *data, u64 val)
{
	val = gpio_func_store_pm(data, chip_debug, val);
	return 0;
}

static int gpio_level_get(void *data, u64 *val)
{
	*val = gpio_level_show_pm(data, chip_debug);
	return 0;
}

static int gpio_level_set(void *data, u64 val)
{
	val = gpio_level_store_pm(data, chip_debug, val);
	return 0;
}
static int gpio_pull_get(void *data, u64 *val)
{
	*val = gpio_pull_show_pm(data, chip_debug);
	return 0;
}

static int gpio_pull_set(void *data, u64 val)
{
	val = gpio_pull_store_pm(data, chip_debug, val);
	return 0;
}

static int gpio_drv_get(void *data, u64 *val)
{
	*val = gpio_drv_show_pm(data, chip_debug);
	return 0;
}

static int gpio_direction_get(void *data, u64 *val)
{
	*val = gpio_direction_show_pm(data, chip_debug);
	return 0;
}

static int gpio_int_enable_get(void *data, u64 *val)
{
	*val = gpio_int_enable_show_pm(data, chip_debug);
	return 0;
}

static int gpio_int_owner_get(void *data, u64 *val)
{
	*val = gpio_int_owner_show_pm(data, chip_debug);
	return 0;
}

static int gpio_int_dect_get(void *data, u64 *val)
{
	*val = gpio_int_dect_show_pm(data, chip_debug);
	return 0;
}

static int gpio_drv_set(void *data, u64 val)
{
	val = gpio_drv_store_pm(data, chip_debug, val);
	return 0;
}

static int gpio_direction_set(void *data, u64 val)
{
	val = gpio_direction_store_pm(data, chip_debug, val);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(gpio_direction_fops, gpio_direction_get, gpio_direction_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(gpio_level_fops, gpio_level_get, gpio_level_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(gpio_drv_fops, gpio_drv_get, gpio_drv_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(gpio_func_sel_fops, gpio_func_get, gpio_func_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(gpio_pull_fops, gpio_pull_get, gpio_pull_set, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(gpio_int_enable_fops, gpio_int_enable_get, NULL, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(gpio_int_owner_fops, gpio_int_owner_get, NULL, "%llu\n");
DEFINE_SIMPLE_ATTRIBUTE(gpio_int_dect_fops, gpio_int_dect_get, NULL, "%llu\n");

/*int pmic_dump_pins(struct seq_file *m, int curr_len, char *gpio_buffer);*/
extern int vendor_print_gpio_buffer(struct seq_file *m) __attribute__((weak));

static int list_gpios_show(struct seq_file *m, void *unused)
{
	msm_dump_gpios(m, 0, NULL);
	/*pmic_dump_pins(m, 0, NULL);*/
	return 0;
}

static int list_sleep_gpios_show(struct seq_file *m, void *unused)
{
	vendor_print_gpio_buffer(m);
	return 0;
}

static int list_gpios_open(struct inode *inode, struct file *file)
{
	return single_open(file, list_gpios_show, inode->i_private);
}

static int list_sleep_gpios_open(struct inode *inode, struct file *file)
{
	return single_open(file, list_sleep_gpios_show, inode->i_private);
}

static int list_sleep_gpios_release(struct inode *inode, struct file *file)
{
	/*free_gpio_buffer();*/
	return single_release(inode, file);
}

static const struct file_operations list_gpios_fops = {
	.open		= list_gpios_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release		= seq_release,
};

static const struct file_operations list_sleep_gpios_fops = {
	.open		= list_sleep_gpios_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release		= list_sleep_gpios_release,
};

static struct dentry *debugfs_base;
#define DEBUG_MAX_FNAME	   8

static int gpio_add_status(struct gpio_chip *chip, int id, unsigned int *index_p)
{
	struct dentry *gpio_dir;
	char name[DEBUG_MAX_FNAME];

	*index_p = id;
	snprintf(name, DEBUG_MAX_FNAME-1, "%d", *index_p);

	gpio_dir = debugfs_create_dir(name, debugfs_base);
	if (!gpio_dir)
		return -ENOMEM;

	if (!debugfs_create_file("direction", S_IRUGO | S_IWUSR, gpio_dir, index_p, &gpio_direction_fops))
		goto error;

	if (!debugfs_create_file("level", S_IRUGO | S_IWUSR, gpio_dir, index_p, &gpio_level_fops))
		goto error;

	if (!debugfs_create_file("drv_strength", S_IRUGO | S_IWUSR, gpio_dir, index_p, &gpio_drv_fops))
		goto error;

	if (!debugfs_create_file("func_sel", S_IRUGO | S_IWUSR, gpio_dir, index_p, &gpio_func_sel_fops))
		goto error;

	if (!debugfs_create_file("pull", S_IRUGO | S_IWUSR, gpio_dir, index_p, &gpio_pull_fops))
		goto error;

	if (!debugfs_create_file("int_enable", S_IRUGO, gpio_dir, index_p, &gpio_int_enable_fops))
		goto error;

	if (!debugfs_create_file("int_owner", S_IRUGO | S_IWUSR, gpio_dir, index_p, &gpio_int_owner_fops))
		goto error;

	if (!debugfs_create_file("int_dect_type", S_IRUGO, gpio_dir, index_p, &gpio_int_dect_fops))
		goto error;

	return 0;
error:
	debugfs_remove_recursive(gpio_dir);
	return -ENOMEM;
}

int gpio_status_debug_init(struct gpio_chip *chip)
{
	int i;
	int err = 0;
	unsigned gpio = chip->base;
	unsigned int *index_p;

	chip_debug = chip;

	debugfs_base = debugfs_create_dir("zte_gpio", NULL);
	if (!debugfs_base)
		return -ENOMEM;

	if (!debugfs_create_file("dump_gpios", S_IRUGO, debugfs_base,
				NULL, &list_gpios_fops))
		return -ENOMEM;

	if (!debugfs_create_file("dump_sleep_gpios", S_IRUGO, debugfs_base,
				NULL, &list_sleep_gpios_fops))
		return -ENOMEM;

	/* NOTICE: index_p should be reserved for debugfs, not to invoke kfree(index_p) */
	index_p = kcalloc(chip->ngpio, sizeof(*index_p), GFP_KERNEL);

	for (i = 0; i < chip->ngpio; i++, gpio++) {
		err = gpio_add_status(chip, i, index_p + i);
	}

	return err;
}

/*zte_pm zte_gpio debug, end*/

static void msm_gpio_dbg_show_one(struct seq_file *s,
				  struct pinctrl_dev *pctldev,
				  struct gpio_chip *chip,
				  unsigned offset,
				  unsigned gpio)
{
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(chip);
	unsigned func;
	int is_out;
	int drive;
	int pull;
	int val;
	u32 ctl_reg, io_reg;

	static const char * const pulls_keeper[] = {
		"no pull",
		"pull down",
		"keeper",
		"pull up"
	};

	static const char * const pulls_no_keeper[] = {
		"no pull",
		"pull down",
		"pull up",
	};

	if (!gpiochip_line_is_valid(chip, offset))
		return;

	g = &pctrl->soc->groups[offset];
	ctl_reg = msm_readl_ctl(pctrl, g);
	io_reg = msm_readl_io(pctrl, g);

	is_out = !!(ctl_reg & BIT(g->oe_bit));
	func = (ctl_reg >> g->mux_bit) & 7;
	drive = (ctl_reg >> g->drv_bit) & 7;
	pull = (ctl_reg >> g->pull_bit) & 3;

	if (is_out)
		val = !!(io_reg & BIT(g->out_bit));
	else
		val = !!(io_reg & BIT(g->in_bit));

	seq_printf(s, " %-8s: %-3s", g->name, is_out ? "out" : "in");
	seq_printf(s, " %-4s func%d", val ? "high" : "low", func);
	seq_printf(s, " %dmA", msm_regval_to_drive(drive));
	if (pctrl->soc->pull_no_keeper)
		seq_printf(s, " %s", pulls_no_keeper[pull]);
	else
		seq_printf(s, " %s", pulls_keeper[pull]);
	seq_puts(s, "\n");
}

static void msm_gpio_dbg_show(struct seq_file *s, struct gpio_chip *chip)
{
	unsigned gpio = chip->base;
	unsigned i;

	for (i = 0; i < chip->ngpio; i++, gpio++) {
		/*zte_pm +++++*/
		if (skip_special_gpios(i))
			continue;
		/*zte_pm -----*/
		msm_gpio_dbg_show_one(s, NULL, chip, i, gpio);
	}
}

#else
#define msm_gpio_dbg_show NULL
#endif

static int msm_gpio_init_valid_mask(struct gpio_chip *gc,
					unsigned long *valid_mask,
					unsigned int ngpios)
{
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	int ret;
	unsigned int len, i;
	const int *reserved = pctrl->soc->reserved_gpios;
	u16 *tmp;

	/* Driver provided reserved list overrides DT and ACPI */
	if (reserved) {
		bitmap_fill(valid_mask, ngpios);
		for (i = 0; reserved[i] >= 0; i++) {
			if (i >= ngpios || reserved[i] >= ngpios) {
				dev_err(pctrl->dev, "invalid list of reserved GPIOs\n");
				return -EINVAL;
			}
			clear_bit(reserved[i], valid_mask);
		}

		return 0;
	}

	/* The number of GPIOs in the ACPI tables */
	len = ret = device_property_count_u16(pctrl->dev, "gpios");
	if (ret < 0)
		return 0;

	if (ret > ngpios)
		return -EINVAL;

	tmp = kmalloc_array(len, sizeof(*tmp), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	ret = device_property_read_u16_array(pctrl->dev, "gpios", tmp, len);
	if (ret < 0) {
		dev_err(pctrl->dev, "could not read list of GPIOs\n");
		goto out;
	}

	bitmap_zero(valid_mask, ngpios);
	for (i = 0; i < len; i++)
		set_bit(tmp[i], valid_mask);

out:
	kfree(tmp);
	return ret;
}

static const struct gpio_chip msm_gpio_template = {
	.direction_input  = msm_gpio_direction_input,
	.direction_output = msm_gpio_direction_output,
	.get_direction    = msm_gpio_get_direction,
	.get              = msm_gpio_get,
	.set              = msm_gpio_set,
	.request          = gpiochip_generic_request,
	.free             = gpiochip_generic_free,
	.dbg_show         = msm_gpio_dbg_show,
};

/* For dual-edge interrupts in software, since some hardware has no
 * such support:
 *
 * At appropriate moments, this function may be called to flip the polarity
 * settings of both-edge irq lines to try and catch the next edge.
 *
 * The attempt is considered successful if:
 * - the status bit goes high, indicating that an edge was caught, or
 * - the input value of the gpio doesn't change during the attempt.
 * If the value changes twice during the process, that would cause the first
 * test to fail but would force the second, as two opposite
 * transitions would cause a detection no matter the polarity setting.
 *
 * The do-loop tries to sledge-hammer closed the timing hole between
 * the initial value-read and the polarity-write - if the line value changes
 * during that window, an interrupt is lost, the new polarity setting is
 * incorrect, and the first success test will fail, causing a retry.
 *
 * Algorithm comes from Google's msmgpio driver.
 */
static void msm_gpio_update_dual_edge_pos(struct msm_pinctrl *pctrl,
					  const struct msm_pingroup *g,
					  struct irq_data *d)
{
	int loop_limit = 100;
	unsigned val, val2, intstat;
	unsigned pol;

	do {
		val = msm_readl_io(pctrl, g) & BIT(g->in_bit);

		pol = msm_readl_intr_cfg(pctrl, g);
		pol ^= BIT(g->intr_polarity_bit);
		msm_writel_intr_cfg(pol, pctrl, g);

		val2 = msm_readl_io(pctrl, g) & BIT(g->in_bit);
		intstat = msm_readl_intr_status(pctrl, g);
		if (intstat || (val == val2))
			return;
	} while (loop_limit-- > 0);
	dev_err(pctrl->dev, "dual-edge irq failed to stabilize, %#08x != %#08x\n",
		val, val2);
}

static bool is_gpio_dual_edge(struct irq_data *d, irq_hw_number_t *irq)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	struct msm_dir_conn *dc;
	int i;

	for (i = pctrl->n_dir_conns; i > 0; i--) {
		dc = &pctrl->soc->dir_conn[i];

		if (dc->gpio == d->hwirq) {
			*irq = dc->irq;
			return true;
		}
	}

	return false;
}

static void msm_gpio_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	const struct msm_pingroup *g;
	unsigned long flags;
	struct irq_data *dir_conn_data;
	irq_hw_number_t dir_conn_irq = 0;
	u32 val;

	if (d->parent_data) {
		if (is_gpio_dual_edge(d, &dir_conn_irq)) {
			dir_conn_data = irq_get_irq_data(dir_conn_irq);
			if (!dir_conn_data)
				return;

			dir_conn_data->chip->irq_mask(dir_conn_data);
		}
		irq_chip_mask_parent(d);
	}

	if (test_bit(d->hwirq, pctrl->skip_wake_irqs))
		return;

	g = &pctrl->soc->groups[d->hwirq];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_intr_cfg(pctrl, g);
	/*
	 * There are two bits that control interrupt forwarding to the CPU. The
	 * RAW_STATUS_EN bit causes the level or edge sensed on the line to be
	 * latched into the interrupt status register when the hardware detects
	 * an irq that it's configured for (either edge for edge type or level
	 * for level type irq). The 'non-raw' status enable bit causes the
	 * hardware to assert the summary interrupt to the CPU if the latched
	 * status bit is set. There's a bug though, the edge detection logic
	 * seems to have a problem where toggling the RAW_STATUS_EN bit may
	 * cause the status bit to latch spuriously when there isn't any edge
	 * so we can't touch that bit for edge type irqs and we have to keep
	 * the bit set anyway so that edges are latched while the line is masked.
	 *
	 * To make matters more complicated, leaving the RAW_STATUS_EN bit
	 * enabled all the time causes level interrupts to re-latch into the
	 * status register because the level is still present on the line after
	 * we ack it. We clear the raw status enable bit during mask here and
	 * set the bit on unmask so the interrupt can't latch into the hardware
	 * while it's masked.
	 */
	if (irqd_get_trigger_type(d) & IRQ_TYPE_LEVEL_MASK)
		val &= ~BIT(g->intr_raw_status_bit);

	val &= ~BIT(g->intr_enable_bit);
	msm_writel_intr_cfg(val, pctrl, g);

	clear_bit(d->hwirq, pctrl->enabled_irqs);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

static void msm_gpio_irq_clear_unmask(struct irq_data *d, bool status_clear)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	const struct msm_pingroup *g;
	struct irq_data *dir_conn_data;
	irq_hw_number_t dir_conn_irq = 0;
	unsigned long flags;
	u32 val;

	if (d->parent_data) {
		if (is_gpio_dual_edge(d, &dir_conn_irq)) {
			dir_conn_data = irq_get_irq_data(dir_conn_irq);
			if (!dir_conn_data)
				return;

			dir_conn_data->chip->irq_unmask(dir_conn_data);
		}
		irq_chip_unmask_parent(d);
	}

	if (test_bit(d->hwirq, pctrl->skip_wake_irqs))
		return;

	g = &pctrl->soc->groups[d->hwirq];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	if (status_clear) {
		/*
		 * clear the interrupt status bit before unmask to avoid
		 * any erroneous interrupts that would have got latched
		 * when the interrupt is not in use.
		 */
		val = msm_readl_intr_status(pctrl, g);
		val &= ~BIT(g->intr_status_bit);
		msm_writel_intr_status(val, pctrl, g);
	}

	val = msm_readl_intr_cfg(pctrl, g);
	val |= BIT(g->intr_raw_status_bit);
	val |= BIT(g->intr_enable_bit);
	msm_writel_intr_cfg(val, pctrl, g);

	set_bit(d->hwirq, pctrl->enabled_irqs);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

static void msm_gpio_irq_enable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	struct irq_data *dir_conn_data;
	irq_hw_number_t dir_conn_irq = 0;

	/*
	 * Clear the interrupt that may be pending before we enable
	 * the line.
	 * This is especially a problem with the GPIOs routed to the
	 * PDC. These GPIOs are direct-connect interrupts to the GIC.
	 * Disabling the interrupt line at the PDC does not prevent
	 * the interrupt from being latched at the GIC. The state at
	 * GIC needs to be cleared before enabling.
	 */
	if (d->parent_data) {
		if (is_gpio_dual_edge(d, &dir_conn_irq)) {
			dir_conn_data = irq_get_irq_data(dir_conn_irq);
			if (!dir_conn_data)
				return;

			irq_set_irqchip_state(dir_conn_irq,
					IRQCHIP_STATE_PENDING, 0);
			dir_conn_data->chip->irq_unmask(dir_conn_data);
		}
		irq_chip_set_parent_state(d, IRQCHIP_STATE_PENDING, 0);
		irq_chip_enable_parent(d);
	}

	if (test_bit(d->hwirq, pctrl->skip_wake_irqs))
		return;

	msm_gpio_irq_clear_unmask(d, true);
}

static void msm_gpio_irq_disable(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	struct irq_data *dir_conn_data;
	irq_hw_number_t dir_conn_irq = 0;

	if (d->parent_data) {
		if (is_gpio_dual_edge(d, &dir_conn_irq)) {
			dir_conn_data = irq_get_irq_data(dir_conn_irq);
			if (!dir_conn_data)
				return;

			dir_conn_data->chip->irq_mask(dir_conn_data);
		}
		irq_chip_disable_parent(d);
	}

	if (test_bit(d->hwirq, pctrl->skip_wake_irqs))
		return;

	msm_gpio_irq_mask(d);
}

static void msm_gpio_irq_unmask(struct irq_data *d)
{
	msm_gpio_irq_clear_unmask(d, false);
}

static void msm_gpio_irq_ack(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	const struct msm_pingroup *g;
	unsigned long flags;
	u32 val;

	if (test_bit(d->hwirq, pctrl->skip_wake_irqs))
		return;

	g = &pctrl->soc->groups[d->hwirq];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	val = msm_readl_intr_status(pctrl, g);
	if (g->intr_ack_high)
		val |= BIT(g->intr_status_bit);
	else
		val &= ~BIT(g->intr_status_bit);
	msm_writel_intr_status(val, pctrl, g);

	if (test_bit(d->hwirq, pctrl->dual_edge_irqs))
		msm_gpio_update_dual_edge_pos(pctrl, g, d);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

static void msm_dirconn_cfg_reg(struct irq_data *d, u32 offset)
{
	u32 val;
	const struct msm_pingroup *g;
	unsigned long flags;
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);

	raw_spin_lock_irqsave(&pctrl->lock, flags);
	g = &pctrl->soc->groups[d->hwirq];

	val = (d->hwirq) & 0xFF;

	writel_relaxed(val, pctrl->regs[g->tile] + g->dir_conn_reg
		       + (offset * 4));

	val = msm_readl_intr_cfg(pctrl, g);
	val |= BIT(g->dir_conn_en_bit);

	msm_writel_intr_cfg(val, pctrl, g);
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

static void msm_dirconn_uncfg_reg(struct irq_data *d, u32 offset)
{
	u32 val = 0;
	const struct msm_pingroup *g;
	unsigned long flags;
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);

	raw_spin_lock_irqsave(&pctrl->lock, flags);
	g = &pctrl->soc->groups[d->hwirq];

	writel_relaxed(val, pctrl->regs + g->dir_conn_reg + (offset * 4));
	val = readl_relaxed(pctrl->regs + g->intr_cfg_reg);
	val &= ~BIT(g->dir_conn_en_bit);
	writel_relaxed(val, pctrl->regs + g->intr_cfg_reg);
	raw_spin_unlock_irqrestore(&pctrl->lock, flags);
}

static int select_dir_conn_mux(struct irq_data *d, irq_hw_number_t *irq,
			       bool add)
{
	struct msm_dir_conn *dc = NULL;
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	int i, n_dir_conns = pctrl->n_dir_conns;

	for (i = n_dir_conns; i > 0; i--) {
		dc = &pctrl->soc->dir_conn[i];
		if (dc->gpio == d->hwirq && !add) {
			*irq = dc->irq;
			dc->gpio = -1;
			return n_dir_conns - i;
		}

		if (dc->gpio == -1 && add) {
			dc->gpio = (int)d->hwirq;
			*irq = dc->irq;
			return n_dir_conns - i;
		}
	}

	pr_err("%s: No direct connects selected for interrupt %lu\n",
				__func__, d->hwirq);
	return -EBUSY;
}

static void msm_gpio_dirconn_handler(struct irq_desc *desc)
{
	struct irq_data *irqd = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);

	if (!irqd)
		return;

	chained_irq_enter(chip, desc);
	generic_handle_irq(irqd->irq);
	chained_irq_exit(chip, desc);
	irq_set_irqchip_state(irq_desc_get_irq_data(desc)->irq,
			      IRQCHIP_STATE_ACTIVE, 0);
}

static void add_dirconn_tlmm(struct irq_data *d, struct msm_pinctrl *pctrl)
{
	struct irq_data *dir_conn_data = NULL;
	int offset = 0;
	irq_hw_number_t irq = 0;

	offset = select_dir_conn_mux(d, &irq, true);
	if (offset < 0)
		return;

	msm_dirconn_cfg_reg(d, offset);
	irq_set_handler_data(irq, d);
	dir_conn_data = irq_get_irq_data(irq);

	if (!dir_conn_data)
		return;

	dir_conn_data->chip->irq_unmask(dir_conn_data);
}

static void remove_dirconn_tlmm(struct irq_data *d, irq_hw_number_t irq)
{
	struct irq_data *dir_conn_data = NULL;
	int offset = 0;

	offset = select_dir_conn_mux(d, &irq, false);
	if (offset < 0)
		return;

	msm_dirconn_uncfg_reg(d, offset);
	irq_set_handler_data(irq, NULL);
	dir_conn_data = irq_get_irq_data(irq);

	if (!dir_conn_data)
		return;

	dir_conn_data->chip->irq_mask(dir_conn_data);
}

static int msm_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	const struct msm_pingroup *g;
	irq_hw_number_t irq = 0;
	unsigned long flags;
	u32 val;

	if (d->parent_data) {
		if (pctrl->n_dir_conns > 0) {
			if (type == IRQ_TYPE_EDGE_BOTH)
				add_dirconn_tlmm(d, pctrl);
			else if (is_gpio_dual_edge(d, &irq))
				remove_dirconn_tlmm(d, irq);
		}
		irq_chip_set_type_parent(d, type);
	}

	if (test_bit(d->hwirq, pctrl->skip_wake_irqs))
		return 0;

	g = &pctrl->soc->groups[d->hwirq];

	raw_spin_lock_irqsave(&pctrl->lock, flags);

	/*
	 * For hw without possibility of detecting both edges
	 */
	if (g->intr_detection_width == 1 && type == IRQ_TYPE_EDGE_BOTH)
		set_bit(d->hwirq, pctrl->dual_edge_irqs);
	else
		clear_bit(d->hwirq, pctrl->dual_edge_irqs);

	/* Route interrupts to application cpu */
	val = msm_readl_intr_target(pctrl, g);
	val &= ~(7 << g->intr_target_bit);
	val |= g->intr_target_kpss_val << g->intr_target_bit;
	msm_writel_intr_target(val, pctrl, g);

	/* Update configuration for gpio.
	 * RAW_STATUS_EN is left on for all gpio irqs. Due to the
	 * internal circuitry of TLMM, toggling the RAW_STATUS
	 * could cause the INTR_STATUS to be set for EDGE interrupts.
	 */
	val = msm_readl_intr_cfg(pctrl, g);
	val |= BIT(g->intr_raw_status_bit);
	if (g->intr_detection_width == 2) {
		val &= ~(3 << g->intr_detection_bit);
		val &= ~(1 << g->intr_polarity_bit);
		switch (type) {
		case IRQ_TYPE_EDGE_RISING:
			val |= 1 << g->intr_detection_bit;
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_EDGE_FALLING:
			val |= 2 << g->intr_detection_bit;
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_EDGE_BOTH:
			val |= 3 << g->intr_detection_bit;
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_LEVEL_LOW:
			break;
		case IRQ_TYPE_LEVEL_HIGH:
			val |= BIT(g->intr_polarity_bit);
			break;
		}
	} else if (g->intr_detection_width == 1) {
		val &= ~(1 << g->intr_detection_bit);
		val &= ~(1 << g->intr_polarity_bit);
		switch (type) {
		case IRQ_TYPE_EDGE_RISING:
			val |= BIT(g->intr_detection_bit);
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_EDGE_FALLING:
			val |= BIT(g->intr_detection_bit);
			break;
		case IRQ_TYPE_EDGE_BOTH:
			val |= BIT(g->intr_detection_bit);
			val |= BIT(g->intr_polarity_bit);
			break;
		case IRQ_TYPE_LEVEL_LOW:
			break;
		case IRQ_TYPE_LEVEL_HIGH:
			val |= BIT(g->intr_polarity_bit);
			break;
		}
	} else {
		BUG();
	}
	msm_writel_intr_cfg(val, pctrl, g);

	if (test_bit(d->hwirq, pctrl->dual_edge_irqs))
		msm_gpio_update_dual_edge_pos(pctrl, g, d);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	if (type & (IRQ_TYPE_LEVEL_LOW | IRQ_TYPE_LEVEL_HIGH))
		irq_set_handler_locked(d, handle_level_irq);
	else if (type & (IRQ_TYPE_EDGE_FALLING | IRQ_TYPE_EDGE_RISING))
		irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static int msm_gpio_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	unsigned long flags;

	if (d->parent_data)
		irq_chip_set_wake_parent(d, on);

	/*
	 * While they may not wake up when the TLMM is powered off,
	 * some GPIOs would like to wakeup the system from suspend
	 * when TLMM is powered on. To allow that, enable the GPIO
	 * summary line to be wakeup capable at GIC.
	 */
	raw_spin_lock_irqsave(&pctrl->lock, flags);

	irq_set_irq_wake(pctrl->irq, on);

	raw_spin_unlock_irqrestore(&pctrl->lock, flags);

	return 0;
}

static int msm_gpio_irq_set_affinity(struct irq_data *d,
				const struct cpumask *dest, bool force)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);

	if (d->parent_data && test_bit(d->hwirq, pctrl->skip_wake_irqs))
		return irq_chip_set_affinity_parent(d, dest, force);

	return 0;
}

static int msm_gpio_irq_set_vcpu_affinity(struct irq_data *d, void *vcpu_info)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);

	if (d->parent_data && test_bit(d->hwirq, pctrl->skip_wake_irqs))
		return irq_chip_set_vcpu_affinity_parent(d, vcpu_info);

	return 0;
}

static void msm_gpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	const struct msm_pingroup *g;
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int irq_pin;
	int handled = 0;
	u32 val;
	int i;

	chained_irq_enter(chip, desc);

	/*
	 * Each pin has it's own IRQ status register, so use
	 * enabled_irq bitmap to limit the number of reads.
	 */
	for_each_set_bit(i, pctrl->enabled_irqs, pctrl->chip.ngpio) {
		g = &pctrl->soc->groups[i];
		val = msm_readl_intr_status(pctrl, g);
		if (val & BIT(g->intr_status_bit)) {
			irq_pin = irq_find_mapping(gc->irq.domain, i);
			generic_handle_irq(irq_pin);
			handled++;
		}
	}

	/* No interrupts were flagged */
	if (handled == 0)
		handle_bad_irq(desc);

	chained_irq_exit(chip, desc);
}

static int msm_gpio_wakeirq(struct gpio_chip *gc,
			    unsigned int child,
			    unsigned int child_type,
			    unsigned int *parent,
			    unsigned int *parent_type)
{
	struct msm_pinctrl *pctrl = gpiochip_get_data(gc);
	const struct msm_gpio_wakeirq_map *map;
	int i;

	*parent = GPIO_NO_WAKE_IRQ;
	*parent_type = IRQ_TYPE_EDGE_RISING;

	for (i = 0; i < pctrl->soc->nwakeirq_map; i++) {
		map = &pctrl->soc->wakeirq_map[i];
		if (map->gpio == child) {
			*parent = map->wakeirq;
			break;
		}
	}

	return 0;
}

static bool msm_gpio_needs_valid_mask(struct msm_pinctrl *pctrl)
{
	if (pctrl->soc->reserved_gpios)
		return true;

	return device_property_count_u16(pctrl->dev, "gpios") > 0;
}

static int msm_gpio_init(struct msm_pinctrl *pctrl)
{
	struct gpio_chip *chip;
	struct gpio_irq_chip *girq;
	int ret;
	unsigned ngpio = pctrl->soc->ngpios;
	struct device_node *dn;

	if (WARN_ON(ngpio > MAX_NR_GPIO))
		return -EINVAL;

	chip = &pctrl->chip;
	chip->base = -1;
	chip->ngpio = ngpio;
	chip->label = dev_name(pctrl->dev);
	chip->parent = pctrl->dev;
	chip->owner = THIS_MODULE;
	chip->of_node = pctrl->dev->of_node;
	if (msm_gpio_needs_valid_mask(pctrl))
		chip->init_valid_mask = msm_gpio_init_valid_mask;

	pctrl->irq_chip.name = "msmgpio";
	pctrl->irq_chip.irq_enable = msm_gpio_irq_enable;
	pctrl->irq_chip.irq_disable = msm_gpio_irq_disable;
	pctrl->irq_chip.irq_mask = msm_gpio_irq_mask;
	pctrl->irq_chip.irq_unmask = msm_gpio_irq_unmask;
	pctrl->irq_chip.irq_ack = msm_gpio_irq_ack;
	pctrl->irq_chip.irq_eoi = irq_chip_eoi_parent;
	pctrl->irq_chip.irq_set_type = msm_gpio_irq_set_type;
	pctrl->irq_chip.irq_set_wake = msm_gpio_irq_set_wake;
	pctrl->irq_chip.irq_set_affinity = msm_gpio_irq_set_affinity;
	pctrl->irq_chip.irq_set_vcpu_affinity = msm_gpio_irq_set_vcpu_affinity;

	dn = of_parse_phandle(pctrl->dev->of_node, "wakeup-parent", 0);
	if (dn) {
		int i;
		bool skip;
		unsigned int gpio;

		chip->irq.parent_domain = irq_find_matching_host(dn,
						 DOMAIN_BUS_WAKEUP);
		of_node_put(dn);
		if (!chip->irq.parent_domain)
			return -EPROBE_DEFER;
		chip->irq.child_to_parent_hwirq = msm_gpio_wakeirq;

		skip = irq_domain_qcom_handle_wakeup(chip->irq.parent_domain);
		for (i = 0; skip && i < pctrl->soc->nwakeirq_map; i++) {
			gpio = pctrl->soc->wakeirq_map[i].gpio;
			set_bit(gpio, pctrl->skip_wake_irqs);
		}
	}

	girq = &chip->irq;
	girq->chip = &pctrl->irq_chip;
	girq->parent_handler = msm_gpio_irq_handler;
	girq->fwnode = pctrl->dev->fwnode;
	girq->num_parents = 1;
	girq->fwnode = pctrl->dev->fwnode;
	girq->parents = devm_kcalloc(pctrl->dev, 1, sizeof(*girq->parents),
				     GFP_KERNEL);
	if (!girq->parents)
		return -ENOMEM;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_bad_irq;
	girq->parents[0] = pctrl->irq;

	ret = gpiochip_add_data(&pctrl->chip, pctrl);
	if (ret) {
		dev_err(pctrl->dev, "Failed register gpiochip\n");
		return ret;
	}

	/*
	 * For DeviceTree-supported systems, the gpio core checks the
	 * pinctrl's device node for the "gpio-ranges" property.
	 * If it is present, it takes care of adding the pin ranges
	 * for the driver. In this case the driver can skip ahead.
	 *
	 * In order to remain compatible with older, existing DeviceTree
	 * files which don't set the "gpio-ranges" property or systems that
	 * utilize ACPI the driver has to call gpiochip_add_pin_range().
	 */
	if (!of_property_read_bool(pctrl->dev->of_node, "gpio-ranges")) {
		ret = gpiochip_add_pin_range(&pctrl->chip,
			dev_name(pctrl->dev), 0, 0, chip->ngpio);
		if (ret) {
			dev_err(pctrl->dev, "Failed to add pin range\n");
			gpiochip_remove(&pctrl->chip);
			return ret;
		}
	}

	return 0;
}

static int msm_ps_hold_restart(struct notifier_block *nb, unsigned long action,
				   void *data)
{
	struct msm_pinctrl *pctrl = container_of(nb, struct msm_pinctrl, restart_nb);

	writel(0, pctrl->regs[0] + PS_HOLD_OFFSET);
	mdelay(1000);
	return NOTIFY_DONE;
}

static struct msm_pinctrl *poweroff_pctrl;

static void msm_ps_hold_poweroff(void)
{
	msm_ps_hold_restart(&poweroff_pctrl->restart_nb, 0, NULL);
}

static void msm_pinctrl_setup_pm_reset(struct msm_pinctrl *pctrl)
{
	int i;
	const struct msm_function *func = pctrl->soc->functions;

	for (i = 0; i < pctrl->soc->nfunctions; i++)
		if (!strcmp(func[i].name, "ps_hold")) {
			pctrl->restart_nb.notifier_call = msm_ps_hold_restart;
			pctrl->restart_nb.priority = 128;
			if (register_restart_handler(&pctrl->restart_nb))
				dev_err(pctrl->dev,
					"failed to setup restart handler.\n");
			poweroff_pctrl = pctrl;
			pm_power_off = msm_ps_hold_poweroff;
			break;
		}
}

static __maybe_unused int msm_pinctrl_suspend(struct device *dev)
{
	struct msm_pinctrl *pctrl = dev_get_drvdata(dev);

	return pinctrl_force_sleep(pctrl->pctrl);
}

static __maybe_unused int msm_pinctrl_resume(struct device *dev)
{
	struct msm_pinctrl *pctrl = dev_get_drvdata(dev);

	return pinctrl_force_default(pctrl->pctrl);
}

SIMPLE_DEV_PM_OPS(msm_pinctrl_dev_pm_ops, msm_pinctrl_suspend,
		  msm_pinctrl_resume);

EXPORT_SYMBOL(msm_pinctrl_dev_pm_ops);

int msm_qup_write(u32 mode, u32 val)
{
	int i;
	struct pinctrl_qup *regs = msm_pinctrl_data->soc->qup_regs;
	int num_regs = msm_pinctrl_data->soc->nqup_regs;

	/*Iterate over modes*/
	for (i = 0; i < num_regs; i++) {
		if (regs[i].mode == mode) {
			writel_relaxed(val & QUP_MASK,
				 msm_pinctrl_data->regs[0] + regs[i].offset);
			return 0;
		}
	}

	return -ENOENT;
}
EXPORT_SYMBOL(msm_qup_write);

int msm_qup_read(unsigned int mode)
{
	int i, val;
	struct pinctrl_qup *regs = msm_pinctrl_data->soc->qup_regs;
	int num_regs = msm_pinctrl_data->soc->nqup_regs;

	/*Iterate over modes*/
	for (i = 0; i < num_regs; i++) {
		if (regs[i].mode == mode) {
			val = readl_relaxed(msm_pinctrl_data->regs[0] +
								regs[i].offset);
			return val & QUP_MASK;
		}
	}

	return -ENOENT;
}

/*
 * msm_gpio_mpm_wake_set - API to make interrupt wakeup capable
 * @dev:        Device corrsponding to pinctrl
 * @gpio:       Gpio number to make interrupt wakeup capable
 * @enable:     Enable/Disable wakeup capability
 */
int msm_gpio_mpm_wake_set(unsigned int gpio, bool enable)
{
	const struct msm_pingroup *g;
	unsigned long flags;
	u32 val;

	g = &msm_pinctrl_data->soc->groups[gpio];
	if (g->wake_bit == -1)
		return -ENOENT;

	raw_spin_lock_irqsave(&msm_pinctrl_data->lock, flags);
	val = readl_relaxed(msm_pinctrl_data->regs[g->tile] + g->wake_reg);
	if (enable)
		val |= BIT(g->wake_bit);
	else
		val &= ~BIT(g->wake_bit);

	writel_relaxed(val, msm_pinctrl_data->regs[g->tile] + g->wake_reg);
	raw_spin_unlock_irqrestore(&msm_pinctrl_data->lock, flags);

	return 0;
}
EXPORT_SYMBOL(msm_gpio_mpm_wake_set);

int msm_pinctrl_probe(struct platform_device *pdev,
			  const struct msm_pinctrl_soc_data *soc_data)
{
	struct msm_pinctrl *pctrl;
	struct resource *res;
	int ret, i, num_irq, irq;

	msm_pinctrl_data = pctrl = devm_kzalloc(&pdev->dev, sizeof(*pctrl),
						GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	pctrl->dev = &pdev->dev;
	pctrl->soc = soc_data;
	pctrl->chip = msm_gpio_template;

	raw_spin_lock_init(&pctrl->lock);

	if (soc_data->tiles) {
		for (i = 0; i < soc_data->ntiles; i++) {
			res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							   soc_data->tiles[i]);
			pctrl->regs[i] = devm_ioremap_resource(&pdev->dev, res);
			if (IS_ERR(pctrl->regs[i]))
				return PTR_ERR(pctrl->regs[i]);
		}
	} else {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		pctrl->regs[0] = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pctrl->regs[0]))
			return PTR_ERR(pctrl->regs[0]);
	}

	msm_pinctrl_setup_pm_reset(pctrl);

	pctrl->irq = platform_get_irq(pdev, 0);
	if (pctrl->irq < 0)
		return pctrl->irq;

	pctrl->desc.owner = THIS_MODULE;
	pctrl->desc.pctlops = &msm_pinctrl_ops;
	pctrl->desc.pmxops = &msm_pinmux_ops;
	pctrl->desc.confops = &msm_pinconf_ops;
	pctrl->desc.name = dev_name(&pdev->dev);
	pctrl->desc.pins = pctrl->soc->pins;
	pctrl->desc.npins = pctrl->soc->npins;

	pctrl->pctrl = devm_pinctrl_register(&pdev->dev, &pctrl->desc, pctrl);
	if (IS_ERR(pctrl->pctrl)) {
		dev_err(&pdev->dev, "Couldn't register pinctrl driver\n");
		return PTR_ERR(pctrl->pctrl);
	}

	ret = msm_gpio_init(pctrl);
	if (ret)
		return ret;

	num_irq = platform_irq_count(pdev);

	for (i = 1; i < num_irq; i++) {
		struct msm_dir_conn *dc = &soc_data->dir_conn[i];

		irq = platform_get_irq(pdev, i);
		dc->irq = irq;
		__irq_set_handler(irq, msm_gpio_dirconn_handler, false, NULL);
		irq_set_irq_type(irq, IRQ_TYPE_EDGE_RISING);
		pctrl->n_dir_conns++;
	}

	platform_set_drvdata(pdev, pctrl);

	dev_dbg(&pdev->dev, "Probed Qualcomm pinctrl driver\n");

	/*zte_pm add for zte_gpio debug, begin*/
#ifdef CONFIG_DEBUG_FS
	gpio_status_debug_init(&pctrl->chip);
#endif
	/*zte_pm add for zte_gpio debug, end*/

	return 0;
}
EXPORT_SYMBOL(msm_pinctrl_probe);

int msm_pinctrl_remove(struct platform_device *pdev)
{
	struct msm_pinctrl *pctrl = platform_get_drvdata(pdev);

	gpiochip_remove(&pctrl->chip);

	unregister_restart_handler(&pctrl->restart_nb);

	return 0;
}
EXPORT_SYMBOL(msm_pinctrl_remove);

MODULE_SOFTDEP("pre: qcom-pdc");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. pinctrl-msm driver");
MODULE_LICENSE("GPL v2");
