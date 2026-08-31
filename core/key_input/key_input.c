// SPDX-License-Identifier: GPL-2.0
/* 按键 input 子系统驱动（P6，手册 5.15.1）
 * KEY0（GPIO1_IO18）→ 标准 Linux input 事件流（/dev/input/eventX）
 * 与 P2 key_event（自研字符设备）对比：
 *   - 自研：自定义 UAPI + misc 节点，应用需自建工具链
 *   - input：标准 input API，evtest/Qt/evdev 直接消费（生态即用）
 * 对比文档：docs/driver-architecture-decision.md
 *
 * 设计要点：
 *  1. 全 devm 资源管理（gpio/irq/input），驱动 detach 自动释放，
 *     无需 remove 回调（P1 的 misc 教训对 devm 接口不适用）
 *  2. gpiod_get_value 返回【逻辑电平】：GPIO_ACTIVE_LOW 下物理低=逻辑1=按下
 *     （手册骨架 val?0:1 是裸电平视角，gpiod API 已做 active-low 翻转，
 *     直接以逻辑值上报即可，避免按下/释放颠倒）
 *  3. 按键码由设备树 linux,code 指定（默认 KEY_ARMED=227）
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/of.h>

/* 4.1.15 内核无 KEY_ARMED 宏（新内核 input-event-codes.h 才有），
 * 此处兜底定义，保持代码可读性（设备树 linux,code=<227> 才是权威来源） */
#ifndef KEY_ARMED
#define KEY_ARMED	227
#endif

struct key_input_dev {
	struct gpio_desc *gpio;
	int irq;
	struct input_dev *input;
	int key_code;		/* 设备树指定：KEY_ARMED=227 等 */
};

static irqreturn_t key_input_handler(int irq, void *data)
{
	struct key_input_dev *k = data;
	/* 逻辑电平：GPIO_ACTIVE_LOW 下按下=1（gpiod 已翻转，勿再取反） */
	int pressed = gpiod_get_value(k->gpio);

	input_report_key(k->input, k->key_code, pressed);
	/* 4.1.15 已统一为 input_sync()（input_sync_key 为旧 API，已移除） */
	input_sync(k->input);
	return IRQ_HANDLED;
}

static int key_input_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct key_input_dev *k;
	int ret;

	k = devm_kzalloc(dev, sizeof(*k), GFP_KERNEL);
	if (!k)
		return -ENOMEM;
	platform_set_drvdata(pdev, k);

	/* 按键码：设备树 linux,code，缺省 KEY_ARMED(227) */
	ret = of_property_read_u32(dev->of_node, "linux,code", &k->key_code);
	if (ret) {
		dev_info(dev, "linux,code 未指定，默认 KEY_ARMED(227)\n");
		k->key_code = KEY_ARMED;
	}

	/* gpiod：设备树属性 key-gpios，GPIOD_IN 输入模式 */
	k->gpio = devm_gpiod_get(dev, "key", GPIOD_IN);
	if (IS_ERR(k->gpio))
		return PTR_ERR(k->gpio);

	k->irq = gpiod_to_irq(k->gpio);
	if (k->irq < 0)
		return k->irq;

	/* 分配 input 设备（devm：driver detach 自动 unregister+free） */
	k->input = devm_input_allocate_device(dev);
	if (!k->input)
		return -ENOMEM;

	k->input->name = "imx6ull-gpio-keys";
	k->input->phys = "gpio-keys/input0";
	k->input->id.bustype = BUS_HOST;
	k->input->id.vendor  = 0x0001;
	k->input->id.product = 0x0001;
	k->input->id.version = 0x0100;

	/* 声明能力：EV_KEY + 具体按键码 */
	__set_bit(EV_KEY, k->input->evbit);
	input_set_capability(k->input, EV_KEY, k->key_code);

	/* 双边沿中断：按下/释放都上报 */
	ret = devm_request_irq(dev, k->irq, key_input_handler,
			       IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			       "key-input", k);
	if (ret)
		return ret;

	/* 注册 input 设备，出现在 /dev/input/eventX */
	ret = input_register_device(k->input);
	if (ret)
		return ret;

	dev_info(dev, "key-input 注册成功：code=%d(%s) irq=%d\n",
		 k->key_code, "KEY_ARMED", k->irq);
	return 0;
}

static const struct of_device_id key_input_of_match[] = {
	{ .compatible = "alientek,key-input" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, key_input_of_match);

static struct platform_driver key_input_driver = {
	.probe	= key_input_probe,
	.driver	= {
		.name		= "alientek-key-input",
		.of_match_table	= key_input_of_match,
	},
};
module_platform_driver(key_input_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("SouthBay");
MODULE_DESCRIPTION("KEY0 input subsystem driver (P6, manual 5.15.1)");