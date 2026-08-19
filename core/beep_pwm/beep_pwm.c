// SPDX-License-Identifier: GPL-2.0
/* beep_pwm：PWM 蜂鸣器驱动（P4-B，手册 5.13.1）
 * 数据流: 用户态 ioctl → freq/duty 换算 → pwm_config + pwm_enable/disable → PWM3 引脚
 * 要点: 4 个 ioctl（SET_FREQ/SET_DUTY/ON/OFF）、互斥锁保护配置、
 *       duty=0 即静音（enabled=false）、频率上限 20kHz
 * 配置/开关分离设计: SET_FREQ/SET_DUTY 仅 pwm_config（不使能），
 *       ON 才 pwm_enable——首次使用须先发 BEEP_IOC_ON（测试程序 --on），
 *       仅设 freq/duty 不会出声（P4-B 板测曾因此误判"不响"）
 * API 适配: 手册骨架用 pwm_get_state/pwm_apply_state（4.3+ 引入），
 *          板端内核 4.1.15 无此 API，适配为 pwm_config + pwm_enable/pwm_disable
 *          （正点原子 4.1.15 标准用法）。
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/pwm.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/time.h>		/* NSEC_PER_SEC */

#include "uapi/beep_pwm.h"

#define BEEP_PWM_DEFAULT_FREQ	1000	/* 默认 1kHz */

struct beep_pwm_dev {
	struct device       *dev;
	struct pwm_device   *pwm;
	struct mutex         mutex;	/* 配置 ioctl 用 */
	int                  freq;	/* Hz（1~BEEP_PWM_MAX_FREQ） */
	int                  duty;	/* 占空比 0-100，0=静音 */
	bool                 enabled;
	struct miscdevice    misc;
};

/* 换算 + 应用（调用方须已持 b->mutex）
 * pwm_config 只改 period/duty，enable 状态独立管理 */
static int beep_pwm_apply(struct beep_pwm_dev *b)
{
	u32 period_ns;
	u32 duty_ns;
	int ret;

	period_ns = DIV_ROUND_UP(NSEC_PER_SEC, (u32)b->freq);
	/* 先除后乘避免 u64 除法（ARM 无 __aeabi_uldivmod，模块内不可用）；
	 * period/100 整除误差 <100ns，对周期 >=50us 影响 <0.2%，可忽略 */
	duty_ns = (period_ns / 100) * (u32)b->duty;

	ret = pwm_config(b->pwm, duty_ns, period_ns);
	if (ret)
		return ret;
	if (b->enabled && b->duty > 0)
		return pwm_enable(b->pwm);
	pwm_disable(b->pwm);
	return 0;
}

static long beep_pwm_ioctl(struct file *fp, unsigned int cmd,
			   unsigned long arg)
{
	struct beep_pwm_dev *b = fp->private_data;
	void __user *uarg = (void __user *)arg;
	int val;
	int ret = 0;

	mutex_lock(&b->mutex);
	switch (cmd) {
	case BEEP_IOC_SET_FREQ:
		if (copy_from_user(&val, uarg, sizeof(val))) {
			ret = -EFAULT;
			break;
		}
		if (val < 1 || val > BEEP_PWM_MAX_FREQ) {
			ret = -EINVAL;
			break;
		}
		b->freq = val;
		ret = beep_pwm_apply(b);
		break;
	case BEEP_IOC_SET_DUTY:
		if (copy_from_user(&val, uarg, sizeof(val))) {
			ret = -EFAULT;
			break;
		}
		if (val < 0 || val > 100) {
			ret = -EINVAL;
			break;
		}
		b->duty = val;
		ret = beep_pwm_apply(b);
		break;
	case BEEP_IOC_ON:
		b->enabled = true;
		ret = beep_pwm_apply(b);
		break;
	case BEEP_IOC_OFF:
		b->enabled = false;
		pwm_disable(b->pwm);
		break;
	default:
		ret = -ENOTTY;
		break;
	}
	mutex_unlock(&b->mutex);
	return ret;
}

static int beep_pwm_open(struct inode *inode, struct file *fp)
{
	struct beep_pwm_dev *b = container_of(fp->private_data,
					      struct beep_pwm_dev, misc);

	fp->private_data = b;
	return 0;
}

static int beep_pwm_release(struct inode *inode, struct file *fp)
{
	fp->private_data = NULL;
	return 0;
}

static const struct file_operations beep_pwm_fops = {
	.owner          = THIS_MODULE,
	.open           = beep_pwm_open,
	.release        = beep_pwm_release,
	.unlocked_ioctl = beep_pwm_ioctl,
	.llseek         = no_llseek,
};

static int beep_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct beep_pwm_dev *b;
	int ret;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	mutex_init(&b->mutex);

	/* 从设备树 pwms 属性获取 PWM（单通道，无 name） */
	b->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(b->pwm))
		return PTR_ERR(b->pwm);

	/* 初始：1kHz、占空比 0（静音） */
	b->freq = BEEP_PWM_DEFAULT_FREQ;
	b->duty = 0;
	b->enabled = false;
	pwm_config(b->pwm, 0, NSEC_PER_SEC / BEEP_PWM_DEFAULT_FREQ);

	/* misc 注册（PWM 就绪后再暴露设备节点） */
	b->misc.minor  = MISC_DYNAMIC_MINOR;
	b->misc.name   = "beep_pwm0";
	b->misc.fops   = &beep_pwm_fops;
	b->misc.parent = dev;
	ret = misc_register(&b->misc);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, b);
	return 0;
}

/* remove：先摘设备节点 → 停 PWM（静音） */
static int beep_pwm_remove(struct platform_device *pdev)
{
	struct beep_pwm_dev *b = platform_get_drvdata(pdev);

	misc_deregister(&b->misc);
	pwm_disable(b->pwm);
	return 0;
}

static const struct of_device_id beep_pwm_of_match[] = {
	{ .compatible = "alientek,beep-pwm" },
	{}
};
MODULE_DEVICE_TABLE(of, beep_pwm_of_match);

static struct platform_driver beep_pwm_driver = {
	.probe  = beep_pwm_probe,
	.remove = beep_pwm_remove,
	.driver = {
		.name = "alientek-beep-pwm",
		.of_match_table = beep_pwm_of_match,
	},
};
module_platform_driver(beep_pwm_driver);
MODULE_LICENSE("GPL");