// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_i2csens.c - AP3216C I2C sensor driver with regmap
 *
 * Uses regmap (RBTREE cache) instead of raw i2c_transfer for
 * register caching, auto-synchronization, and PM integration.
 *
 * This module is linked into indaq.ko together with indaq_core.o.
 * The I2C driver is registered via indaq_register_i2c_driver()
 * called from indaq_core's module init.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/of.h>
#include "indaq_core.h"
#include "indaq_i2csens.h"

/*
 * Volatile registers — always read the real hardware value.
 * Status and data registers change independently of writes.
 */
static bool ap3216c_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AP3216C_REG_STATUS:
	case AP3216C_REG_ALS_DATA_LO ... AP3216C_REG_PS_DATA_HI:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config ap3216c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x20,
	.cache_type = REGCACHE_RBTREE,
	.volatile_reg = ap3216c_volatile_reg,
};

/* ---------- Periodic reading via workqueue ---------- */

static void ap3216c_read_worker(struct work_struct *work)
{
	struct indaq_i2csens *sens = container_of(work,
					struct indaq_i2csens, read_work.work);
	unsigned int als_hi, als_lo, ps_hi, ps_lo, ir_hi, ir_lo;
	int ret;

	/* Read ALS data (2 bytes: hi + lo) */
	ret = regmap_read(sens->regmap, AP3216C_REG_ALS_DATA_HI, &als_hi);
	if (ret) {
		dev_warn(&sens->client->dev, "Failed to read ALS_HI\n");
		goto reschedule;
	}
	ret = regmap_read(sens->regmap, AP3216C_REG_ALS_DATA_LO, &als_lo);
	if (ret) {
		dev_warn(&sens->client->dev, "Failed to read ALS_LO\n");
		goto reschedule;
	}

	/* Read IR data (2 bytes: hi + lo) */
	ret = regmap_read(sens->regmap, AP3216C_REG_IR_DATA_HI, &ir_hi);
	if (ret) {
		dev_warn(&sens->client->dev, "Failed to read IR_HI\n");
		goto reschedule;
	}
	ret = regmap_read(sens->regmap, AP3216C_REG_IR_DATA_LO, &ir_lo);
	if (ret) {
		dev_warn(&sens->client->dev, "Failed to read IR_LO\n");
		goto reschedule;
	}

	/* Read PS data (2 bytes: hi + lo) */
	ret = regmap_read(sens->regmap, AP3216C_REG_PS_DATA_HI, &ps_hi);
	if (ret) {
		dev_warn(&sens->client->dev, "Failed to read PS_HI\n");
		goto reschedule;
	}
	ret = regmap_read(sens->regmap, AP3216C_REG_PS_DATA_LO, &ps_lo);
	if (ret) {
		dev_warn(&sens->client->dev, "Failed to read PS_LO\n");
		goto reschedule;
	}

	sens->als_value = (als_hi << 8) | als_lo;

	/* IR: check IR_OF (bit 7 of IR_LO), 10-bit value */
	if (ir_lo & AP3216C_IR_OF) {
		sens->ir_value = 0;
		dev_dbg(&sens->client->dev, "IR invalid (IR_OF)\n");
	} else {
		sens->ir_value = (ir_hi << AP3216C_IR_HI_SHIFT)
				| (ir_lo & AP3216C_IR_LO_MASK);
	}

	/* PS: check PS_IR_OF (bit 6 of PS_LO), 10-bit value */
	if (ps_lo & AP3216C_PS_IR_OF) {
		sens->ps_value = 0;
		dev_dbg(&sens->client->dev, "PS invalid (IR_OF)\n");
	} else {
		sens->ps_value = ((ps_hi & AP3216C_PS_HI_MASK)
				  << AP3216C_PS_HI_SHIFT)
				| (ps_lo & AP3216C_PS_LO_MASK);
	}

	/* Push sample into core ring buffer */
	indaq_push_sample(sens->als_value, sens->ps_value, sens->ir_value);

	dev_dbg(&sens->client->dev, "ALS=%u  PS=%u  IR=%u\n",
		sens->als_value, sens->ps_value, sens->ir_value);

reschedule:
	schedule_delayed_work(&sens->read_work,
			      msecs_to_jiffies(sens->read_interval_ms));
}

/* ---------- Global instance for core access ---------- */

static struct indaq_i2csens *g_ap3216c_sens;

