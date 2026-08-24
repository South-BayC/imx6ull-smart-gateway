// SPDX-License-Identifier: GPL-2.0
/* icm20608：ICM20608 六轴 IMU IIO 驱动（P7-4，参考正点原子 27_iio）
 * 框架: spi_driver + regmap(SPI, read_flag_mask=0x80) + iio_dev（sysfs 接口）
 * 通道: in_temp_raw(带 OFFSET/SCALE) / in_anglvel_{x,y,z}_raw / in_accel_{x,y,z}_raw
 *
 * 板级经验（沿用 P4-B misc 版）:
 *   - WHO_AM_I 校验 0xAF/0xAE（ICM20608G/D）
 *   - 量程 probe 固定 ±2000dps / ±16G（正点原子板级验证值）
 *   - SPI 读时序由 regmap read_flag_mask=0x80 处理（地址 bit7=1 为读）
 *
 * 相对正点原子例程的修正:
 *   - probe 顺序：regmap 初始化先于 iio_device_register（例程相反，
 *     存在 sysfs 早于 regmap 就绪的隐患）
 *   - accel 通道 scan_index 顺序修正为 X/Y/Z
 *   - 温度 OFFSET 修正：raw=0 对应 25℃，offset=25000/scale≈8170
 *     （例程 offset=0 导致温度少 25℃）
 *
 * 用户态: /sys/bus/iio/devices/iio:deviceX/{in_accel_x_raw, in_temp_raw, ...}
 */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#define ICM20608_NAME "icm20608"

/* 寄存器（正点原子 icm20608reg.h） */
#define ICM20_SMPLRT_DIV	0x19	/* 采样率分频 */
#define ICM20_CONFIG		0x1A	/* 陀螺仪低通 */
#define ICM20_GYRO_CONFIG	0x1B	/* 陀螺仪量程 */
#define ICM20_ACCEL_CONFIG	0x1C	/* 加速度计量程 */
#define ICM20_ACCEL_CONFIG2	0x1D	/* 加速度计低通 */
#define ICM20_LP_MODE_CFG	0x1E	/* 低功耗 */
#define ICM20_PWR_MGMT_2	0x6C	/* 轴使能 */
#define ICM20_ACCEL_XOUT_H	0x3B	/* 加速度 X 高字节（0x3B~0x40） */
#define ICM20_TEMP_OUT_H	0x41	/* 温度高字节 */
#define ICM20_GYRO_XOUT_H	0x43	/* 陀螺仪 X 高字节（0x43~0x48） */
#define ICM20_PWR_MGMT_1	0x6B	/* 电源管理 1 */
#define ICM20_WHO_AM_I		0x75	/* 芯片 ID */

#define ICM20608_ID_G		0xAF
#define ICM20608_ID_D		0xAE

#define ICM20_PWR_RESET		0x80
#define ICM20_PWR_CLK_PLL	0x01
#define ICM20_GYRO_2000DPS	0x18
#define ICM20_ACCEL_16G		0x18

/* 温度换算：℃ = raw / 326.8 + 25
 * IIO 公式 temp_milli = (raw + OFFSET) * SCALE：
 *   SCALE  = 1000/326.8 ≈ 3.06 milli℃/raw
 *   OFFSET = 25000/3.06 ≈ 8170 raw
 * 验证 raw=0 → 25000 milli = 25℃ ✓ */
#define ICM20608_TEMP_SCALE_NUM		3
#define ICM20608_TEMP_SCALE_DEN		60000	/* 3.06（INT_PLUS_MICRO） */
#define ICM20608_TEMP_OFFSET		8170

/* 扫描元素索引 */
enum icm20608_scan {
	ICM_SCAN_ACCL_X,
	ICM_SCAN_ACCL_Y,
	ICM_SCAN_ACCL_Z,
	ICM_SCAN_TEMP,
	ICM_SCAN_GYRO_X,
	ICM_SCAN_GYRO_Y,
	ICM_SCAN_GYRO_Z,
};

struct icm20608_dev {
	struct spi_device *spi;
	struct regmap *regmap;
	struct mutex lock;
};

