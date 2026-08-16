// SPDX-License-Identifier: GPL-2.0
/* gpio_event 多通道事件采集驱动（P3，企业级）
 * 手册 5.12 完整实现（对标 5.4 设计）：
 *   数据流: GPIO 边沿 → IRQ handler → 时间戳+事件封装 → spin_lock → kfifo 入队
 *           → (空→非空) wake_up → read(): copy_to_user → 消费
 *   要点: 序列号守恒（queued + dropped == irq + snapshot，可验证）、
 *         事务式 read（peek → copy_to_user → 消费）、START 初始快照、
 *         独占打开、7 个 ioctl、模块内 tracepoint（ftrace 观测）
 * 实现顺序：先单通道（KEY0 = GPIO1_IO18）跑通，再扩多通道
 */
#define CREATE_TRACE_POINTS
#include "trace/edt_capture.h"

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/log2.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "uapi/edt_capture.h"

/* 内部数据结构（与 UAPI 分离） */
struct edt_capture_dev {
	struct device          *dev;
	struct gpio_descs      *gpios;        /* 通道 GPIO 数组（gpiod_get_array 返回） */
	int                     num_chans;    /* 实际使能通道数 */
	unsigned long           channel_mask; /* 设备树声明通道位图（SET_CONFIG 可收窄） */
	u32                     edge_mask;    /* 运行时边沿过滤（SET_CONFIG 可改） */
	struct kfifo            fifo;         /* 事件 FIFO（2 的幂） */
	spinlock_t              lock;         /* IRQ + 入队 + 读共用 */
	struct mutex            mutex;        /* 配置 ioctl 用 */
	wait_queue_head_t       wq;
	atomic_t                running;
	atomic_t                opened;       /* 独占打开 */
	atomic_t                removing;
	int                     irqs[EDT_CAPTURE_MAX_CHANNELS];
	/* 统计（全部在 spinlock 保护下更新，GET_STATS 加锁拷贝） */
	struct edt_capture_stats stats;
	struct miscdevice       misc;
	unsigned int            fifo_depth;
};

/* 由 irq 号反查通道号（单通道线性遍历可接受） */
static int edt_capture_irq_to_channel(struct edt_capture_dev *cap, int irq)
{
	int i;

	for (i = 0; i < cap->num_chans; i++)
		if (cap->irqs[i] == irq)
			return i;
	return -1;
}

/* 内部入队函数（可测性设计：KUnit/自测可直接调用，不依赖真实 IRQ）
 * 调用方须已持有 cap->lock。
 * 返回 0=已入队，1=丢弃（FIFO 满）
 * 设计取舍：kfifo 满时仍消耗序列号——保证 queued + dropped == irq 守恒
 */
static int edt_capture_enqueue_event(struct edt_capture_dev *cap,
				     struct edt_capture_event *ev)
{
	if (kfifo_avail(&cap->fifo) >= sizeof(*ev)) {
		kfifo_in(&cap->fifo, ev, sizeof(*ev));
		cap->stats.queued_events++;
		if (cap->stats.fifo_level_events < kfifo_len(&cap->fifo)) {
			cap->stats.fifo_level_events = kfifo_len(&cap->fifo);
			if (cap->stats.fifo_high_watermark <
			    cap->stats.fifo_level_events)
				cap->stats.fifo_high_watermark =
					cap->stats.fifo_level_events;
		}
		return 0;
	}
	cap->stats.dropped_events++;
	cap->stats.fifo_overflow_events++;
	trace_edt_capture_drop(ev->sequence, ev->channel, kfifo_len(&cap->fifo));
	return 1;
}