/* Phase 3: called from indaq_core's read to get latest sensor data */
int indaq_i2csens_get_data(u16 *als, u16 *ps, u16 *ir)
{
	if (!g_ap3216c_sens)
		return -ENODEV;

	*als = g_ap3216c_sens->als_value;
	*ps  = g_ap3216c_sens->ps_value;
	*ir  = g_ap3216c_sens->ir_value;
	return 0;
}

/* Phase 5: dynamically change the I2C worker sampling interval */
int indaq_i2csens_set_interval(u32 interval_ms)
{
	if (!g_ap3216c_sens)
		return -ENODEV;

	if (interval_ms < 10 || interval_ms > 60000)
		return -EINVAL;

	g_ap3216c_sens->read_interval_ms = interval_ms;
	return 0;
}

/* ---------- I2C driver ---------- */

static int ap3216c_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct indaq_i2csens *sens;
	int ret;

	dev_info(&client->dev, "AP3216C probe started\n");

	sens = devm_kzalloc(&client->dev, sizeof(*sens), GFP_KERNEL);
	if (!sens)
		return -ENOMEM;

	sens->client = client;
	sens->read_interval_ms = AP3216C_DEFAULT_INTERVAL_MS;
	i2c_set_clientdata(client, sens);

	/* Initialize regmap with RBTREE cache */
	sens->regmap = devm_regmap_init_i2c(client, &ap3216c_regmap_config);
	if (IS_ERR(sens->regmap)) {
		ret = PTR_ERR(sens->regmap);
		dev_err(&client->dev, "Failed to init regmap: %d\n", ret);
		return ret;
	}

	/* Reset sensor */
	ret = regmap_write(sens->regmap, AP3216C_REG_SYS_CONFIG,
			   AP3216C_SYS_RESET);
	if (ret) {
		dev_err(&client->dev, "Failed to reset sensor: %d\n", ret);
		return ret;
	}
	msleep(10);

	/* Enable ALS + PS */
	ret = regmap_write(sens->regmap, AP3216C_REG_SYS_CONFIG,
			   AP3216C_SYS_ENABLE);
	if (ret) {
		dev_err(&client->dev, "Failed to enable sensor: %d\n", ret);
		return ret;
	}
	msleep(10);

	/* Start periodic reading */
	INIT_DELAYED_WORK(&sens->read_work, ap3216c_read_worker);
	schedule_delayed_work(&sens->read_work,
			      msecs_to_jiffies(sens->read_interval_ms));

	g_ap3216c_sens = sens;

	dev_info(&client->dev, "AP3216C initialized (regmap RBTREE cache)\n");
	return 0;
}

static int ap3216c_remove(struct i2c_client *client)
{
	struct indaq_i2csens *sens = i2c_get_clientdata(client);

	g_ap3216c_sens = NULL;

	cancel_delayed_work_sync(&sens->read_work);

	/* Disable sensor */
	regmap_write(sens->regmap, AP3216C_REG_SYS_CONFIG, 0x00);

	dev_info(&client->dev, "AP3216C removed\n");
	return 0;
}

static const struct i2c_device_id ap3216c_id[] = {
	{ "ap3216c", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ap3216c_id);

static const struct of_device_id ap3216c_of_match[] = {
	{ .compatible = "alientek,ap3216c" },
	{ .compatible = "sallenkey,ap3216c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

static struct i2c_driver ap3216c_i2c_driver = {
	.driver = {
		.name = "ap3216c",
		.of_match_table = ap3216c_of_match,
	},
	.probe    = ap3216c_probe,
	.remove   = ap3216c_remove,
	.id_table = ap3216c_id,
};

/* ---------- Registration helpers for core module ---------- */

int __init indaq_register_i2c_driver(void)
{
	return i2c_add_driver(&ap3216c_i2c_driver);
}

void indaq_unregister_i2c_driver(void)
{
	i2c_del_driver(&ap3216c_i2c_driver);
}

/*
 * INDAQ sub-device integration: called from indaq_core's probe/remove.
 *
 * Note: The I2C driver probes independently via device tree matching.
 * These functions just link the already-probed I2C device to the core
 * indaq_device structure for unified management (Phase 3+).
 */
int indaq_i2csens_init(struct indaq_device *indev)
{
	/*
	 * The AP3216C probes independently via i2c_driver matching.
	 * In later phases we'll link it to the core here.
	 */
	dev_info(indev->dev, "I2C sensor subsystem ready\n");
	return 0;
}

void indaq_i2csens_exit(struct indaq_device *indev)
{
	/* Cleanup handled by i2c_driver's remove */
	indev->i2csens = NULL;
}