/* 陀螺仪分辨率表（250/500/1000/2000dps → ×1e6），对应 GYRO_CONFIG[4:3] */
static const int gyro_scale_tbl[] = { 7629, 15258, 30517, 61035 };
/* 加速度计分辨率表（2/4/8/16g → ×1e9），对应 ACCEL_CONFIG[4:3] */
static const int accel_scale_tbl[] = { 61035, 122070, 244140, 488281 };

/* 三轴通道宏（陀螺仪/加速度计共用） */
#define ICM20608_CHAN(_type, _channel2, _index)			\
	{							\
		.type = _type,					\
		.modified = 1,					\
		.channel2 = _channel2,				\
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
		.scan_index = _index,				\
	}

static const struct iio_chan_spec icm20608_channels[] = {
	{	/* 温度 */
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_OFFSET) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = ICM_SCAN_TEMP,
	},
	ICM20608_CHAN(IIO_ANGL_VEL, IIO_MOD_X, ICM_SCAN_GYRO_X),	/* 陀螺仪 X */
	ICM20608_CHAN(IIO_ANGL_VEL, IIO_MOD_Y, ICM_SCAN_GYRO_Y),	/* 陀螺仪 Y */
	ICM20608_CHAN(IIO_ANGL_VEL, IIO_MOD_Z, ICM_SCAN_GYRO_Z),	/* 陀螺仪 Z */
	ICM20608_CHAN(IIO_ACCEL, IIO_MOD_X, ICM_SCAN_ACCL_X),		/* 加速度 X */
	ICM20608_CHAN(IIO_ACCEL, IIO_MOD_Y, ICM_SCAN_ACCL_Y),		/* 加速度 Y */
	ICM20608_CHAN(IIO_ACCEL, IIO_MOD_Z, ICM_SCAN_ACCL_Z),		/* 加速度 Z */
};

/* 读 16bit 有符号通道数据（regmap_bulk_read 2 字节大端） */
static int icm20608_sensor_show(struct icm20608_dev *dev, u8 reg, int axis,
				int *val)
{
	__be16 d;
	int ret;

	ret = regmap_bulk_read(dev->regmap, reg + (axis - IIO_MOD_X) * 2,
			       (u8 *)&d, 2);
	if (ret)
		return -EINVAL;
	*val = (short)be16_to_cpup(&d);
	return IIO_VAL_INT;
}

/* 读取通道原始值 */
static int icm20608_read_channel_data(struct iio_dev *indio_dev,
				      struct iio_chan_spec const *chan,
				      int *val)
{
	struct icm20608_dev *dev = iio_priv(indio_dev);

	switch (chan->type) {
	case IIO_ANGL_VEL:
		return icm20608_sensor_show(dev, ICM20_GYRO_XOUT_H,
					    chan->channel2, val);
	case IIO_ACCEL:
		return icm20608_sensor_show(dev, ICM20_ACCEL_XOUT_H,
					    chan->channel2, val);
	case IIO_TEMP:
		return icm20608_sensor_show(dev, ICM20_TEMP_OUT_H,
					    IIO_MOD_X, val);
	default:
		return -EINVAL;
	}
}

static int icm20608_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct icm20608_dev *dev = iio_priv(indio_dev);
	unsigned int regdata;
	int ret = 0;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&dev->lock);
		ret = icm20608_read_channel_data(indio_dev, chan, val);
		mutex_unlock(&dev->lock);
		return ret;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ANGL_VEL:	/* 陀螺仪量程（dps ×1e6） */
			mutex_lock(&dev->lock);
			ret = regmap_read(dev->regmap, ICM20_GYRO_CONFIG, &regdata);
			mutex_unlock(&dev->lock);
			if (ret)
				return ret;
			*val = 0;
			*val2 = gyro_scale_tbl[(regdata & 0x18) >> 3];
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_ACCEL:		/* 加速度量程（g ×1e9） */
			mutex_lock(&dev->lock);
			ret = regmap_read(dev->regmap, ICM20_ACCEL_CONFIG, &regdata);
			mutex_unlock(&dev->lock);
			if (ret)
				return ret;
			*val = 0;
			*val2 = accel_scale_tbl[(regdata & 0x18) >> 3];
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_TEMP:		/* 3.06 milli℃/raw */
			*val = ICM20608_TEMP_SCALE_NUM;
			*val2 = ICM20608_TEMP_SCALE_DEN;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:	/* raw=0 → 25℃ */
		if (chan->type != IIO_TEMP)
			return -EINVAL;
		*val = ICM20608_TEMP_OFFSET;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static const struct iio_info icm20608_info = {
	.read_raw		= icm20608_read_raw,
};

