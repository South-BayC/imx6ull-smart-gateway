// SPDX-License-Identifier: GPL-2.0
/* icm20608：ICM20608 六轴 IMU 驱动（P4-B，手册 5.13.3）
 * 数据流: spi_driver probe → spi_transfer 读写 → miscdevice
 *         read() → 突发读 0x3B~0x48 连续 14 字节 → 重排解出 7×s32
 * 要点: ICM20608 SPI 读：地址字节期间 MISO 输出无效字节，数据自其后有效，
 *       采用正点原子 22_spi 板级验证读法（单 transfer 多收 1 字节丢弃首位）；
 *       寄存器 0x3B 起顺序为 accel_x,y,z / temp / gyro_x,y,z，
 *       输出按 uapi 协议重排为 gyro_x,y,z / accel_x,y,z / temp；
 *       probe 时软复位→WHO_AM_I 校验(0xAF/0xAE)→量程配置；
 *       read 无锁（spi_sync_transfer 串行化，多轴数据单事务一致性）。
 * 参考: 正点原子 22_spi/icm20608.c（寄存器表 + 读时序 + 复位/配置序列）
 */
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/string.h>

#include "uapi/icm20608.h"

/* 寄存器（正点原子 icm20608reg.h） */
#define ICM20_SMPLRT_DIV		0x19	/* 采样率分频 */
#define ICM20_CONFIG			0x1A	/* 陀螺仪低通 */
#define ICM20_GYRO_CONFIG		0x1B	/* 陀螺仪量程 */
#define ICM20_ACCEL_CONFIG		0x1C	/* 加速度计量程 */
#define ICM20_ACCEL_CONFIG2		0x1D	/* 加速度计低通 */
#define ICM20_LP_MODE_CFG		0x1E	/* 低功耗 */
#define ICM20_FIFO_EN			0x23	/* FIFO 使能 */
#define ICM20_ACCEL_XOUT_H		0x3B	/* 加速度 X 高字节（突发读起点） */
#define ICM20_PWR_MGMT_1		0x6B	/* 电源管理 1（复位/时钟） */
#define ICM20_PWR_MGMT_2		0x6C	/* 电源管理 2（轴使能） */
#define ICM20_WHO_AM_I			0x75	/* 芯片 ID */

#define ICM20608_ID_G			0xAF	/* ICM20608G */
#define ICM20608_ID_D			0xAE	/* ICM20608D */

#define ICM20_PWR_RESET			0x80	/* 软复位 */
#define ICM20_PWR_CLK_PLL		0x01	/* 时钟源 PLL */
#define ICM20_GYRO_2000DPS		0x18	/* ±2000dps */
#define ICM20_ACCEL_16G			0x18	/* ±16G */
#define ICM20_LPF_20HZ			0x04	/* 陀螺仪低通 20Hz */
#define ICM20_ALPF_21HZ			0x04	/* 加速度计低通 21.2Hz */

#define ICM20_MAX_REG_CNT		14	/* 单次最大读写字节数（7×s16） */

struct icm20608_dev {
	struct spi_device *spi;
	struct miscdevice misc;
};

/* ICM20608 SPI 读时序：地址字节期间 MISO 输出无效字节（哑数据），
 * 数据自第 2 个字节起有效 → 多收 1 字节并丢弃首位（正点原子板级验证读法） */
static int icm20608_read_regs(struct icm20608_dev *d, u8 reg, u8 *buf, int len)
{
	u8 tx = reg | 0x80;			/* 读地址 bit7=1 */
	u8 rx[ICM20_MAX_REG_CNT + 1];	/* 多收 1 个无效字节 */
	struct spi_transfer t = {
		.tx_buf = &tx,
		.rx_buf = rx,
		.len    = len + 1,
	};
	int ret;

	if (len > ICM20_MAX_REG_CNT)
		return -EINVAL;

	ret = spi_sync_transfer(d->spi, &t, 1);
	if (ret)
		return ret;
	memcpy(buf, rx + 1, len);	/* 丢弃地址期间的无效字节 */
	return 0;
}

static int icm20608_read_reg(struct icm20608_dev *d, u8 reg, u8 *val)
{
	return icm20608_read_regs(d, reg, val, 1);
}

/* 写无哑字节问题：地址(bit7=0) + 数据 直接发送 */
static int icm20608_write_regs(struct icm20608_dev *d, u8 reg, u8 *buf, int len)
{
	u8 tx[1 + ICM20_MAX_REG_CNT];
	struct spi_transfer t = {
		.tx_buf = tx,
		.len    = 1 + len,
	};
	int ret;

	if (len > ICM20_MAX_REG_CNT)
		return -EINVAL;

	tx[0] = reg & ~0x80;		/* 写地址 bit7=0 */
	memcpy(tx + 1, buf, len);

	ret = spi_sync_transfer(d->spi, &t, 1);
	if (ret)
		return ret;
	return 0;
}

static int icm20608_write_reg(struct icm20608_dev *d, u8 reg, u8 val)
{
	return icm20608_write_regs(d, reg, &val, 1);
}

