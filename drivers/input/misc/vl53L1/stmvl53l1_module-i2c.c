/**************************************************************************
 * Copyright (c) 2016, STMicroelectronics - All Rights Reserved

 License terms: BSD 3-clause "New" or "Revised" License.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice,
 this list of conditions and the following disclaimer in the documentation
 and/or other materials provided with the distribution.

 3. Neither the name of the copyright holder nor the names of its contributors
 may be used to endorse or promote products derived from this software
 without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ****************************************************************************/

/**
 * @file stmvl53l1_module-i2c.c
 *
 *  implement STM VL53L1 module interface i2c wrapper + control
 *  using linux native i2c + gpio + reg api
 */
#include <linux/uaccess.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/miscdevice.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/platform_device.h>
#include <linux/version.h>

/*
 * power specific includes
 */
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/pinctrl/consumer.h>
#include <linux/clk.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>

#include "stmvl53l1-i2c.h"
#include "stmvl53l1.h"

#define STMVL53L1_SLAVE_ADDR	(0x52>>1)

/** @ingroup drv_port
 * @{
 */

/**
 * control specific debug message echo
 *
 * Set to 0/1 do not remove
 *
 * most dbg warn err messages goes true main driver macro
 * this one permit some specific debug without activating all main dbg
 */
#define MODI2C_DEBUG	0

/*
 * mutex to handle device i2c address changes. It allow to avoid multiple
 * device active with same i2c addresses at the same time. Note that we don't
 * support case where boot_reg has the same value as a final i2c address of
 * another device.
 */
static DEFINE_MUTEX(dev_addr_change_mutex);

/**
 * i2c client assigned to our driver
 *
 * this is use for stm test purpose as we fake client create and regstration
 * we stores the i2c client for release in clean-up overwise we wan't reload
 * the module multiple time
 *
 * in a normal dev tree prod system this is not required
 */
static struct i2c_client *stm_test_i2c_client;

/*
 * pi3:
 * insmod stmvl53l1.ko force_device=1 adapter_nb=1 xsdn_gpio_nb=19
 * intr_gpio_nb=16 pwren_gpio_nb=12
 *
 * panda
 * insmod stmvl53l1.ko force_device=1 adapter_nb=4 xsdn_gpio_nb=56
 * intr_gpio_nb=59 pwren_gpio_nb=55
 */

static int force_device;
static int adapter_nb = -1;
static int xsdn_gpio_nb = -1;
static int pwren_gpio_nb = -1;
static int intr_gpio_nb = -1;

module_param(force_device, int, 0000);
MODULE_PARM_DESC(force_device, "force device insertion at module init");

module_param(adapter_nb, int, 0000);
MODULE_PARM_DESC(adapter_nb, "i2c adapter to use");

module_param(xsdn_gpio_nb, int, 0000);
MODULE_PARM_DESC(xsdn_gpio_nb, "select gpio numer to use for vl53l1 reset");

module_param(pwren_gpio_nb, int, 0000);
MODULE_PARM_DESC(pwren_gpio_nb, "select gpio numer to use for vl53l1 power");

module_param(intr_gpio_nb, int, 0000);
MODULE_PARM_DESC(intr_gpio_nb, "select gpio numer to use for vl53l1 interrupt");

/**
 * warn message
 *
 * @warning use only in scope where i2c_data ptr is present
 **/
