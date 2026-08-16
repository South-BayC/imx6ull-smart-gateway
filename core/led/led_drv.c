// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/ioctl.h>
#include <linux/fs.h>

#define LED_IOC_MAGIC 'L'
#define LED_IOC_ON    _IO(LED_IOC_MAGIC, 0x01)
#define LED_IOC_OFF   _IO(LED_IOC_MAGIC, 0x02)
#define LED_IOC_GET   _IOR(LED_IOC_MAGIC, 0x03, int)

struct led_dev {
    struct gpio_desc *gpio;
    int state;
    struct miscdevice misc;
};

static struct led_dev g_led;

static long led_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
    case LED_IOC_ON:
        gpiod_set_value(g_led.gpio, 1);
        g_led.state = 1;
        return 0;
    case LED_IOC_OFF:
        gpiod_set_value(g_led.gpio, 0);
        g_led.state = 0;
        return 0;
    case LED_IOC_GET:
        return copy_to_user((void __user *)arg, &g_led.state, sizeof(int)) ? -EFAULT : 0;
    default:
        return -ENOTTY;
    }
}

static const struct file_operations led_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = led_ioctl,
};

static int led_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    int ret;

    /* gpiod 获取：设备树属性 "led-gpios"，GPIO_ACTIVE_LOW 由设备树表达 */
    g_led.gpio = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
    if (IS_ERR(g_led.gpio))
        return PTR_ERR(g_led.gpio);

    g_led.misc.minor = MISC_DYNAMIC_MINOR;
    g_led.misc.name  = "led";
    g_led.misc.fops  = &led_fops;
    g_led.misc.parent = dev;
    ret = misc_register(&g_led.misc);
    if (ret)
        return ret;
    return 0;
}

/* 卸载回调：注销 misc 设备，避免 misc_list 残留指向已卸载模块内存 */
static int led_remove(struct platform_device *pdev)
{
    misc_deregister(&g_led.misc);
    return 0;
}

static const struct of_device_id led_of_match[] = {
    { .compatible = "alientek,led" },
    {}
};
MODULE_DEVICE_TABLE(of, led_of_match);

static struct platform_driver led_driver = {
    .probe  = led_probe,
    .remove = led_remove,
    .driver = {
        .name = "alientek-led",
        .of_match_table = led_of_match,
    },
};
module_platform_driver(led_driver);
MODULE_LICENSE("GPL");