/* IRQ 处理 + 入队（正确性关键）：硬中断上下文，仅用 spin_lock_irqsave */
static irqreturn_t edt_capture_irq_handler(int irq, void *data)
{
	struct edt_capture_dev *cap = data;
	struct edt_capture_event ev;
	unsigned long flags;
	bool wake = false;
	int idx = edt_capture_irq_to_channel(cap, irq);
	int level;

	if (idx < 0)
		return IRQ_NONE;

	spin_lock_irqsave(&cap->lock, flags);
	if (atomic_read(&cap->running) && (cap->channel_mask & BIT(idx))) {
		ev.timestamp_ns = ktime_get_ns();
		ev.sequence = cap->stats.last_sequence + 1;
		cap->stats.last_sequence = ev.sequence;
		ev.channel = idx;

		/* 硬中断安全采样（probe 已校验 gpiod_cansleep == false） */
		level = gpiod_get_value(cap->gpios->desc[idx]);
		if (level < 0) {
			/* 采样错误：计入 irq 与 dropped（守恒），不入队 */
			cap->stats.irq_events++;
			cap->stats.dropped_events++;
			cap->stats.sample_error_events++;
			trace_edt_capture_drop(ev.sequence, idx,
					       kfifo_len(&cap->fifo));
			goto out;
		}
		ev.level = level;
		/* 边沿由边沿后电平推断：1 → 上升沿，0 → 下降沿 */
		ev.edge = level ? EDT_CAPTURE_EDGE_RISING : EDT_CAPTURE_EDGE_FALLING;
		/* edge_mask 过滤：不关心的边沿直接忽略（不计统计，不影响守恒） */
		if (!(cap->edge_mask & ev.edge))
			goto out;

		cap->stats.irq_events++;
		{
			bool was_empty = kfifo_is_empty(&cap->fifo);

			if (edt_capture_enqueue_event(cap, &ev) == 0 && was_empty) {
				cap->stats.wake_signals++;
				wake = true;   /* 仅空→非空唤醒 */
			}
		}
		trace_edt_capture_irq(idx, ev.level, ev.timestamp_ns);
		trace_edt_capture_enqueue(ev.sequence, idx,
					  kfifo_len(&cap->fifo));
	}
out:
	spin_unlock_irqrestore(&cap->lock, flags);

	if (wake)
		wake_up_interruptible_poll(&cap->wq, POLLIN);
	return IRQ_HANDLED;
}

/* read：staging 事务式消费（peek → copy_to_user 成功 → 消费） */
static ssize_t edt_capture_read(struct file *fp, char __user *buf,
				size_t count, loff_t *off)
{
	struct edt_capture_dev *cap = fp->private_data;
	struct edt_capture_event ev;
	unsigned long flags;
	ssize_t ret;

	if (count % sizeof(struct edt_capture_event))
		return -EINVAL;

	/* 非阻塞：无数据直接 EAGAIN（无论运行状态） */
	if (fp->f_flags & O_NONBLOCK) {
		if (kfifo_is_empty(&cap->fifo))
			return -EAGAIN;
	} else {
		/* 运行停止且 FIFO 空：返回 0（EOF 语义，不挂起） */
		if (!atomic_read(&cap->running) && kfifo_is_empty(&cap->fifo))
			return 0;
		ret = wait_event_interruptible(cap->wq,
				!kfifo_is_empty(&cap->fifo) ||
				!atomic_read(&cap->running));
		if (ret)
			return -ERESTARTSYS;
		if (kfifo_is_empty(&cap->fifo))
			return 0;   /* STOP 唤醒 */
	}

	/* 事务式：先 peek 到内核 staging，copy_to_user 成功后才消费 */
	spin_lock_irqsave(&cap->lock, flags);
	if (!kfifo_out_peek(&cap->fifo, &ev, sizeof(ev))) {
		/* 可能被并发消费（理论上独占打开 + 单读者，防御性处理） */
		spin_unlock_irqrestore(&cap->lock, flags);
		return -EAGAIN;
	}
	spin_unlock_irqrestore(&cap->lock, flags);

	if (copy_to_user(buf, &ev, sizeof(ev)))
		return -EFAULT;

	spin_lock_irqsave(&cap->lock, flags);
	/* peek 后未被并发消费（独占打开 + 单读者），out 理论必成功 */
	if (kfifo_out(&cap->fifo, &ev, sizeof(ev)) != sizeof(ev)) {
		spin_unlock_irqrestore(&cap->lock, flags);
		return -EIO;
	}
	cap->stats.read_events++;
	spin_unlock_irqrestore(&cap->lock, flags);
	trace_edt_capture_read(1, kfifo_len(&cap->fifo));

	return sizeof(ev);
}