#define modi2c_warn(fmt, ...)\
	dev_WARN(&i2c_data->client->dev, fmt, ##__VA_ARGS__)

/**
 * err message
 *
 * @warning use only in scope where i2c_data ptr is present
 */
#define modi2c_err(fmt, ...)\
	dev_err(&i2c_data->client->dev, fmt, ##__VA_ARGS__)



#if MODI2C_DEBUG
#	define modi2c_dbg(fmt, ...)\
		pr_devel("%s "fmt"\n", __func__, ##__VA_ARGS__)
#else
#	define modi2c_dbg(...)	(void)0
#endif

static int insert_device(void)
{
	int ret = 0;
	struct i2c_adapter *adapter;
	struct i2c_board_info info = {
		.type = "stmvl53l1",
		.addr = STMVL53L1_SLAVE_ADDR,
	};

	memset(&info, 0, sizeof(info));
	strlcpy(info.type, "stmvl53l1", sizeof(info.type));
	info.addr = STMVL53L1_SLAVE_ADDR;
	adapter = i2c_get_adapter(adapter_nb);
	if (!adapter) {
		ret = -EINVAL;
		goto done;
	}
	stm_test_i2c_client = i2c_new_device(adapter, &info);
	if (!stm_test_i2c_client)
		ret = -EINVAL;

done:
	return ret;
}

static int get_xsdn(struct device *dev, struct i2c_data *i2c_data)
{
	int rc = 0;

	i2c_data->io_flag.xsdn_owned = 0;
	if (i2c_data->xsdn_gpio == -1) {
		vl53l1_errmsg("reset gpio is required");
		rc = -ENODEV;
		goto no_gpio;
	}

	vl53l1_dbgmsg("request xsdn_gpio %d", i2c_data->xsdn_gpio);
	rc = gpio_request(i2c_data->xsdn_gpio, "vl53l1_xsdn");
	if (rc) {
		vl53l1_errmsg("fail to acquire xsdn %d", rc);
		goto request_failed;
	}

	rc = gpio_direction_output(i2c_data->xsdn_gpio, 0);
	if (rc) {
		vl53l1_errmsg("fail to configure xsdn as output %d", rc);
		goto direction_failed;
	}
	i2c_data->io_flag.xsdn_owned = 1;

	return rc;

direction_failed:
	gpio_free(i2c_data->xsdn_gpio);

request_failed:
no_gpio:
	return rc;
}

static void put_xsdn(struct i2c_data *i2c_data)
{
	if (i2c_data->io_flag.xsdn_owned) {
		vl53l1_dbgmsg("release xsdn_gpio %d", i2c_data->xsdn_gpio);
		gpio_free(i2c_data->xsdn_gpio);
		i2c_data->io_flag.xsdn_owned = 0;
		i2c_data->xsdn_gpio = -1;
	}
	i2c_data->xsdn_gpio = -1;
}

static int get_pwren(struct device *dev, struct i2c_data *i2c_data)
{
	int rc = 0;

	i2c_data->io_flag.pwr_owned = 0;
	if (i2c_data->pwren_gpio == -1) {
		vl53l1_wanrmsg("pwren gpio disable");
		goto no_gpio;
	}

	vl53l1_dbgmsg("request pwren_gpio %d", i2c_data->pwren_gpio);
	rc = gpio_request(i2c_data->pwren_gpio, "vl53l1_pwren");
	if (rc) {
		vl53l1_errmsg("fail to acquire pwren %d", rc);
		goto request_failed;
	}

	rc = gpio_direction_output(i2c_data->pwren_gpio, 0);
	if (rc) {
		vl53l1_errmsg("fail to configure pwren as output %d", rc);
		goto direction_failed;
	}
	i2c_data->io_flag.pwr_owned = 1;

	return rc;

direction_failed:
	gpio_free(i2c_data->xsdn_gpio);

request_failed:
no_gpio:
	return rc;
}

static void put_pwren(struct i2c_data *i2c_data)
{
	if (i2c_data->io_flag.pwr_owned) {
		vl53l1_dbgmsg("release pwren_gpio %d", i2c_data->pwren_gpio);
		gpio_free(i2c_data->pwren_gpio);
		i2c_data->io_flag.pwr_owned = 0;
		i2c_data->pwren_gpio = -1;
	}
	i2c_data->pwren_gpio = -1;
}

int pinctrl_init(struct device *dev, struct dev_pinctrl_info *pinctrl_info)
{

	pinctrl_info->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(pinctrl_info->pinctrl)) {
		vl53l1_errmsg("Getting pinctrl handle failed");
		return -EINVAL;
	}
	pinctrl_info->gpio_state_active =
		pinctrl_lookup_state(pinctrl_info->pinctrl, "laser_default");
	if (IS_ERR_OR_NULL(pinctrl_info->gpio_state_active)) {
		vl53l1_errmsg("Failed to get the active state pinctrl handle");
		return -EINVAL;
	}
	pinctrl_info->gpio_state_suspend
		= pinctrl_lookup_state(pinctrl_info->pinctrl, "laser_suspend");
	if (IS_ERR_OR_NULL(pinctrl_info->gpio_state_suspend)) {
		vl53l1_errmsg("Failed to get the suspend state pinctrl handle");
		return -EINVAL;
	}

	return 0;
}

static int get_intr(struct device *dev, struct i2c_data *i2c_data)
{
	int rc = 0;

	i2c_data->io_flag.intr_owned = 0;
	if (i2c_data->intr_gpio == -1) {
		vl53l1_wanrmsg("no interrupt gpio");
		goto no_gpio;
	}


	rc = pinctrl_init(dev, &(i2c_data->pinctrl_info));
	if (rc != 0) {
		vl53l1_errmsg("Initialization of pinctrl failed");
		i2c_data->pinctrl_status = 0;
	} else {
		i2c_data->pinctrl_status = 1;
	}

	vl53l1_dbgmsg("request intr_gpio %d", i2c_data->intr_gpio);
	rc = gpio_request(i2c_data->intr_gpio, "vl53l1_intr");
	if (rc) {
		vl53l1_errmsg("fail to acquire intr %d", rc);
		goto request_failed;
	}

	rc = gpio_direction_input(i2c_data->intr_gpio);
	if (rc) {
		vl53l1_errmsg("fail to configure intr as input %d", rc);
		goto direction_failed;
	}

	i2c_data->irq = gpio_to_irq(i2c_data->intr_gpio);
	if (i2c_data->irq < 0) {
		vl53l1_errmsg("fail to map GPIO: %d to interrupt:%d\n",
				i2c_data->intr_gpio, i2c_data->irq);
		goto irq_failed;
	}
	i2c_data->io_flag.intr_owned = 1;

	if (i2c_data->pinctrl_status) {
		rc = pinctrl_select_state(
			i2c_data->pinctrl_info.pinctrl,
			i2c_data->pinctrl_info.gpio_state_active);
		if (rc)
			vl53l1_errmsg("cannot set pin to active state");
	}

	return rc;

irq_failed:
direction_failed:
	gpio_free(i2c_data->intr_gpio);

request_failed:
no_gpio:
	return rc;
}

static void put_intr(struct i2c_data *i2c_data)
{
	if (i2c_data->io_flag.intr_owned) {
		if (i2c_data->io_flag.intr_started) {
			free_irq(i2c_data->irq, i2c_data);
			i2c_data->io_flag.intr_started = 0;
		}
		vl53l1_dbgmsg("release intr_gpio %d", i2c_data->intr_gpio);
		gpio_free(i2c_data->intr_gpio);
		i2c_data->io_flag.intr_owned = 0;
	}
	i2c_data->intr_gpio = -1;

	if (i2c_data->pinctrl_status) {
		int ret = pinctrl_select_state(
			i2c_data->pinctrl_info.pinctrl,
			i2c_data->pinctrl_info.gpio_state_suspend);
		if (ret)
			vl53l1_errmsg("cannot set pin to suspend state");

		devm_pinctrl_put(i2c_data->pinctrl_info.pinctrl);
	}
	i2c_data->pinctrl_status = 0;

}


static int get_dt_regulator_info(struct device *dev, struct i2c_data *i2c_data)
{
	int rc = 0, count = 0, i = 0;
	struct device_node *of_node = NULL;

	if (!dev) {
		vl53l1_errmsg("Invalid parameters");
		return -EINVAL;
	}

	of_node = dev->of_node;

	count = of_property_count_strings(of_node, "regulator-names");
	if (count != -EINVAL) {
		if (count <= 0) {
			vl53l1_errmsg("no regulators found");
			count = 0;
			return -EINVAL;
		}

		i2c_data->num_rgltr = count;

	} else {
		vl53l1_errmsg("No regulators node found");
		return 0;
	}

	for (i = 0; i < count; i++) {
		rc = of_property_read_string_index(of_node,
			"regulator-names", i, &i2c_data->rgltr_name[i]);
		vl53l1_errmsg("rgltr_name[%d] = %s",
			i, i2c_data->rgltr_name[i]);
		if (rc) {
			vl53l1_errmsg("no regulator resource at cnt=%d", i);
			return -ENODEV;
		}
	}

	if (!of_property_read_bool(of_node, "rgltr-cntrl-support")) {
		vl53l1_dbgmsg("No regulator control parameter defined");
		i2c_data->rgltr_ctrl_support = false;
		return 0;
	}

	i2c_data->rgltr_ctrl_support = true;

	rc = of_property_read_u32_array(of_node, "rgltr-min-voltage",
		i2c_data->rgltr_min_volt, i2c_data->num_rgltr);
	if (rc) {
		vl53l1_errmsg("No minimum volatage value found, rc=%d", rc);
		return -EINVAL;
	}

	rc = of_property_read_u32_array(of_node, "rgltr-max-voltage",
		i2c_data->rgltr_max_volt, i2c_data->num_rgltr);
	if (rc) {
		vl53l1_errmsg("No maximum volatage value found, rc=%d", rc);
		return -EINVAL;
	}

	rc = of_property_read_u32_array(of_node, "rgltr-load-current",
		i2c_data->rgltr_op_mode, i2c_data->num_rgltr);
	if (rc) {
		vl53l1_errmsg("No Load curent found rc=%d", rc);
		return -EINVAL;
	}

	for (i = 0; i < count; i++) {
		i2c_data->rgltr[i] = regulator_get(dev, i2c_data->rgltr_name[i]);
		if (IS_ERR(i2c_data->rgltr[i])) {
			vl53l1_errmsg("%s get failed", i2c_data->rgltr_name[i]);
			i2c_data->rgltr[i] = NULL;
			return -EINVAL;
		}
	}

	return rc;
}


static int soc_util_regulator_enable(struct regulator *rgltr,
	const char *rgltr_name,
	uint32_t rgltr_min_volt, uint32_t rgltr_max_volt,
	uint32_t rgltr_op_mode)
{
	int32_t rc = 0;

	if (!rgltr) {
		vl53l1_errmsg("Invalid NULL parameter");
		return -EINVAL;
	}

	if (regulator_count_voltages(rgltr) > 0) {
		vl53l1_dbgmsg("voltage min=%d, max=%d",
			rgltr_min_volt, rgltr_max_volt);

		rc = regulator_set_voltage(
			rgltr, rgltr_min_volt, rgltr_max_volt);
		if (rc) {
			vl53l1_errmsg("%s set voltage failed", rgltr_name);
			return rc;
		}

		rc = regulator_set_load(rgltr, rgltr_op_mode);
		if (rc) {
			vl53l1_errmsg("%s set optimum mode failed",
				rgltr_name);
			return rc;
		}
	}

	rc = regulator_enable(rgltr);
	if (rc) {
		vl53l1_errmsg("%s regulator_enable failed", rgltr_name);
		return rc;
	}

	return rc;
}

static int soc_util_regulator_disable(struct regulator *rgltr,
	const char *rgltr_name, uint32_t rgltr_min_volt,
	uint32_t rgltr_max_volt, uint32_t rgltr_op_mode)
{
	int32_t rc = 0;

	if (!rgltr) {
		vl53l1_errmsg("Invalid NULL parameter");
		return -EINVAL;
	}

	rc = regulator_disable(rgltr);
	if (rc) {
		vl53l1_errmsg("%s regulator disable failed", rgltr_name);
		return rc;
	}

	if (regulator_count_voltages(rgltr) > 0) {
		regulator_set_load(rgltr, 0);
		regulator_set_voltage(rgltr, 0, rgltr_max_volt);
	}

	return rc;
}


/**
 *  parse dev tree for all platform specific input
 */
static int stmvl53l1_parse_tree(struct device *dev, struct i2c_data *i2c_data)
{
	int rc = 0;

	/* if force device is in use then gpio nb comes from module param else
	 * we use devicetree.
	 */
	i2c_data->vdd = NULL;
	i2c_data->pwren_gpio = -1;
	i2c_data->xsdn_gpio = -1;
	i2c_data->intr_gpio = -1;
	i2c_data->boot_reg = STMVL53L1_SLAVE_ADDR;
	if (force_device) {
		i2c_data->xsdn_gpio = xsdn_gpio_nb;
		i2c_data->pwren_gpio = pwren_gpio_nb;
		i2c_data->intr_gpio = intr_gpio_nb;
	} else if (dev->of_node) {
		/* power : either vdd or pwren_gpio. try reulator first */
		rc = get_dt_regulator_info(dev, i2c_data);
		if (rc != 0) {
			i2c_data->vdd = NULL;
			/* try gpio */
			rc = of_property_read_u32_array(dev->of_node,
				"pwren-gpio", &i2c_data->pwren_gpio, 1);
			if (rc) {
				i2c_data->pwren_gpio = -1;
				vl53l1_wanrmsg(
			"no regulator, nor power gpio => power ctrl disabled");
			}
		}

		i2c_data->xsdn_gpio = of_get_named_gpio_flags(dev->of_node, "xsdn-gpio", 0, NULL);
		if (!gpio_is_valid(i2c_data->xsdn_gpio)) {
			vl53l1_wanrmsg("Unable to find xsdn_gpio %d", i2c_data->xsdn_gpio);
			i2c_data->xsdn_gpio = -1;
		}

		i2c_data->intr_gpio = of_get_named_gpio_flags(dev->of_node, "intr-gpio", 0, NULL);
		if (!gpio_is_valid(i2c_data->intr_gpio)) {
			vl53l1_wanrmsg("Unable to find intr-gpio %d", i2c_data->intr_gpio);
			i2c_data->intr_gpio = -1;
		}

		rc = of_property_read_u32_array(dev->of_node, "boot-reg",
			&i2c_data->boot_reg, 1);
		if (rc) {
			vl53l1_wanrmsg("Unable to find boot-reg %d %d",
				rc, i2c_data->boot_reg);
			i2c_data->boot_reg = STMVL53L1_SLAVE_ADDR;
		}
	}

	/* configure gpios */
	rc = get_xsdn(dev, i2c_data);
	if (rc)
		goto no_xsdn;
	rc = get_pwren(dev, i2c_data);
	if (rc)
		goto no_pwren;
	rc = get_intr(dev, i2c_data);
	if (rc)
		goto no_intr;

	return rc;

no_intr:
	if (i2c_data->vdd) {
		regulator_put(i2c_data->vdd);
		i2c_data->vdd = NULL;
	}
	put_pwren(i2c_data);
no_pwren:
	put_xsdn(i2c_data);
no_xsdn:
	return rc;
}

static void stmvl53l1_release_gpios(struct i2c_data *i2c_data)
{
	put_xsdn(i2c_data);
	if (i2c_data->vdd) {
		regulator_put(i2c_data->vdd);
		i2c_data->vdd = NULL;
	}
	put_pwren(i2c_data);
	put_intr(i2c_data);
}

static int stmvl53l1_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	int rc = 0;
	struct stmvl53l1_data *vl53l1_data = NULL;
	struct i2c_data *i2c_data = NULL;

	vl53l1_dbgmsg("Enter %s : 0x%02x\n", client->name, client->addr);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE)) {
		rc = -EIO;
		return rc;
	}

	vl53l1_data = kzalloc(sizeof(struct stmvl53l1_data), GFP_KERNEL);
	if (!vl53l1_data) {
		rc = -ENOMEM;
		return rc;
	}
	if (vl53l1_data) {
		vl53l1_data->client_object =
				kzalloc(sizeof(struct i2c_data), GFP_KERNEL);
		if (!vl53l1_data)
			goto done_freemem;
		i2c_data = (struct i2c_data *)vl53l1_data->client_object;
	}
	i2c_data->client = client;
	i2c_data->vl53l1_data = vl53l1_data;
	i2c_data->irq = -1 ; /* init to no irq */

	/* parse and configure hardware */
	rc = stmvl53l1_parse_tree(&i2c_data->client->dev, i2c_data);
	if (rc)
		goto done_freemem;

	/* setup device name */
	/* vl53l1_data->dev_name = dev_name(&client->dev); */

	/* setup client data */
	i2c_set_clientdata(client, vl53l1_data);

	/* end up by core driver setup */
	rc = stmvl53l1_setup(vl53l1_data);
	if (rc)
		goto release_gpios;
	vl53l1_dbgmsg("End\n");

	kref_init(&i2c_data->ref);

	return rc;