static int icm20608_probe(struct spi_device *spi)
{
	int ret;
	unsigned int who;
	struct icm20608_dev *dev;
	struct iio_dev *indio_dev;
	struct regmap_config regmap_config = { 0 };

	/* 1、申请 iio_dev 内存 */
	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*dev));
	if (!indio_dev)
		return -ENOMEM;

	dev = iio_priv(indio_dev);
	dev->spi = spi;
	spi_set_drvdata(spi, indio_dev);
	mutex_init(&dev->lock);

	/* 2、regmap(SPI)：读掩码 0x80（ICM20608 SPI 读时地址 bit7=1） */
	regmap_config.reg_bits = 8;
	regmap_config.val_bits = 8;
	regmap_config.read_flag_mask = 0x80;
	dev->regmap = regmap_init_spi(spi, &regmap_config);
	if (IS_ERR(dev->regmap))
		return PTR_ERR(dev->regmap);

	/* 3、spi_device：MODE0 */
	spi->mode = SPI_MODE_0;
	spi_setup(spi);

	/* 4、芯片初始化：软复位 → PLL → WHO_AM_I 校验 → 量程/滤波 */
	regmap_write(dev->regmap, ICM20_PWR_MGMT_1, ICM20_PWR_RESET);
	msleep(50);
	regmap_write(dev->regmap, ICM20_PWR_MGMT_1, ICM20_PWR_CLK_PLL);
	msleep(50);

	ret = regmap_read(dev->regmap, ICM20_WHO_AM_I, &who);
	if (ret || (who != ICM20608_ID_G && who != ICM20608_ID_D)) {
		dev_err(&spi->dev, "WHO_AM_I mismatch: 0x%02x\n", who);
		regmap_exit(dev->regmap);
		return ret ? ret : -ENODEV;
	}
	dev_info(&spi->dev, "ICM20608 WHO_AM_I = 0x%02x\n", who);

	regmap_write(dev->regmap, ICM20_SMPLRT_DIV, 0x00);	/* 内部采样率 */
	regmap_write(dev->regmap, ICM20_GYRO_CONFIG, ICM20_GYRO_2000DPS);	/* ±2000dps */
	regmap_write(dev->regmap, ICM20_ACCEL_CONFIG, ICM20_ACCEL_16G);		/* ±16G */
	regmap_write(dev->regmap, ICM20_CONFIG, 0x04);		/* 陀螺低通 20Hz */
	regmap_write(dev->regmap, ICM20_ACCEL_CONFIG2, 0x04);	/* 加速度低通 21.2Hz */
	regmap_write(dev->regmap, ICM20_PWR_MGMT_2, 0x00);	/* 全轴使能 */
	regmap_write(dev->regmap, ICM20_LP_MODE_CFG, 0x00);	/* 关低功耗 */

	/* 5、iio_dev 成员（regmap 就绪后再注册 sysfs） */
	indio_dev->dev.parent = &spi->dev;
	indio_dev->info = &icm20608_info;
	indio_dev->name = ICM20608_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = icm20608_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm20608_channels);

	/* 6、注册 iio_dev */
	ret = iio_device_register(indio_dev);
	if (ret) {
		dev_err(&spi->dev, "iio_device_register failed\n");
		regmap_exit(dev->regmap);
		return ret;
	}

	dev_info(&spi->dev, "ICM20608 IIO ready\n");
	return 0;
}

static int icm20608_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct icm20608_dev *dev = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);
	regmap_exit(dev->regmap);
	return 0;
}

static const struct spi_device_id icm20608_id[] = {
	{ "alientek,icm20608", 0 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(spi, icm20608_id);

static const struct of_device_id icm20608_of_match[] = {
	{ .compatible = "alientek,icm20608" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

static struct spi_driver icm20608_driver = {
	.probe  = icm20608_probe,
	.remove = icm20608_remove,
	.driver = {
		.name = ICM20608_NAME,
		.of_match_table = icm20608_of_match,
	},
	.id_table = icm20608_id,
};
module_spi_driver(icm20608_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ALIENTEK / SouthBay");
MODULE_DESCRIPTION("ICM20608 gyro/accel/temp IIO driver");