static unsigned int edt_capture_poll(struct file *fp, poll_table *wait)
{
	struct edt_capture_dev *cap = fp->private_data;
	unsigned int mask = 0;

	poll_wait(fp, &cap->wq, wait);
	if (!kfifo_is_empty(&cap->fifo))
		mask |= POLLIN | POLLRDNORM;
	if (!atomic_read(&cap->running))
		mask |= POLLHUP;   /* STOP 后唤醒 poll 等待 */
	return mask;
}

/* START 初始快照（正确性关键）：冻结中断 → 读当前电平 → 恢复 */
static int edt_capture_start(struct edt_capture_dev *cap)
{
	unsigned long flags;
	int i;

	/* 防并发 START：CAS */
	if (atomic_cmpxchg(&cap->running, 0, 1) != 0)
		return -EBUSY;

	/* 冻结所有通道中断，读当前电平作为快照（防快照与边沿竞态） */
	for (i = 0; i < cap->num_chans; i++)
		disable_irq(cap->irqs[i]);
	spin_lock_irqsave(&cap->lock, flags);
	for (i = 0; i < cap->num_chans; i++) {
		struct edt_capture_event ev;

		if (!(cap->channel_mask & BIT(i)))
			continue;
		ev.timestamp_ns = ktime_get_ns();
		ev.sequence = ++cap->stats.last_sequence;
		ev.channel  = i;
		ev.level    = gpiod_get_value(cap->gpios->desc[i]);
		ev.edge     = 0;   /* 快照无边沿 */
		if (kfifo_avail(&cap->fifo) >= sizeof(ev)) {
			kfifo_in(&cap->fifo, &ev, sizeof(ev));
			cap->stats.queued_events++;
			cap->stats.snapshot_events++;
		}
	}
	spin_unlock_irqrestore(&cap->lock, flags);
	for (i = 0; i < cap->num_chans; i++)
		enable_irq(cap->irqs[i]);
	wake_up_interruptible_poll(&cap->wq, POLLIN);
	return 0;
}

/* STOP：running=0 → 同步 IRQ → 唤醒读等待（POLLHUP） */
static int edt_capture_stop(struct edt_capture_dev *cap)
{
	int i;

	if (atomic_cmpxchg(&cap->running, 1, 0) != 1)
		return 0;   /* 已停止，幂等 */

	for (i = 0; i < cap->num_chans; i++)
		synchronize_irq(cap->irqs[i]);
	wake_up_interruptible_poll(&cap->wq, POLLIN | POLLHUP);
	return 0;
}

/* SET_CONFIG：mutex 保护；仅允许未运行/清空后修改；校验 mask */
static int edt_capture_set_config(struct edt_capture_dev *cap,
				  struct edt_capture_config *cfg)
{
	unsigned long flags;
	int ret = 0;

	if (cfg->abi_version != EDT_CAPTURE_ABI_VERSION)
		return -EINVAL;
	if (cfg->struct_size != sizeof(*cfg))
		return -EINVAL;