release_gpios:
	stmvl53l1_release_gpios(i2c_data);

done_freemem:
	/* kfree safe against NULL */
	kfree(vl53l1_data);
	kfree(i2c_data);

	return -EINVAL;
}

static int stmvl53l1_remove(struct i2c_client *client)
{
	struct stmvl53l1_data *data = i2c_get_clientdata(client);
	struct i2c_data *i2c_data = (struct i2c_data *)data->client_object;

	vl53l1_dbgmsg("Enter\n");
	mutex_lock(&data->work_mutex);
	/* main driver cleanup */
	stmvl53l1_cleanup(data);

	/* release gpios */
	stmvl53l1_release_gpios(i2c_data);

	mutex_unlock(&data->work_mutex);

	stmvl53l1_put(data->client_object);

	vl53l1_dbgmsg("End\n");

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int stmvl53l1_suspend(struct device *dev)
{
	struct stmvl53l1_data *data = i2c_get_clientdata(to_i2c_client(dev));

	vl53l1_dbgmsg("Enter\n");
	mutex_lock(&data->work_mutex);
	/* Stop ranging */
	stmvl53l1_pm_suspend_stop(data);

	mutex_unlock(&data->work_mutex);

	vl53l1_dbgmsg("End\n");

	return 0;
}

static int stmvl53l1_resume(struct device *dev)
{
#if 0
	struct stmvl53l1_data *data = i2c_get_clientdata(to_i2c_client(dev));

	vl53l1_dbgmsg("Enter\n");

	mutex_lock(&data->work_mutex);

	/* do nothing user will restart measurements */

	mutex_unlock(&data->work_mutex);

	vl53l1_dbgmsg("End\n");
#else
	vl53l1_dbgmsg("Enter\n");
	vl53l1_dbgmsg("End\n");
#endif
	return 0;
}
#endif


static SIMPLE_DEV_PM_OPS(stmvl53l1_pm_ops, stmvl53l1_suspend, stmvl53l1_resume);

static const struct i2c_device_id stmvl53l1_id[] = {
	{ STMVL53L1_DRV_NAME, 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, stmvl53l1_id);

static const struct of_device_id st_stmvl53l1_dt_match[] = {
	{ .compatible = "st,"STMVL53L1_DRV_NAME, },
	{ },
};

static struct i2c_driver stmvl53l1_driver = {
	.driver = {
		.name	= STMVL53L1_DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = st_stmvl53l1_dt_match,
		.pm	= &stmvl53l1_pm_ops,
	},
	.probe	= stmvl53l1_probe,
	.remove	= stmvl53l1_remove,
	.id_table = stmvl53l1_id,

};

/**
 * give power to device
 *
 * @param object  the i2c layer object
 * @return
 */
int stmvl53l1_power_up_i2c(void *object)
{
	int rc = 0;
	int i;
	struct i2c_data *data = (struct i2c_data *) object;

	vl53l1_dbgmsg("Enter\n");

	/* turn on power */
	if (data->num_rgltr) {
		for (i = 0; i < data->num_rgltr; i++) {
			rc =  soc_util_regulator_enable(
				data->rgltr[i],
				data->rgltr_name[i],
				data->rgltr_min_volt[i],
				data->rgltr_max_volt[i],
				data->rgltr_op_mode[i]);
			if (rc) {
				vl53l1_errmsg("fail to turn on %s regulator, rc = %d", data->rgltr_name[i], rc);
				return rc;
			}
		}
	} else if (data->pwren_gpio != -1) {
		gpio_set_value(data->pwren_gpio, 1);
		vl53l1_info("slow power on");
	} else
		vl53l1_wanrmsg("no power control");

	return rc;
}

/**
 * remove power to device (reset it)
 *
 * @param i2c_object the i2c layer object
 * @return 0 on success
 */
int stmvl53l1_power_down_i2c(void *i2c_object)
{
	struct i2c_data *data = (struct i2c_data *) i2c_object;
	int rc = 0;
	int i;

	vl53l1_dbgmsg("Enter\n");

	/* turn off power */
	if (data->num_rgltr) {
		for (i = 0; i < data->num_rgltr; i++) {
			rc =  soc_util_regulator_disable(
				data->rgltr[i],
				data->rgltr_name[i],
				data->rgltr_min_volt[i],
				data->rgltr_max_volt[i],
				data->rgltr_op_mode[i]);
			if (rc) {
				vl53l1_errmsg("%s reg disable failed, rc = %d", data->rgltr_name[i], rc);
				return rc;
			}
		}

		rc = regulator_disable(data->vdd);
		if (rc)
			vl53l1_errmsg("reg disable failed. rc=%d\n",
				rc);
	} else if (data->pwren_gpio != -1) {
		gpio_set_value(data->pwren_gpio, 0);
	}
	vl53l1_dbgmsg("power off");

	vl53l1_dbgmsg("End\n");

	return rc;
}

static int handle_i2c_address_device_change_lock(struct i2c_data *data)
{
	struct i2c_client *client = (struct i2c_client *) data->client;
	uint8_t buffer[3];
	struct i2c_msg msg;
	int rc = 0;

	vl53l1_dbgmsg("change device i2c address from 0x%02x to 0x%02x",
		data->boot_reg, client->addr);
	/* no i2c-access must occur before fw boot time */
	usleep_range(VL53L1_FIRMWARE_BOOT_TIME_US,
		VL53L1_FIRMWARE_BOOT_TIME_US + 1);

	/* manually send message to update i2c address */
	buffer[0] = (VL53L1_I2C_SLAVE__DEVICE_ADDRESS >> 8) & 0xFF;
	buffer[1] = (VL53L1_I2C_SLAVE__DEVICE_ADDRESS >> 0) & 0xFF;
	buffer[2] = client->addr;
	msg.addr = data->boot_reg;
	msg.flags = client->flags;
	msg.buf = buffer;
	msg.len = 3;
	if (i2c_transfer(client->adapter, &msg, 1) != 1) {
		rc = -ENXIO;
		vl53l1_errmsg("Fail to change i2c address to 0x%02x",
			client->addr);
	}

	return rc;
}

/* reset release will also handle device address change. It will avoid state
 * where multiple stm53l1 are bring out of reset at the same time with the
 * same boot address.
 * Note that we don't manage case where boot_reg has the same value as a final
 * i2c address of another device. This case is not supported and will lead
 * to unpredictable behavior.
 */
static int release_reset(struct i2c_data *data)
{
	struct i2c_client *client = (struct i2c_client *) data->client;
	int rc = 0;
	bool is_address_change = client->addr != data->boot_reg;

	if (is_address_change)
		mutex_lock(&dev_addr_change_mutex);

	gpio_set_value(data->xsdn_gpio, 1);
	if (is_address_change) {
		rc = handle_i2c_address_device_change_lock(data);
		if (rc)
			gpio_set_value(data->xsdn_gpio, 0);
	}

	if (is_address_change)
		mutex_unlock(&dev_addr_change_mutex);

	return rc;
}

/**
 * release device reset
 *
 * @param i2c_object the i2c layer object
 * @return 0 on success
 */
int stmvl53l1_reset_release_i2c(void *i2c_object)
{
	int rc;
	struct i2c_data *data = (struct i2c_data *) i2c_object;

	vl53l1_dbgmsg("Enter\n");

	rc = release_reset(data);
	if (rc)
		goto error;

	/* and now wait for device end of boot */
	data->vl53l1_data->is_delay_allowed = true;
	rc = VL53L1_WaitDeviceBooted(&data->vl53l1_data->stdev);
	data->vl53l1_data->is_delay_allowed = false;
	if (rc) {
		gpio_set_value(data->xsdn_gpio, 0);
		vl53l1_errmsg("boot fail with error %d", rc);
		data->vl53l1_data->last_error = rc;
		rc = -EIO;
	}

error:
	vl53l1_dbgmsg("End\n");

	return rc;
}

/**
 * put device under reset
 *
 * @param i2c_object the i2c layer object
 * @return 0 on success
 */
int stmvl53l1_reset_hold_i2c(void *i2c_object)
{
	struct i2c_data *data = (struct i2c_data *) i2c_object;

	vl53l1_dbgmsg("Enter\n");

	gpio_set_value(data->xsdn_gpio, 0);

	vl53l1_dbgmsg("End\n");

	return 0;
}

int stmvl53l1_init_i2c(void)
{
	int ret = 0;

	vl53l1_dbgmsg("Enter\n");

	/* register as a i2c client device */
	ret = i2c_add_driver(&stmvl53l1_driver);
	if (ret)
		vl53l1_errmsg("%d erro ret:%d\n", __LINE__, ret);

	if (!ret && force_device)
		ret = insert_device();

	if (ret)
		i2c_del_driver(&stmvl53l1_driver);

	vl53l1_dbgmsg("End with rc:%d\n", ret);

	return ret;
}


void stmvl53l1_clean_up_i2c(void)
{
	if (stm_test_i2c_client) {
		vl53l1_dbgmsg("to unregister i2c client\n");
		i2c_unregister_device(stm_test_i2c_client);
	}
}

static irqreturn_t stmvl53l1_irq_handler_i2c(int vec, void *info)
{
	struct i2c_data *i2c_data = (struct i2c_data *)info;

	if (i2c_data->irq == vec) {
		modi2c_dbg("irq");
		stmvl53l1_intr_handler(i2c_data->vl53l1_data);
		modi2c_dbg("over");
	} else {
		if (!i2c_data->msg_flag.unhandled_irq_vec) {
			modi2c_warn("unmatching vec %d != %d\n",
					vec, i2c_data->irq);
			i2c_data->msg_flag.unhandled_irq_vec = 1;
		}
	}

	return IRQ_HANDLED;
}

/**
 * enable and start intr handling
 *
 * @param object  our i2c_data specific object
 * @param poll_mode [in/out] set to force mode clear to use irq
 * @return 0 on success and set ->poll_mode if it fail ranging wan't start
 */
int stmvl53l1_start_intr(void *object, int *poll_mode)
{
	struct i2c_data *i2c_data;
	int rc;

	i2c_data = (struct i2c_data *)object;
	/* irq and gpio acquire config done in parse_tree */
	if (i2c_data->irq < 0) {
		/* the i2c tree as no intr force polling mode */
		*poll_mode = -1;
		return 0;
	}
	/* clear irq warning report enabe it again for this session */
	i2c_data->msg_flag.unhandled_irq_vec = 0;
	/* if started do no nothing */
	if (i2c_data->io_flag.intr_started) {
		/* nothing to do */
		*poll_mode = 0;
		return 0;
	}

	vl53l1_dbgmsg("to register_irq:%d\n", i2c_data->irq);
	rc = request_threaded_irq(i2c_data->irq, NULL,
			stmvl53l1_irq_handler_i2c,
			IRQF_TRIGGER_FALLING|IRQF_ONESHOT,
			"vl53l1_interrupt",
			(void *)i2c_data);
	if (rc) {
		vl53l1_errmsg("fail to req threaded irq rc=%d\n", rc);
		*poll_mode = 0;
	} else {
		vl53l1_dbgmsg("irq %d now handled\n", i2c_data->irq);
		i2c_data->io_flag.intr_started = 1;
		*poll_mode = 0;
	}
	return rc;
}

void *stmvl53l1_get(void *object)
{
	struct i2c_data *data = (struct i2c_data *) object;

	vl53l1_dbgmsg("Enter\n");
	kref_get(&data->ref);
	vl53l1_dbgmsg("End\n");

	return object;
}

static void memory_release(struct kref *kref)
{
	struct i2c_data *data = container_of(kref, struct i2c_data, ref);

	vl53l1_dbgmsg("Enter\n");
	kfree(data->vl53l1_data);
	kfree(data);
	vl53l1_dbgmsg("End\n");
}

void stmvl53l1_put(void *object)
{
	struct i2c_data *data = (struct i2c_data *) object;

	vl53l1_dbgmsg("Enter\n");
	kref_put(&data->ref, memory_release);
	vl53l1_dbgmsg("End\n");
}

void __exit stmvl53l1_exit_i2c(void *i2c_object)
{
	vl53l1_dbgmsg("Enter\n");
	i2c_del_driver(&stmvl53l1_driver);
	vl53l1_dbgmsg("End\n");
}
