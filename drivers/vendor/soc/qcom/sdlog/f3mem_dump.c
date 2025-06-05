/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/notifier.h>
#include <soc/qcom/subsystem_restart.h>
#include <soc/qcom/subsystem_notif.h>
#include <soc/qcom/ramdump.h>
#include <soc/qcom/scm.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include "f3mem_dump.h"
#include <vendor/soc/qcom/sdlog_mem_reserve.h>
#include <linux/utsname.h>


#define LINUX_VERSION_ABOVE_FOUR          '4'

#ifndef SMEM_MODEM
#define SMEM_MODEM	1
#endif

/* Macros */
#define MAX_SSR_REASON_LEN	4096U

static void *f3mem_ramdump_dev;
static struct ramdump_segment *f3mem_ramdump_segments;

static void *imem_ramdump_dev;
static struct ramdump_segment *imem_ramdump_segment;

static void *modem_fatal_error_ramdump_dev;
static struct ramdump_segment *modem_fatal_error_ramdump_segments;

static struct device *f3mem_dev;

static char reason[MAX_SSR_REASON_LEN];

struct restart_notifier_block {
	unsigned processor;
	char *name;
	struct notifier_block nb;
};

static int f3mem_restart_notifier_cb(struct notifier_block *this,
		unsigned long code,
		void *data);
static int imem_restart_notifier_cb(struct notifier_block *this,
		unsigned long code,
		void *data);

static struct restart_notifier_block f3mem_restart_notifiers[] = {
	{SMEM_MODEM, "modem", .nb.notifier_call = f3mem_restart_notifier_cb},
};

static struct restart_notifier_block imem_restart_notifiers[] = {
	{SMEM_MODEM, "modem", .nb.notifier_call = imem_restart_notifier_cb},
};

void modem_fatal_error_update_reason(char *modem_crash_reason)
{
	snprintf(reason, MAX_SSR_REASON_LEN,
		"modem subsystem failure reason: %s.\n", modem_crash_reason);
}

static int do_modem_crash_info_dump(struct notif_data *notifdata)
{
	int ret = 0;

	if (!(modem_fatal_error_ramdump_dev &&
			notifdata->enable_ramdump))
		return ret;
	pr_info("%s: saving ramdump\n", __func__);
	if (modem_fatal_error_ramdump_segments->v_address &&
			modem_fatal_error_ramdump_segments->size != 0) {
		/*
		 * XPU protection does not currently allow the
		 * auxiliary memory regions to be dumped.  If this
		 * changes, then num_smem_areas + 1 should be passed
		 * into do_elf_ramdump() to dump all regions.
		 */
		ret = do_ramdump(modem_fatal_error_ramdump_dev,
				modem_fatal_error_ramdump_segments, 1);
		if (ret < 0)
			pr_err("%s: unable to dump modem crash %d\n",
				__func__, ret);
	}
	return ret;
}