	mutex_lock(&cap->mutex);
	/* 运行中禁止修改（须先 STOP） */
	if (atomic_read(&cap->running)) {
		ret = -EBUSY;
		goto out;
	}
	/* channel_mask 只能选择已配置通道的子集 */
	if (cfg->channel_mask & ~cap->channel_mask) {
		ret = -EINVAL;
		goto out;
	}
	if (cfg->channel_mask == 0) {
		ret = -EINVAL;
		goto out;
	}
	/* edge_mask 必须被支持 */
	if (!cfg->edge_mask || (cfg->edge_mask & ~EDT_CAPTURE_EDGE_BOTH)) {
		ret = -EINVAL;
		goto out;
	}
	/* fifo_depth 必须为 2 的幂（0 表示保持当前） */
	if (cfg->fifo_depth_events &&
	    !is_power_of_2(cfg->fifo_depth_events)) {
		ret = -EINVAL;
		goto out;
	}

	/* 修改仅允许在 FIFO 清空后进行 */
	spin_lock_irqsave(&cap->lock, flags);
	if (!kfifo_is_empty(&cap->fifo)) {
		spin_unlock_irqrestore(&cap->lock, flags);
		ret = -EBUSY;   /* 先 CLEAR_FIFO 再改配置 */
		goto out;
	}
	if (cfg->fifo_depth_events && cfg->fifo_depth_events != cap->fifo_depth) {
		struct kfifo new_fifo;
		int a;

		/* kfifo 重建：kfifo_alloc（GFP_KERNEL 可睡眠）必须放锁外 */
		spin_unlock_irqrestore(&cap->lock, flags);
		a = kfifo_alloc(&new_fifo,
				cfg->fifo_depth_events *
				sizeof(struct edt_capture_event),
				GFP_KERNEL);
		if (a) {
			ret = -ENOMEM;
			goto out;
		}
		/* 分配期间可能被并发 START 入队 → 重新校验后再交换 */
		spin_lock_irqsave(&cap->lock, flags);
		if (atomic_read(&cap->running) ||
		    !kfifo_is_empty(&cap->fifo)) {
			spin_unlock_irqrestore(&cap->lock, flags);
			kfifo_free(&new_fifo);
			ret = -EBUSY;
			goto out;
		}
		kfifo_free(&cap->fifo);
		cap->fifo = new_fifo;
		cap->fifo_depth = cfg->fifo_depth_events;
		cap->stats.fifo_capacity_events = cap->fifo_depth;
	}
	cap->channel_mask = cfg->channel_mask;
	cap->edge_mask = cfg->edge_mask;
	spin_unlock_irqrestore(&cap->lock, flags);
out:
	mutex_unlock(&cap->mutex);
	return ret;
}

