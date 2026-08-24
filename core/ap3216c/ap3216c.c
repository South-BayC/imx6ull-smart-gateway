// SPDX-License-Identifier: GPL-2.0
/* ap3216c：AP3216C 环境光/接近传感器 IIO 驱动（P7-4，参考正点原子 27_iio）
 * 框架: i2c_driver + regmap(I2C 8bit) + iio_dev（INDIO_DIRECT_MODE → sysfs 接口）
 * 通道: in_illuminance_both_raw(ALS 16bit, 带 SCALE 量程)
 *       in_proximity_raw(PS 10bit) / in_illuminance_ir_raw(IR 10bit)
 *
 * 板级经验（沿用 P4-B misc 版，勿丢）:
 *   - 逐字节读寄存器：曾用连续 bulk 读 2/6 字节，板端实测数据帧交替失效
 *     (IR_OF/PS_OF 置位 → 全 0 帧)，故 16bit 数据也拆两次单字节读；
 *   - PS 寄存器 0x0E/0x0F（手册骨架 0x10 系笔误），值 = (hi&0x3F)<<4 | (lo&0x0F)；
 *   - 溢出标志（IR: hi.bit7 / PS: hi.bit6）置位时对应值归 0；
 *   - probe 一次初始化（软复位 → 使能 ALS+PS+IR），不重复复位避免数据流中断。
 *
 * 用户态: /sys/bus/iio/devices/iio:deviceX/{in_illuminance_both_raw,
 *         in_proximity_raw, in_illuminance_ir_raw, in_illuminance_both_scale}
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define AP3216C_NAME "ap3216c"

/* 寄存器（正点原子 ap3216creg.h） */
#define AP3216C_SYS_CONF	0x00	/* 系统配置 */
#define AP3216C_IR_DATA_L	0x0A	/* IR 数据低字节 */
#define AP3216C_ALS_DATA_L	0x0C	/* ALS 数据低字节 */
#define AP3216C_PS_DATA_L	0x0E	/* PS 数据低字节 */
#define AP3216C_ALS_CONF	0x10	/* ALS 配置（量程 [5:4]） */

#define AP3216C_CONF_RESET	0x04	/* 软复位 */
#define AP3216C_CONF_ENABLE	0x03	/* 开启 ALS+PS+IR */

/* 扫描元素索引 */
enum ap3216c_scan {
	AP3216C_ALS,
	AP3216C_PS,
	AP3216C_IR,
};

/* ALS 量程表（lux，×1e6），对应 ALSCONFIG[5:4] = 0..3
 * 量程依次 0~20661 / 0~5162 / 0~1291 / 0~323 lux */
static const int als_scale_tbl[] = { 315000, 78800, 19700, 4900 };

struct ap3216c_dev {
	struct i2c_client *client;
	struct regmap *regmap;
	struct mutex lock;
};

/* 通道表：ALS(带量程) / PS / IR */
static const struct iio_chan_spec ap3216c_channels[] = {
	{	/* ALS 环境光（16bit，可设量程） */
		.type = IIO_INTENSITY,
		.modified = 1,
		.channel2 = IIO_MOD_LIGHT_BOTH,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = AP3216C_ALS,
	},
	{	/* PS 接近（10bit） */
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.scan_index = AP3216C_PS,
	},
	{	/* IR 红外（10bit） */
		.type = IIO_INTENSITY,
		.modified = 1,
		.channel2 = IIO_MOD_LIGHT_IR,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.scan_index = AP3216C_IR,
	},
};

/* 读 16bit 数据：拆两次单字节 regmap 读（板级验证，见文件头注释） */
static int ap3216c_read_data16(struct ap3216c_dev *d, u8 reg_low,
			       unsigned int *val)
{
	unsigned int lo, hi;
	int ret;

	ret = regmap_read(d->regmap, reg_low, &lo);
	if (ret)
		return ret;
	ret = regmap_read(d->regmap, reg_low + 1, &hi);
	if (ret)
		return ret;

	*val = (hi << 8) | lo;
	return 0;
}

/* 芯片初始化：软复位 → 使能 ALS+PS+IR（probe 时一次） */
static int ap3216c_init_chip(struct ap3216c_dev *d)
{
	regmap_write(d->regmap, AP3216C_SYS_CONF, AP3216C_CONF_RESET);
	msleep(50);		/* 复位最少 10ms，取 50ms 稳妥 */
	regmap_write(d->regmap, AP3216C_SYS_CONF, AP3216C_CONF_ENABLE);
	regmap_write(d->regmap, AP3216C_ALS_CONF, 0x00);	/* ALS 量程 0（最大） */
	return 0;
}

