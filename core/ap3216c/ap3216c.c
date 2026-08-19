// SPDX-License-Identifier: GPL-2.0
/* ap3216c：AP3216C 环境光/接近传感器驱动（P4-B，手册 5.13.2）
 * 数据流: i2c_driver probe → miscdevice /dev/ap3216c0
 *         read() → 逐字节读 0x0A~0x0F → 解出 ir/als/ps（小端 u16）
 * 要点: probe 时一次初始化（软复位→上电使能），read 无锁（i2c 传输原子）；
 *       溢出标志位(IR.bit7 / PS.bit6)置位时对应值归 0；
 *       PS 寄存器为 0x0E/0x0F（手册骨架 0x10 系笔误，见 uapi/ap3216c.h）。
 * 参考: 正点原子 21_iic/ap3216c.c（逐字节读 + 数据解包，板级验证；
 *       曾用连续读 6 字节，板端实测数据帧交替失效，故改逐字节读），
 *       本项目按 P3/PWM 惯例改为 miscdevice + devm 多设备支持。
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/fs.h>

#include "uapi/ap3216c.h"

/* AP3216C 寄存器（正点原子 ap3216creg.h + datasheet） */
#define AP3216C_SYS_CONF	0x00	/* 系统配置 */
#define AP3216C_IR_DATA		0x0A	/* IR 数据低字节（0x0B 为高） */
#define AP3216C_ALS_DATA	0x0C	/* ALS 数据低字节（0x0D 为高） */
#define AP3216C_PS_DATA		0x0E	/* PS 数据低字节（0x0F 为高） */

#define AP3216C_CONF_RESET	0x04	/* 软复位 */
#define AP3216C_CONF_ENABLE	0x03	/* 开启 ALS+PS+IR */

struct ap3216c_dev {
	struct i2c_client *client;
	struct miscdevice misc;
};

/* 读 1 字节寄存器（i2c_transfer 双消息）。
 * 注意: 曾用一次连续读 0x0A~0x0F 6 字节，板端实测出现数据帧交替失效
 *       (IR_OF/PS_OF 置位 → 全 0 帧)，改为与正点原子板级验证一致的逐字节读。 */
static int ap3216c_read_reg(struct ap3216c_dev *d, u8 reg, u8 *val)
{
	struct i2c_msg msgs[2];
	int ret;

	msgs[0].addr = d->client->addr;
	msgs[0].flags = 0;		/* 写: 发送寄存器地址 */
	msgs[0].buf = &reg;
	msgs[0].len = 1;

	msgs[1].addr = d->client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].buf = val;
	msgs[1].len = 1;

	ret = i2c_transfer(d->client->adapter, msgs, 2);
	if (ret != 2)
		return -EREMOTEIO;
	return 0;
}

/* 写 1 字节寄存器（i2c_transfer 单消息；buf 为 __u8* 与 msg.buf 类型一致，
 * 避免 i2c_master_send 的 const char* 触 -Wpointer-sign） */
static int ap3216c_write_reg(struct ap3216c_dev *d, u8 reg, u8 val)
{
	struct i2c_msg msg;
	u8 w[2] = { reg, val };
	int ret;

	msg.addr = d->client->addr;
	msg.flags = 0;		/* 写 */
	msg.buf = w;
	msg.len = sizeof(w);

	ret = i2c_transfer(d->client->adapter, &msg, 1);
	if (ret != 1)
		return ret < 0 ? ret : -EREMOTEIO;
	return 0;
}

/* 软复位 + 上电使能 ALS/PS/IR（probe 时一次；open 不重复复位避免中断数据流） */
static int ap3216c_init_chip(struct ap3216c_dev *d)
{
	int ret;

	ret = ap3216c_write_reg(d, AP3216C_SYS_CONF, AP3216C_CONF_RESET);
	if (ret)
		return ret;
	msleep(50);		/* 复位至少 10ms，正点原子例程取 50ms */

	ret = ap3216c_write_reg(d, AP3216C_SYS_CONF, AP3216C_CONF_ENABLE);
	if (ret)
		return ret;
	return 0;
}

/* read: 一次性返回 ir/als/ps（3×u16 小端） */
static ssize_t ap3216c_read(struct file *fp, char __user *buf,
			    size_t cnt, loff_t *off)
{
	struct ap3216c_dev *d = fp->private_data;
	u8 raw[AP3216C_READ_LEN];
	u16 data[3];
	int ret, i;

	if (cnt < AP3216C_READ_LEN)
		return -EINVAL;

	/* 逐字节读 0x0A~0x0F（与正点原子一致；连续读实测会致数据帧交替失效） */
	for (i = 0; i < AP3216C_READ_LEN; i++) {
		ret = ap3216c_read_reg(d, AP3216C_IR_DATA + i, &raw[i]);
		if (ret)
			return ret;
	}

	/* IR: 0x0A.bit7=溢出标志; 有效 10 位 = 高字节<<2 | 低字节低 2 位 */
	if (raw[0] & 0x80)
		data[0] = 0;
	else
		data[0] = (u16)(raw[1] << 2) | (raw[0] & 0x03);

	/* ALS: 0x0C/0x0D 直接拼 16 位（bit15:12 通常为 0） */
	data[1] = (u16)(raw[3] << 8) | raw[2];

	/* PS: 0x0E.bit6=溢出标志; 有效 10 位 = (高字节&0x3F)<<4 | 低字节低 4 位 */
	if (raw[4] & 0x40)
		data[2] = 0;
	else
		data[2] = (u16)((raw[5] & 0x3F) << 4) | (raw[4] & 0x0F);

	if (copy_to_user(buf, data, sizeof(data)))
		return -EFAULT;
	return sizeof(data);
}

static int ap3216c_open(struct inode *inode, struct file *fp)
{
	struct ap3216c_dev *d = container_of(fp->private_data,
					     struct ap3216c_dev, misc);

	fp->private_data = d;
	return 0;
}

static const struct file_operations ap3216c_fops = {
	.owner  = THIS_MODULE,
	.open   = ap3216c_open,
	.read   = ap3216c_read,
	.llseek = no_llseek,
};

static int ap3216c_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ap3216c_dev *d;
	int ret;

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->client = client;
	i2c_set_clientdata(client, d);

	ret = ap3216c_init_chip(d);
	if (ret) {
		dev_err(dev, "chip init failed: %d\n", ret);
		return ret;
	}

	/* misc 注册（初始化成功后再暴露设备节点） */
	d->misc.minor  = MISC_DYNAMIC_MINOR;
	d->misc.name   = "ap3216c0";
	d->misc.fops   = &ap3216c_fops;
	d->misc.parent = dev;
	ret = misc_register(&d->misc);
	if (ret)
		return ret;

	dev_info(dev, "AP3216C ready (als/ps/ir), node /dev/ap3216c0\n");
	return 0;
}

static int ap3216c_remove(struct i2c_client *client)
{
	struct ap3216c_dev *d = i2c_get_clientdata(client);

	misc_deregister(&d->misc);
	return 0;
}

static const struct i2c_device_id ap3216c_id[] = {
	{ "ap3216c", 0 },
	{}
};

static const struct of_device_id ap3216c_of_match[] = {
	{ .compatible = "alientek,ap3216c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

static struct i2c_driver ap3216c_driver = {
	.probe    = ap3216c_probe,
	.remove   = ap3216c_remove,
	.id_table = ap3216c_id,
	.driver = {
		.name = "ap3216c",
		.of_match_table = ap3216c_of_match,
	},
};
module_i2c_driver(ap3216c_driver);
MODULE_LICENSE("GPL");
