// SPDX-License-Identifier: GPL-2.0
/* 按键事件上报驱动（P2）：单通道 KEY0 → 事件上报
 * 三种读模式：阻塞 read / 非阻塞 read(O_NONBLOCK) / poll
 * 手册 5.11 骨架补全：补 open(private_data)、O_NONBLOCK 分支、
 * misc 注册、remove 回调（P1 教训：必须注销 misc）
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/ktime.h>

#include "uapi/key_event_uapi.h"

struct key_dev {
    struct gpio_desc *gpio;
    int irq;
    /* 事件存储：P2 用简单的单事件槽 + 计数（P3 升级为 kfifo） */
    struct key_event last_event;
    atomic_t event_ready;      /* 1=有未读事件 */
    spinlock_t lock;
    wait_queue_head_t wq;
    struct miscdevice misc;
};

static struct key_dev g_key;

static irqreturn_t key_irq_handler(int irq, void *data)
{
    struct key_dev *k = data;
    int level = gpiod_get_value(k->gpio);
    unsigned long flags;

    /* 硬中断直接入队（操作简单），用 spin_lock_irqsave 保护 */
    spin_lock_irqsave(&k->lock, flags);
    k->last_event.timestamp_ns = ktime_get_ns();
    k->last_event.key = 1;                    /* KEY0 */
    k->last_event.pressed = (level == 0);     /* 按下为低电平 */
    atomic_set(&k->event_ready, 1);
    spin_unlock_irqrestore(&k->lock, flags);

    wake_up_interruptible(&k->wq);
    return IRQ_HANDLED;
}

static ssize_t key_read(struct file *file, char __user *buf, size_t count, loff_t *off)
{
    struct key_dev *k = file->private_data;
    struct key_event ev;
    unsigned long flags;

    if (count < sizeof(struct key_event))
        return -EINVAL;

    /* 阻塞等待事件；O_NONBLOCK 则立即返回 -EAGAIN */
    if (file->f_flags & O_NONBLOCK) {
        if (!atomic_read(&k->event_ready))
            return -EAGAIN;
    } else {
        if (wait_event_interruptible(k->wq, atomic_read(&k->event_ready)))
            return -ERESTARTSYS;
    }

    spin_lock_irqsave(&k->lock, flags);
    memcpy(&ev, &k->last_event, sizeof(ev));
    atomic_set(&k->event_ready, 0);
    spin_unlock_irqrestore(&k->lock, flags);

    if (copy_to_user(buf, &ev, sizeof(ev)))
        return -EFAULT;
    return sizeof(ev);
}

static unsigned int key_poll(struct file *file, poll_table *wait)
{
    struct key_dev *k = file->private_data;

    poll_wait(file, &k->wq, wait);
    return atomic_read(&k->event_ready) ? POLLIN : 0;
}

static long key_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct key_dev *k = file->private_data;

    switch (cmd) {
    case KEY_EVENT_CLEAR:
        atomic_set(&k->event_ready, 0);
        return 0;
    default:
        return -ENOTTY;
    }
}

static int key_open(struct inode *inode, struct file *file)
{
    /* 显式挂接私有数据（misc_open 默认放 miscdevice 指针，这里直接挂设备） */
    file->private_data = &g_key;
    return 0;
}

static const struct file_operations key_fops = {
    .owner          = THIS_MODULE,
    .open           = key_open,
    .read           = key_read,
    .poll           = key_poll,
    .unlocked_ioctl = key_ioctl,
};

static int key_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    int ret;

    /* gpiod 获取：设备树属性 "key-gpios"，GPIOD_IN 输入 */
    g_key.gpio = devm_gpiod_get(dev, "key", GPIOD_IN);
    if (IS_ERR(g_key.gpio))
        return PTR_ERR(g_key.gpio);

    g_key.irq = gpiod_to_irq(g_key.gpio);
    if (g_key.irq < 0)
        return g_key.irq;

    /* 双边沿触发：按下/松开都上报事件 */
    ret = devm_request_irq(dev, g_key.irq, key_irq_handler,
                           IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                           "key-event", &g_key);
    if (ret)
        return ret;

    atomic_set(&g_key.event_ready, 0);
    spin_lock_init(&g_key.lock);
    init_waitqueue_head(&g_key.wq);

    g_key.misc.minor = MISC_DYNAMIC_MINOR;
    g_key.misc.name  = "key-event";
    g_key.misc.fops  = &key_fops;
    g_key.misc.parent = dev;
    ret = misc_register(&g_key.misc);
    if (ret)
        return ret;
    return 0;
}

/* 卸载回调：注销 misc 设备，避免 misc_list 残留指向已卸载模块内存 */
static int key_remove(struct platform_device *pdev)
{
    misc_deregister(&g_key.misc);
    return 0;
}

static const struct of_device_id key_of_match[] = {
    { .compatible = "alientek,key-event" },
    {}
};
MODULE_DEVICE_TABLE(of, key_of_match);

static struct platform_driver key_driver = {
    .probe  = key_probe,
    .remove = key_remove,
    .driver = {
        .name = "alientek-key-event",
        .of_match_table = key_of_match,
    },
};
module_platform_driver(key_driver);
MODULE_LICENSE("GPL");