/* sysfs 读：IIO_CHAN_INFO_RAW / IIO_CHAN_INFO_SCALE */
static int ap3216c_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ap3216c_dev *dev = iio_priv(indio_dev);
	unsigned int data, regdata;
	int ret = 0;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&dev->lock);
		switch (chan->type) {
		case IIO_INTENSITY:
			if (chan->channel2 == IIO_MOD_LIGHT_BOTH) {
				/* ALS 16bit 原始值 */
				ret = ap3216c_read_data16(dev, AP3216C_ALS_DATA_L, &data);
				if (!ret)
					*val = (int)data;
			} else {
				/* IR 10bit：hi.bit7 溢出标志置位 → 归 0 */
				ret = ap3216c_read_data16(dev, AP3216C_IR_DATA_L, &data);
				if (!ret) {
					if (data & 0x8000)
						data = 0;
					*val = (int)((data >> 8) << 2) | (int)(data & 0x03);
				}
			}
			break;
		case IIO_PROXIMITY:
			/* PS 10bit：hi.bit6 溢出标志置位 → 归 0 */
			ret = ap3216c_read_data16(dev, AP3216C_PS_DATA_L, &data);
			if (!ret) {
				if (data & 0x4000)
					data = 0;
				*val = (int)(((data >> 8) & 0x3F) << 4) |
				       (int)(data & 0x0F);
			}
			break;
		default:
			ret = -EINVAL;
			break;
		}
		mutex_unlock(&dev->lock);
		return ret ? ret : IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:	/* ALS 量程（lux ×1e6） */
		if (chan->type != IIO_INTENSITY)
			return -EINVAL;
		mutex_lock(&dev->lock);
		ret = regmap_read(dev->regmap, AP3216C_ALS_CONF, &regdata);
		if (!ret) {
			*val = 0;
			*val2 = als_scale_tbl[(regdata & 0x30) >> 4];
		}
		mutex_unlock(&dev->lock);
		return ret ? ret : IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

/* sysfs 写：设置 ALS 量程（用户写 val2，单位 lux×1e6） */
static int ap3216c_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct ap3216c_dev *dev = iio_priv(indio_dev);
	int i, ret = -EINVAL;

	if (mask != IIO_CHAN_INFO_SCALE || chan->type != IIO_INTENSITY)
		return -EINVAL;

	mutex_lock(&dev->lock);
	for (i = 0; i < ARRAY_SIZE(als_scale_tbl); i++) {
		if (als_scale_tbl[i] == val2) {
			ret = regmap_write(dev->regmap, AP3216C_ALS_CONF,
					   (u8)(i << 4));
			break;
		}
	}
	mutex_unlock(&dev->lock);
	return ret;
}

/* 用户空间写数值格式 */
static int ap3216c_write_raw_get_fmt(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan,
				     long mask)
{
	return IIO_VAL_INT_PLUS_MICRO;
}

static const struct iio_info ap3216c_info = {
	.read_raw		= ap3216c_read_raw,
	.write_raw		= ap3216c_write_raw,
	.write_raw_get_fmt	= ap3216c_write_raw_get_fmt,
};

static int ap3216c_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	int ret;
	struct ap3216c_dev *dev;
	struct iio_dev *indio_dev;
	struct regmap_config regmap_config = { 0 };

	/* 1、申请 iio_dev 内存（私有数据随附） */
	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*dev));
	if (!indio_dev)
		return -ENOMEM;

	dev = iio_priv(indio_dev);
	dev->client = client;
	i2c_set_clientdata(client, indio_dev);

	/* 2、regmap(I2C)：8bit 寄存器 / 8bit 值 */
	regmap_config.reg_bits = 8;
	regmap_config.val_bits = 8;
	dev->regmap = regmap_init_i2c(client, &regmap_config);
	if (IS_ERR(dev->regmap))
		return PTR_ERR(dev->regmap);

	mutex_init(&dev->lock);

	/* 3、iio_dev 成员 */
	indio_dev->dev.parent = &client->dev;
	indio_dev->info = &ap3216c_info;
	indio_dev->name = AP3216C_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE;	/* sysfs 直接模式 */
	indio_dev->channels = ap3216c_channels;
	indio_dev->num_channels = ARRAY_SIZE(ap3216c_channels);

	/* 4、初始化芯片（先于注册，避免 sysfs 早于芯片就绪） */
	ap3216c_init_chip(dev);

	/* 5、注册 iio_dev */
	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&client->dev, "iio_device_register failed\n");
		regmap_exit(dev->regmap);
		return ret;
	}

	dev_info(&client->dev, "AP3216C IIO ready\n");
	return 0;
}

static int ap3216c_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ap3216c_dev *dev = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	regmap_exit(dev->regmap);
	return 0;
}

static const struct i2c_device_id ap3216c_id[] = {
	{ "alientek,ap3216c", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, ap3216c_id);

static const struct of_device_id ap3216c_of_match[] = {
	{ .compatible = "alientek,ap3216c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

static struct i2c_driver ap3216c_driver = {
	.probe  = ap3216c_probe,
	.remove = ap3216c_remove,
	.driver = {
		.name = AP3216C_NAME,
		.of_match_table = ap3216c_of_match,
	},
	.id_table = ap3216c_id,
};
module_i2c_driver(ap3216c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ALIENTEK / SouthBay");
MODULE_DESCRIPTION("AP3216C ALS/PS/IR IIO driver");