/* ioctl 全集（7 个） */
static long edt_capture_ioctl(struct file *fp, unsigned int cmd,
			      unsigned long arg)
{
	struct edt_capture_dev *cap = fp->private_data;
	void __user *uarg = (void __user *)arg;
	unsigned long flags;

	switch (cmd) {
	case EDT_IOC_GET_CAPS: {
		struct edt_capture_caps caps;

		memset(&caps, 0, sizeof(caps));
		caps.abi_version = EDT_CAPTURE_ABI_VERSION;
		caps.struct_size = sizeof(caps);
		caps.event_size = sizeof(struct edt_capture_event);
		caps.max_channels = EDT_CAPTURE_MAX_CHANNELS;
		caps.configured_channels = cap->num_chans;
		caps.supported_edges = EDT_CAPTURE_EDGE_BOTH;
		caps.fifo_depth_events = cap->fifo_depth;
		caps.flags = EDT_CAPTURE_CAP_NONBLOCK |
			     EDT_CAPTURE_CAP_POLL |
			     EDT_CAPTURE_CAP_EXCLUSIVE_OPEN |
			     EDT_CAPTURE_CAP_MONOTONIC_TIMESTAMP |
			     EDT_CAPTURE_CAP_DROP_REASON_STATS |
			     EDT_CAPTURE_CAP_INITIAL_SNAPSHOT |
			     EDT_CAPTURE_CAP_RESET_STATS;
		if (copy_to_user(uarg, &caps, sizeof(caps)))
			return -EFAULT;
		return 0;
	}
	case EDT_IOC_START:
		return edt_capture_start(cap);
	case EDT_IOC_STOP:
		return edt_capture_stop(cap);
	case EDT_IOC_SET_CONFIG: {
		struct edt_capture_config cfg;

		if (copy_from_user(&cfg, uarg, sizeof(cfg)))
			return -EFAULT;
		return edt_capture_set_config(cap, &cfg);
	}
	case EDT_IOC_CLEAR_FIFO: {
		unsigned int len;

		spin_lock_irqsave(&cap->lock, flags);
		len = kfifo_len(&cap->fifo);
		kfifo_reset(&cap->fifo);
		cap->stats.cleared_events += len;
		spin_unlock_irqrestore(&cap->lock, flags);
		return 0;
	}
	case EDT_IOC_GET_STATS: {
		struct edt_capture_stats st;

		/* 加锁拷贝（spinlock 保护统计原子性） */
		spin_lock_irqsave(&cap->lock, flags);
		st = cap->stats;
		st.running = atomic_read(&cap->running);
		spin_unlock_irqrestore(&cap->lock, flags);
		if (copy_to_user(uarg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}
	case EDT_IOC_RESET_STATS: {
		spin_lock_irqsave(&cap->lock, flags);
		memset(&cap->stats, 0, sizeof(cap->stats));
		cap->stats.abi_version = EDT_CAPTURE_ABI_VERSION;
		cap->stats.struct_size = sizeof(cap->stats);
		cap->stats.fifo_capacity_events = cap->fifo_depth;
		spin_unlock_irqrestore(&cap->lock, flags);
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

/* 独占打开：重复 open 返回 -EBUSY */
static int edt_capture_open(struct inode *inode, struct file *fp)
{
	struct edt_capture_dev *cap = container_of(fp->private_data,
						   struct edt_capture_dev,
						   misc);

	if (atomic_cmpxchg(&cap->opened, 0, 1) != 0)
		return -EBUSY;
	fp->private_data = cap;
	return 0;
}

static int edt_capture_release(struct inode *inode, struct file *fp)
{
	struct edt_capture_dev *cap = fp->private_data;

	/* 进程退出/崩溃时自动 STOP：防止 running 残留 1 导致设备死锁
	 * （交互程序 Ctrl-C 退出后，新进程 open 成功但 START 必 EBUSY，
	 *   且无任何 ioctl 能重置 running——必须在此回收） */
	if (atomic_cmpxchg(&cap->running, 1, 0) == 1) {
		int i;

		for (i = 0; i < cap->num_chans; i++)
			synchronize_irq(cap->irqs[i]);
	}
	atomic_set(&cap->opened, 0);
	wake_up_all(&cap->wq);   /* 唤醒 remove 的等待（opened==0 条件） */
	return 0;
}

static const struct file_operations edt_capture_fops = {
	.owner          = THIS_MODULE,
	.open           = edt_capture_open,
	.release        = edt_capture_release,
	.read           = edt_capture_read,
	.poll           = edt_capture_poll,
	.unlocked_ioctl = edt_capture_ioctl,
	.llseek         = no_llseek,
};

static int edt_capture_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct edt_capture_dev *cap;
	struct gpio_descs *g;
	u32 fifo_depth = 0;
	int i, ret;

	g = devm_gpiod_get_array(dev, "capture", GPIOD_IN);
	if (IS_ERR(g))
		return PTR_ERR(g);
	if (g->ndescs > EDT_CAPTURE_MAX_CHANNELS)
		return -EINVAL;
	/* 每个 GPIO 校验：gpiod_cansleep? 是 → 不能在硬中断采样，拒绝 */
	for (i = 0; i < g->ndescs; i++)
		if (gpiod_cansleep(g->desc[i]))
			return -EINVAL;

	/* 设备树 fifo-depth-events，无则默认 1024 */
	device_property_read_u32(dev, "fifo-depth-events", &fifo_depth);
	fifo_depth = fifo_depth ? fifo_depth : 1024;
	if (!is_power_of_2(fifo_depth))
		return -EINVAL;

	cap = devm_kzalloc(dev, sizeof(*cap), GFP_KERNEL);
	if (!cap)
		return -ENOMEM;
	cap->dev = dev;
	cap->gpios = g;
	cap->num_chans = g->ndescs;
	cap->channel_mask = (1UL << g->ndescs) - 1;   /* 初始全部使能 */
	cap->edge_mask = EDT_CAPTURE_EDGE_BOTH;
	cap->fifo_depth = fifo_depth;
	spin_lock_init(&cap->lock);
	mutex_init(&cap->mutex);
	init_waitqueue_head(&cap->wq);
	atomic_set(&cap->running, 0);
	atomic_set(&cap->opened, 0);
	atomic_set(&cap->removing, 0);
	cap->stats.abi_version = EDT_CAPTURE_ABI_VERSION;
	cap->stats.struct_size = sizeof(cap->stats);
	cap->stats.fifo_capacity_events = fifo_depth;

	platform_set_drvdata(pdev, cap);

	/* IRQ 申请（双边沿，每通道；先于 misc 注册，失败路径干净） */
	for (i = 0; i < cap->num_chans; i++) {
		int irq = gpiod_to_irq(g->desc[i]);

		if (irq < 0)
			return irq;
		cap->irqs[i] = irq;
		ret = devm_request_irq(dev, irq, edt_capture_irq_handler,
				       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				       "edt-capture", cap);
		if (ret)
			return ret;
	}

	/* FIFO 分配：kfifo_alloc 自动分配缓冲。
	 * 注意：kfifo_init(fifo, NULL, size) 是静态初始化用法，传 NULL 会让
	 * fifo->data == NULL，首次 kfifo_in 即 memcpy 到 NULL 崩溃（板端已踩坑） */
	ret = kfifo_alloc(&cap->fifo,
			  fifo_depth * sizeof(struct edt_capture_event),
			  GFP_KERNEL);
	if (ret)
		return ret;

	/* misc 注册（中断就绪后再暴露设备节点） */
	cap->misc.minor = MISC_DYNAMIC_MINOR;
	cap->misc.name  = "edt_capture0";
	cap->misc.fops  = &edt_capture_fops;
	cap->misc.parent = dev;
	ret = misc_register(&cap->misc);
	if (ret) {
		kfifo_free(&cap->fifo);
		return ret;
	}
	return 0;
}

/* remove：先摘设备节点 → 停收 → 同步 IRQ → 等读者退出 → 释放 FIFO
 * 注：4.1.15 的 platform_driver.remove 须返回 int */
static int edt_capture_remove(struct platform_device *pdev)
{
	struct edt_capture_dev *cap = platform_get_drvdata(pdev);
	int i;

	atomic_set(&cap->removing, 1);
	misc_deregister(&cap->misc);       /* 先移除设备节点 */
	atomic_set(&cap->running, 0);
	wake_up_all(&cap->wq);             /* 唤醒所有读等待 */
	for (i = 0; i < cap->num_chans; i++)
		synchronize_irq(cap->irqs[i]);
	wait_event(cap->wq, atomic_read(&cap->opened) == 0);  /* 等读者退出 */
	kfifo_free(&cap->fifo);
	return 0;
}

static const struct of_device_id edt_capture_of_match[] = {
	{ .compatible = "alientek,gpio-event-capture" },
	{}
};
MODULE_DEVICE_TABLE(of, edt_capture_of_match);

static struct platform_driver edt_capture_driver = {
	.probe  = edt_capture_probe,
	.remove = edt_capture_remove,
	.driver = {
		.name = "alientek-gpio-event-capture",
		.of_match_table = edt_capture_of_match,
	},
};
module_platform_driver(edt_capture_driver);
MODULE_LICENSE("GPL");