static int imem_restart_notifier_cb(struct notifier_block *this,
		unsigned long code,
		void *data)
{
	struct restart_notifier_block *notifier;
	struct notif_data *notifdata = data;
	int ret;

	pr_info("imem_restart_notifier_cb: code 0x%lx\n", code);

	switch (code) {

	case SUBSYS_AFTER_SHUTDOWN:
		notifier = container_of(this,
					struct restart_notifier_block, nb);
		pr_info("%s: ssrestart for processor %d ('%s')\n",
			__func__, notifier->processor,
			notifier->name);
		break;
	case SUBSYS_RAMDUMP_NOTIFICATION:
		if (!(imem_ramdump_dev && notifdata->enable_ramdump))
			break;
		pr_info("%s: saving ramdump\n", __func__);

		/*
		 * XPU protection does not currently allow the
		 * auxiliary memory regions to be dumped.  If this
		 * changes, then num_smem_areas + 1 should be passed
		 * into do_elf_ramdump() to dump all regions.
		 */
		ret = do_ramdump(imem_ramdump_dev,
				 imem_ramdump_segment, 1);
		if (ret < 0)
			pr_err("%s: unable to dump smem %d\n", __func__, ret);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static int f3mem_restart_notifier_cb(struct notifier_block *this,
		unsigned long code,
		void *data)
{
	struct restart_notifier_block *notifier;
	struct notif_data *notifdata = data;
	int ret;

	pr_info("f3mem_restart_notifier_cb: code 0x%lx\n", code);

	switch (code) {

	case SUBSYS_AFTER_SHUTDOWN:
		notifier = container_of(this,
				struct restart_notifier_block, nb);
		pr_info("%s: ssrestart for processor %d ('%s')\n",
			__func__, notifier->processor,
			notifier->name);
		break;
	case SUBSYS_RAMDUMP_NOTIFICATION:
		if (!(f3mem_ramdump_dev && notifdata->enable_ramdump))
			break;
		pr_info("%s: saving ramdump\n", __func__);
		if (f3mem_ramdump_segments->address &&
				f3mem_ramdump_segments->size != 0) {
			/*
			 * XPU protection does not currently allow the
			 * auxiliary memory regions to be dumped.  If this
			 * changes, then num_smem_areas + 1 should be passed
			 * into do_elf_ramdump() to dump all regions.
			 */

			ret = do_ramdump(f3mem_ramdump_dev,
					 f3mem_ramdump_segments, 1);
			if (ret < 0)
				pr_err("%s: unable to dump smem %d\n",
						__func__, ret);
		}

		ret = do_modem_crash_info_dump(notifdata);
		if (ret < 0)
			pr_err("%s: unable to dump modem crash info %d\n",
					__func__, ret);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static int __init msm_scan_dt_map_imem_for_ramdump(
	unsigned long node,
	const char *uname,
	int depth, void *data)
{
	int i;
	void *handle;
	struct restart_notifier_block *nb;

	const unsigned int *imem_prop;
	int imem_prop_len;
	int ret;
	const char *compat = "qcom,msm-imem";

	ret = of_flat_dt_is_compatible(node, compat);

	if (!ret)
		return -EFAULT;

	pr_info("imem_modem_restart_late_init\n");

	imem_prop = of_get_flat_dt_prop(node, "reg",
			&imem_prop_len);

	if (!imem_prop) {
		pr_err("IMEM reg field not found\n");
		return -EFAULT;
	}

	if (imem_prop_len != (2 * sizeof(u32))) {
		pr_err("IMEM range malformed\n");
		return -EFAULT;
	}

	imem_ramdump_dev = create_ramdump_device("imem", f3mem_dev);
	if (IS_ERR_OR_NULL(imem_ramdump_dev)) {
		pr_err("%s: Unable to create imem ramdump device.\n",
				__func__);
		imem_ramdump_dev = NULL;
	}

	imem_ramdump_segment = kcalloc(1,
			sizeof(struct ramdump_segment), GFP_KERNEL);
	if (!imem_ramdump_segment) {
		ret = -ENOMEM;
		return ret;
	}
	imem_ramdump_segment->address = be32_to_cpu(imem_prop[0]);
	imem_ramdump_segment->size = be32_to_cpu(imem_prop[1]);
	imem_ramdump_segment->v_address = 0;
	pr_info("msm_scan_dt_map_imem_for_ramdump: v_address 0x%lx, address 0x%lx, size 0x%lx\n",
			(unsigned long int)imem_ramdump_segment->v_address,
			(unsigned long int)imem_ramdump_segment->address,
			imem_ramdump_segment->size);

	for (i = 0; i < ARRAY_SIZE(imem_restart_notifiers); i++) {
		nb = &imem_restart_notifiers[i];
		handle = subsys_notif_register_notifier(nb->name, &nb->nb);
		pr_info("%s: registering notif for '%s', handle=%p\n",
				__func__, nb->name, handle);
	}

	return 0;
}

static int f3mem_init(void)
{
	unsigned int sdlog_phy_addr;
	int sdlog_memory_size;
	int rc;

	pr_info("%s\n", __func__);

	f3mem_ramdump_segments = kcalloc(1,
			sizeof(struct ramdump_segment_local), GFP_KERNEL);
	if (!f3mem_ramdump_segments) {
		rc = -ENOMEM;
		return rc;
	}

	sdlog_phy_addr = sdlog_memory_get_addr();
	sdlog_memory_size = sdlog_memory_get_size();

	if (sdlog_phy_addr && sdlog_memory_size) {
		f3mem_ramdump_segments->address = sdlog_phy_addr;
		f3mem_ramdump_segments->size = sdlog_memory_size;
	} else {
		f3mem_ramdump_segments->address = 0;
		f3mem_ramdump_segments->size = 0;
		f3mem_ramdump_segments->v_address = 0;
	}

	pr_info("%s success f3mem address 0x%lx size 0x%lx\n",
			__func__,
			f3mem_ramdump_segments->address,
			f3mem_ramdump_segments->size);
	return 0;
}

static int modem_fatal_error_log_init(void)
{
	modem_fatal_error_ramdump_dev =
		create_ramdump_device("crashinfo", NULL);
	if (IS_ERR_OR_NULL(modem_fatal_error_ramdump_dev)) {
		pr_err("%s: Unable to create modem crash ramdump device.\n",
				__func__);
		modem_fatal_error_ramdump_dev = NULL;
		return -ENOMEM;
	}

	modem_fatal_error_ramdump_segments = kcalloc(1,
			sizeof(struct ramdump_segment), GFP_KERNEL);
	if (!modem_fatal_error_ramdump_segments)
		return -ENOMEM;

	modem_fatal_error_ramdump_segments->size = sizeof(reason);
	modem_fatal_error_ramdump_segments->v_address = reason;

	return 0;
}

static __init int f3mem_modem_restart_late_init(void)
{
	int i;
	void *handle;
	struct restart_notifier_block *nb;

	pr_info("f3mem_modem_restart_late_init\n");


	f3mem_ramdump_dev = create_ramdump_device("f3mem", f3mem_dev);
	if (IS_ERR_OR_NULL(f3mem_ramdump_dev)) {
		pr_err("%s: Unable to create f3mem ramdump device.\n",
				__func__);
		f3mem_ramdump_dev = NULL;
	}

	for (i = 0; i < ARRAY_SIZE(f3mem_restart_notifiers); i++) {
		nb = &f3mem_restart_notifiers[i];
		handle = subsys_notif_register_notifier(nb->name, &nb->nb);
		pr_info("%s: registering notif for '%s', handle=%p\n",
				__func__, nb->name, handle);
	}

	f3mem_init();
	modem_fatal_error_log_init();

	of_scan_flat_dt(msm_scan_dt_map_imem_for_ramdump, NULL);

	return 0;
}

late_initcall(f3mem_modem_restart_late_init);




static int vendor_f3mem_probe(struct platform_device *pdev)
{
	pr_info("vendor_f3mem_probe\n");
	f3mem_dev = &pdev->dev;
	return 0;
}



static struct of_device_id vendor_f3_match_table[] = {
	{ .compatible = "vendor,f3mem" },
	{},
};

static struct platform_driver vendor_f3mem_driver = {
	.probe = vendor_f3mem_probe,
	.driver = {
		.name = "vendor_f3mem",
		.owner = THIS_MODULE,
		.of_match_table = vendor_f3_match_table,
	},
};


int __init vendor_f3mem_init(void)
{
	int rc;

	pr_info("vendor_f3mem_init\n");
	rc = platform_driver_register(&vendor_f3mem_driver);
	if (rc) {
		pr_err("%s: vendor_f3mem_driver register failed %d\n",
							__func__, rc);
		return rc;
	}

	return 0;
}


arch_initcall(vendor_f3mem_init);

MODULE_DESCRIPTION("F3MEM Subsystem Dump Driver");
MODULE_LICENSE("GPL v2");