/* 软复位 → 上电 → WHO_AM_I 校验 → 量程/滤波配置（probe 时一次） */
static int icm20608_init_chip(struct icm20608_dev *d)
{
	struct device *dev = &d->spi->dev;
	u8 who;
	int ret;

	/* 1. 软复位 + 上电（正点原子序列） */
	ret = icm20608_write_reg(d, ICM20_PWR_MGMT_1, ICM20_PWR_RESET);
	if (ret)
		return ret;
	msleep(50);

	ret = icm20608_write_reg(d, ICM20_PWR_MGMT_1, ICM20_PWR_CLK_PLL);
	if (ret)
		return ret;
	msleep(50);

	/* 2. 校验芯片（通信失败/未焊接 → probe 失败） */
	ret = icm20608_read_reg(d, ICM20_WHO_AM_I, &who);
	if (ret)
		return ret;
	if (who != ICM20608_ID_G && who != ICM20608_ID_D) {
		dev_err(dev, "WHO_AM_I mismatch: %#x (expect 0xAF/0xAE)\n", who);
		return -ENODEV;
	}
	dev_info(dev, "ICM20608 WHO_AM_I = %#x\n", who);

	/* 3. 量程与滤波（正点原子板级验证值） */
	ret = icm20608_write_reg(d, ICM20_SMPLRT_DIV, 0x00);	/* 采样率=内部 */
	if (ret)
		return ret;
	ret = icm20608_write_reg(d, ICM20_GYRO_CONFIG, ICM20_GYRO_2000DPS);
	if (ret)
		return ret;
	ret = icm20608_write_reg(d, ICM20_ACCEL_CONFIG, ICM20_ACCEL_16G);
	if (ret)
		return ret;
	ret = icm20608_write_reg(d, ICM20_CONFIG, ICM20_LPF_20HZ);
	if (ret)
		return ret;
	ret = icm20608_write_reg(d, ICM20_ACCEL_CONFIG2, ICM20_ALPF_21HZ);
	if (ret)
		return ret;
	ret = icm20608_write_reg(d, ICM20_PWR_MGMT_2, 0x00);	/* 全轴打开 */
	if (ret)
		return ret;
	ret = icm20608_write_reg(d, ICM20_LP_MODE_CFG, 0x00);	/* 关低功耗 */
	if (ret)
		return ret;
	ret = icm20608_write_reg(d, ICM20_FIFO_EN, 0x00);	/* 关 FIFO */
	if (ret)
		return ret;
	return 0;
}

/* 一次读 7 轴原始值: gyro3 + accel3 + temp（14 字节突发读，每个 16 位大端有符号）
 * 寄存器顺序(0x3B 起): accel_x,y,z / temp / gyro_x,y,z（正点原子 icm20608reg.h），
 * 输出重排为 uapi 协议顺序: gyro_x,y,z / accel_x,y,z / temp */
static int icm20608_read_raw(struct icm20608_dev *d, s32 raw[ICM20608_WORD_CNT])
{
	u8 buf[14];
	s16 v[ICM20608_WORD_CNT];
	int ret, i;

	ret = icm20608_read_regs(d, ICM20_ACCEL_XOUT_H, buf, sizeof(buf));
	if (ret)
		return ret;

	for (i = 0; i < ICM20608_WORD_CNT; i++)
		v[i] = (s16)((buf[i * 2] << 8) | buf[i * 2 + 1]);

	raw[0] = v[4];	/* gyro_x */
	raw[1] = v[5];	/* gyro_y */
	raw[2] = v[6];	/* gyro_z */
	raw[3] = v[0];	/* accel_x */
	raw[4] = v[1];	/* accel_y */
	raw[5] = v[2];	/* accel_z */
	raw[6] = v[3];	/* temp */
	return 0;
}

static int icm20608_open(struct inode *inode, struct file *fp)
{
	struct icm20608_dev *d = container_of(fp->private_data,
					       struct icm20608_dev, misc);

	fp->private_data = d;
	return 0;
}

/* read: 一次性返回 7×s32（gyro_x/y/z, accel_x/y/z, temp，小端） */
static ssize_t icm20608_read(struct file *fp, char __user *buf,
			     size_t cnt, loff_t *off)
{
	struct icm20608_dev *d = fp->private_data;
	s32 raw[ICM20608_WORD_CNT];
	int ret;

	if (cnt < ICM20608_READ_LEN)
		return -EINVAL;

	ret = icm20608_read_raw(d, raw);
	if (ret)
		return ret;

	if (copy_to_user(buf, raw, sizeof(raw)))
		return -EFAULT;
	return sizeof(raw);
}

static const struct file_operations icm20608_fops = {
	.owner  = THIS_MODULE,
	.open   = icm20608_open,
	.read   = icm20608_read,
	.llseek = no_llseek,
};

static int icm20608_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct icm20608_dev *d;
	int ret;

	spi->mode = SPI_MODE_0;		/* CPOL=0, CPHA=0（正点原子板级验证） */
	ret = spi_setup(spi);
	if (ret)
		return ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->spi = spi;
	spi_set_drvdata(spi, d);

	/* 芯片初始化/校验失败则不暴露设备节点 */
	ret = icm20608_init_chip(d);
	if (ret)
		return ret;

	d->misc.minor  = MISC_DYNAMIC_MINOR;
	d->misc.name   = "icm20608";
	d->misc.fops   = &icm20608_fops;
	d->misc.parent = dev;
	ret = misc_register(&d->misc);
	if (ret)
		return ret;

	dev_info(dev, "ICM20608 ready, node /dev/icm20608\n");
	return 0;
}

static int icm20608_remove(struct spi_device *spi)
{
	struct icm20608_dev *d = spi_get_drvdata(spi);

	misc_deregister(&d->misc);
	return 0;
}

static const struct spi_device_id icm20608_id[] = {
	{ "icm20608", 0 },
	{}
};

static const struct of_device_id icm20608_of_match[] = {
	{ .compatible = "alientek,icm20608" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, icm20608_of_match);

static struct spi_driver icm20608_driver = {
	.probe    = icm20608_probe,
	.remove   = icm20608_remove,
	.id_table = icm20608_id,
	.driver = {
		.name = "icm20608",
		.of_match_table = icm20608_of_match,
	},
};
module_spi_driver(icm20608_driver);
MODULE_LICENSE("GPL");