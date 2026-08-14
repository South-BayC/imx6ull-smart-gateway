// SPDX-License-Identifier: GPL-2.0
/*
 * indaq_core.c - i.MX6U 工业数据采集核心驱动实现
 * 
 * MFD (Multi-Function Device) 风格的平台驱动,管理多个子设备模块
 * 
 * 架构设计:
 * ┌─────────────────────────────────────────────┐
 * │          用户空间应用程序                    │
 * └──────────────┬──────────────────────────────┘
 *                │ read() / ioctl()
 * ┌──────────────▼──────────────────────────────┐
 * │     字符设备层 (cdev + file_operations)      │
 * │  - indaq_read():  阻塞读取环形缓冲区         │
 * │  - indaq_ioctl(): 控制采集启停、采样率       │
 * └──────────────┬──────────────────────────────┘
 *                │
 * ┌──────────────▼──────────────────────────────┐
 * │       核心数据管理层                         │
 * │  - 环形缓冲区 (lock-free ringbuf)           │
 * │  - 样本融合 (latest_sample)                 │
 * │  - 等待队列 (read_wq)                       │
 * └──┬──────────────┬──────────────┬───────────┘
 *    │              │              │
 * ┌──▼───┐   ┌─────▼────┐  ┌────▼────────┐
 * │I2C   │   │  SPI/IMU │  │ 其他子设备   │
 * │AP3216│   │ ICM-20608│  │ (GPIO/IIO..)│
 * └──────┘   └──────────┘  └─────────────┘
 * 
 * 数据流:
 * 传感器中断 → 工作队列 → push_sample() → 融合到latest_sample 
 *          → 推入ringbuf → 唤醒read_wq → 用户空间read()
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include "indaq_core.h"
#include "indaq_i2csens.h"
#include "indaq_imu.h"

/* ========== 全局变量 ========== */

/**
 * 全局设备指针,供子设备工作队列调用 push_sample() 时使用
 * 
 * @note 这是一个简化设计,假设系统中只有一个 indaq 实例
 *       如需支持多实例,应通过 indev->i2csens/imu 等私有数据传递
 */
static struct indaq_device *g_indev;

/* ========== 文件操作接口实现 ========== */

/**
 * 设备打开回调 - 当用户空间 open("/dev/indaq") 时调用
 * 
 * @param inode VFS inode 节点,包含 cdev 信息
 * @param filp  文件结构指针,用于存储私有数据
 * @return 0 成功
 * 
 * 功能:
 * - 通过 container_of 从 cdev 反推出 indaq_device 结构
 * - 将设备指针保存到 filp->private_data,供后续 read/ioctl 使用
 */
static int indaq_open(struct inode *inode, struct file *filp)
{
	struct indaq_device *indev;

	/* 从 inode 中的 cdev 指针恢复完整的 indaq_device 结构 *///通过indaq_device 的成员cdev的指针获取indaq_device结构体的指针
	indev = container_of(inode->i_cdev, struct indaq_device, cdev);
	
	/* 保存设备上下文到文件私有数据,后续操作可直接访问 */
	filp->private_data = indev;
	return 0;
}

/**
 * 设备关闭回调 - 当用户空间 close(fd) 时调用
 * 
 * @param inode VFS inode 节点
 * @param filp  文件结构指针
 * @return 0 成功
 * 
 * @note 当前无需清理操作,预留接口以备将来扩展
 */
static int indaq_release(struct inode *inode, struct file *filp)
{
	return 0;
}

/**
 * 批量读取传感器样本 - 核心数据输出接口
 * 
 * @param filp   文件结构指针,包含 private_data (indaq_device)
 * @param buf    用户空间缓冲区指针
 * @param count  请求读取的字节数
 * @param f_pos  文件偏移指针(本驱动不使用,因为是非seekable设备)
 * @return 实际读取的字节数,或负值错误码
 * 
 * 工作流程:
 * 1. 计算可读取的最大样本数(count / sizeof(struct indaq_sample))
 * 2. 限制批量大小(最多64个样本),避免占用过多内核内存
 * 3. 分配临时内核缓冲区
 * 4. 检查 O_NONBLOCK 标志:
 *    - 非阻塞模式:若无数据立即返回 -EAGAIN
 *    - 阻塞模式:等待 read_wq 直到有数据或被信号中断
 * 5. 从环形缓冲区批量读取样本
 * 6. 拷贝到用户空间并更新统计信息
 * 
 * @note 此函数是数据路径的关键部分,需要高效执行
 *       环形缓冲区的无锁设计保证了高并发性能
 */
static ssize_t indaq_read(struct file *filp, char __user *buf,
			  size_t count, loff_t *f_pos)
{
	struct indaq_device *indev = filp->private_data;
	struct indaq_sample *samples;
	u32 max_samples = count / sizeof(struct indaq_sample);
	u32 n;
	ssize_t ret;

	/* 参数校验:至少能容纳一个完整样本 */
	if (!max_samples)
		return -EINVAL;

	/* 
	 * 限制批量大小,防止恶意用户请求过大缓冲区导致内核内存压力
	 * 64个样本约 64 * 40字节 = 2.5KB,合理范围
	 */
	if (max_samples > 64)
		max_samples = 64;

	/* 分配临时内核缓冲区用于批量传输 */
	samples = kmalloc_array(max_samples, sizeof(*samples), GFP_KERNEL);
	if (!samples)
		return -ENOMEM;

	/* 
	 * 等待数据就绪(尊重 O_NONBLOCK 标志)
	 * 
	 * 阻塞模式下的典型场景:
	 * - 用户线程调用 read() 但缓冲区为空
	 * - 线程进入睡眠状态(wait_event_interruptible)
	 * - 传感器工作队列推送新样本后调用 wake_up_interruptible()
	 * - 用户线程被唤醒,继续执行数据拷贝
	 */
	if (filp->f_flags & O_NONBLOCK) {
		/* 非阻塞模式:立即检查是否有数据 */
		if (indev->ringbuf->count == 0) {
			ret = -EAGAIN;  /* 资源暂时不可用 */
			goto out_free;
		}
	} else {
		/* 
		 * 阻塞模式:等待直到有数据或收到信号
		 * 
		 * wait_event_interruptible 特性:
		 * - 原子性地检查条件并进入睡眠
		 * - 可被信号中断(返回 -ERESTARTSYS)
		 * - 避免竞态条件(相比先检查再睡眠的方式)
		 */
		ret = wait_event_interruptible(indev->read_wq,
					       indev->ringbuf->count > 0);
		if (ret)
			goto out_free;  /* 被信号中断 */
	}

	/* 从环形缓冲区批量读取样本(无锁操作) */
	n = indaq_ringbuf_read(indev->ringbuf, samples, max_samples);
	if (!n) {
		/* 理论上不应发生(因为前面已检查 count > 0) */
		ret = 0;
		goto out_free;
	}

	/* 拷贝数据到用户空间 */
	if (copy_to_user(buf, samples,
			 n * sizeof(struct indaq_sample))) {
		ret = -EFAULT;  /* 用户空间地址无效 */
		goto out_free;
	}

	/* 更新累计样本计数(用于统计和调试) */
	indev->total_samples += n;
	ret = n * sizeof(struct indaq_sample);  /* 返回实际字节数 */

out_free:
	kfree(samples);  /* 释放临时缓冲区 */
	return ret;
}

/**
 * IOCTL 控制接口 - 提供设备配置和状态查询
 * 
 * @param filp 文件结构指针
 * @param cmd  IOCTL 命令码
 * @param arg  命令参数(可能是整数或指针)
 * @return 0 成功,或负值错误码
 * 
 * 支持的命令:
 * - INDAQ_IOCTL_GET_INFO:          获取设备状态信息
 * - INDAQ_IOCTL_START_CAPTURE:     启动数据采集
 * - INDAQ_IOCTL_STOP_CAPTURE:      停止数据采集
 * - INDAQ_IOCTL_SET_SAMPLING_RATE: 设置采样率(1-100Hz)
 */
static long indaq_ioctl(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	struct indaq_device *indev = filp->private_data;
	struct indaq_info info;
	int ret = 0;

	switch (cmd) {
	case INDAQ_IOCTL_GET_INFO:
		/* 
		 * 填充设备状态信息并返回给用户空间
		 * 
		 * 应用场景:
		 * - 监控程序定期检查采集状态
		 * - 诊断工具验证设备是否正常工作
		 */
		info.version = 0x0100;  /* 硬编码版本号 v1.0 */
		info.sampling_rate = indev->sampling_rate;
		info.total_samples = indev->total_samples;
		info.errors = 0;  /* 预留字段,当前未实现错误计数 */
		
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			ret = -EFAULT;
		break;

	case INDAQ_IOCTL_START_CAPTURE:
		/* 
		 * 启动数据采集流程
		 * 
		 * 操作步骤:
		 * 1. 清空环形缓冲区(丢弃旧数据,从干净状态开始)
		 * 2. 设置采集激活标志(原子操作,保证线程安全)
		 * 3. 记录日志便于调试
		 * 
		 * @note 子设备的工作队列会检查 capture_active 标志
		 *       只有激活时才会推送样本
		 */
		indaq_ringbuf_reset(indev->ringbuf);
		atomic_set(&indev->capture_active, 1);
		dev_info(indev->dev, "Capture started\n");
		break;

	case INDAQ_IOCTL_STOP_CAPTURE:
		/* 
		 * 停止数据采集
		 * 
		 * 效果:
		 * - 子设备工作队列检测到标志为0后停止推送
		 * - 已缓冲的数据仍可被读取
		 * - 不会丢失正在处理的中断(优雅停止)
		 */
		atomic_set(&indev->capture_active, 0);
		dev_info(indev->dev, "Capture stopped\n");
		break;

	case INDAQ_IOCTL_SET_SAMPLING_RATE: {
		u32 rate_hz;

		/* 从用户空间读取目标采样率 */
		if (copy_from_user(&rate_hz, (u32 __user *)arg,
				   sizeof(rate_hz))) {
			ret = -EFAULT;
			break;
		}
		
		/* 参数有效性检查:限制在硬件支持范围内 */
		if (rate_hz < 1 || rate_hz > 100) {
			ret = -EINVAL;
			break;
		}
		
		/* 更新核心配置的采样率 */
		indev->sampling_rate = rate_hz;
		
		/* 
		 * 将采样率转换为毫秒间隔,并下发到 I2C 传感器
		 * 
		 * 计算逻辑:
		 * - 100 Hz → 10ms 间隔
		 * - 50 Hz  → 20ms 间隔
		 * - 1 Hz   → 1000ms 间隔
		 * 
		 * @note 这里只更新了 AP3216C,I2C 传感器的采样间隔
		 *       IMU 的采样率可能需要单独配置(取决于硬件设计)
		 */
		ret = indaq_i2csens_set_interval(1000 / rate_hz);
		if (ret) {
			dev_err(indev->dev,
				"Failed to set sensor interval: %d\n", ret);
			break;
		}
		dev_info(indev->dev, "Sampling rate set to %u Hz\n",
			 rate_hz);
		break;
	}

	default:
		/* 不支持的命令 */
		ret = -ENOTTY;  /* Inappropriate ioctl for device */
		break;
	}

	return ret;
}

/**
 * 文件操作表 - 注册到 VFS 层
 * 
 * 定义了用户空间可以调用的所有文件操作
 */
static const struct file_operations indaq_fops = {
	.owner          = THIS_MODULE,        /**< 模块所有者,防止卸载时仍有引用 */
	.open           = indaq_open,         /**< 打开设备 */
	.release        = indaq_release,      /**< 关闭设备 */
	.read           = indaq_read,         /**< 读取传感器数据 */
	.unlocked_ioctl = indaq_ioctl,        /**< 无锁 IOCTL 控制 */
};

/* ========== 样本推送接口(由子设备工作队列调用) ========== */

/**
 * 推送 AP3216C 环境光/接近/红外传感器样本到环形缓冲区
 * 
 * @param als 环境光强度值(0-65535)
 * @param ps  接近传感器值(检测物体距离)
 * @param ir  红外光强度值
 * 
 * 调用上下文:
 * - AP3216C I2C 工作队列(indaq_i2csens.c 中定义)
 * - 周期性触发,频率由采样率决定
 * 
 * 数据融合机制:
 * 1. 复制 latest_sample 保留 IMU 等其他传感器的最新数据
 * 2. 更新 AP3216C 相关字段(als/ps/ir)
 * 3. 更新时间戳为当前时刻
 * 4. 将融合后的完整样本推入环形缓冲区
 * 5. 唤醒等待读取的用户空间进程
 * 
 * @note 此函数可能被频繁调用(最高100Hz),需保持轻量级
 *       不进行内存分配,不持有锁,保证实时性
 */
void indaq_push_sample(u16 als, u16 ps, u16 ir)
{
	struct indaq_device *indev = g_indev;
	struct indaq_sample s;

	/* 安全检查:设备未初始化或采集未启动时直接返回 */
	if (!indev || !atomic_read(&indev->capture_active))
		return;

	/* 
	 * 数据融合:保留其他传感器数据,仅更新 AP3216C 字段
	 * 
	 * 设计优势:
	 * - 即使各传感器采样时刻不同步,也能得到完整的多维数据
	 * - 用户空间每次读取都获得包含所有传感器信息的样本
	 * - 简化了应用层的数据对齐逻辑
	 */
	s = indev->latest_sample;
	s.ts_ns = ktime_get_ns();  /* 纳秒级时间戳 */
	s.als = als;
	s.ps = ps;
	s.ir = ir;
	indev->latest_sample = s;  /* 更新最新样本缓存 */

	/* 推送到环形缓冲区(无锁操作,高性能) */
	indaq_ringbuf_push(indev->ringbuf, &s);
	
	/* 唤醒阻塞在 read() 的用户空间进程 */
	wake_up_interruptible(&indev->read_wq);
}

/**
 * 推送 ICM-20608 IMU 六轴+温度样本到环形缓冲区
 * 
 * @param ax   X轴加速度计原始数据(单位:LSB,需校准转换为 m/s²)
 * @param ay   Y轴加速度计原始数据
 * @param az   Z轴加速度计原始数据
 * @param gx   X轴陀螺仪原始数据(单位:LSB,需校准转换为 rad/s)
 * @param gy   Y轴陀螺仪原始数据
 * @param gz   Z轴陀螺仪原始数据
 * @param temp 温度传感器原始数据(需转换为摄氏度)
 * 
 * 调用上下文:
 * - ICM-20608 SPI 工作队列(indaq_imu.c 中定义)
 * - 通常以固定频率采样(如 100Hz)
 * 
 * 数据融合机制:
 * 与 indaq_push_sample() 类似,但更新的是 IMU 字段
 * 保留 AP3216C 等其他传感器的最新数据
 * 
 * @note IMU 数据通常是 signed 类型(有正负方向)
 *       而环境光传感器是 unsigned 类型(只有正值)
 */
void indaq_push_imu_sample(s16 ax, s16 ay, s16 az,
			   s16 gx, s16 gy, s16 gz, s16 temp)
{
	struct indaq_device *indev = g_indev;
	struct indaq_sample s;

	/* 安全检查:设备未初始化或采集未启动时直接返回 */
	if (!indev || !atomic_read(&indev->capture_active))
		return;

	/* 数据融合:保留其他传感器数据,仅更新 IMU 字段 */
	s = indev->latest_sample;
	s.ts_ns = ktime_get_ns();  /* 更新为当前时间戳 */
	s.ax = ax;
	s.ay = ay;
	s.az = az;
	s.temp = temp;
	s.gx = gx;
	s.gy = gy;
	s.gz = gz;
	indev->latest_sample = s;  /* 更新最新样本缓存 */

	/* 推送到环形缓冲区 */
	indaq_ringbuf_push(indev->ringbuf, &s);
	
	/* 唤醒等待读取的用户空间进程 */
	wake_up_interruptible(&indev->read_wq);
}

/* ========== 平台驱动实现 ========== */

/**
 * 平台设备探测函数 - 驱动加载时的初始化入口
 * 
 * @param pdev 平台设备指针,包含设备树解析的资源
 * @return 0 成功,或负值错误码
 * 
 * 初始化流程(按依赖顺序):
 * 1. 分配并初始化核心数据结构
 * 2. 创建环形缓冲区(Phase 4)
 * 3. 注册字符设备(/dev/indaq)
 * 4. 解析设备树配置
 * 5. 依次初始化各子设备模块
 * 
 * 错误处理策略:
 * - 关键资源失败(内存/字符设备):立即返回错误
 * - 子设备初始化失败:打印警告但继续(允许部分功能可用)
 */
static int indaq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct indaq_device *indev;
	int ret;

	dev_info(dev, "INDAQ driver v%s probing...\n", INDAQ_DRV_VERSION);

	/* 
	 * 分配核心设备结构体(使用 devm_ 自动管理生命周期)
	 * 
	 * devm_kzalloc 优势:
	 * - 自动零初始化
	 * - 设备移除时自动释放,避免内存泄漏
	 * - 简化错误处理路径
	 */
	indev = devm_kzalloc(dev, sizeof(*indev), GFP_KERNEL);
	if (!indev)
		return -ENOMEM;

	/* 初始化基础字段 */
	indev->pdev = pdev;
	indev->dev = dev;
	indev->sampling_rate = 1000;  /* 默认采样率 1000Hz? 可能需要调整 */
	atomic_set(&indev->capture_active, 0);  /* 初始状态:未采集 */
	mutex_init(&indev->lock);               /* 初始化配置锁 */
	init_waitqueue_head(&indev->read_wq);   /* 初始化等待队列 */

	/* Phase 4: 分配环形缓冲区 */
	/**
	 * 环形缓冲区是多传感器数据的核心存储
	 * 
	 * 容量配置:
	 * - INDAQ_RINGBUF_DEFAULT_SIZE 通常在 indaq_ringbuf.h 中定义
	 * - 建议大小:能容纳数秒的数据(如 100Hz * 5秒 = 500样本)
	 * - 过大浪费内存,过小容易溢出丢数据
	 */
	indev->ringbuf = indaq_ringbuf_create(INDAQ_RINGBUF_DEFAULT_SIZE);
	if (!indev->ringbuf) {
		dev_err(dev, "Failed to allocate ring buffer\n");
		return -ENOMEM;
	}

	/* 保存设备指针供后续使用 */
	platform_set_drvdata(pdev, indev);
	g_indev = indev;  /* 设置全局指针供子设备使用 */

	/* 从设备树读取采样率配置(可选,有默认值) */
	of_property_read_u32(dev->of_node, "indaq,sampling-rate",
			     &indev->sampling_rate);
	dev_info(dev, "Sampling rate: %u Hz\n", indev->sampling_rate);

	/* ========== 注册字符设备 ========== */
	
	/* 动态分配设备号(主设备号由内核自动分配) */
	ret = alloc_chrdev_region(&indev->devno, 0, 1, INDAQ_DRV_NAME);
	if (ret) {
		dev_err(dev, "Failed to alloc chrdev region\n");
		return ret;
	}

	/* 初始化字符设备结构,关联文件操作表 */
	cdev_init(&indev->cdev, &indaq_fops);
	indev->cdev.owner = THIS_MODULE;
	
	/* 将字符设备添加到系统 */
	ret = cdev_add(&indev->cdev, indev->devno, 1);
	if (ret) {
		dev_err(dev, "Failed to add cdev\n");
		goto err_cdev;
	}

	/* 创建设备类(用于 sysfs 和 udev 自动创建设备节点) */
	indev->cls = class_create(THIS_MODULE, INDAQ_DRV_NAME);
	if (IS_ERR(indev->cls)) {
		ret = PTR_ERR(indev->cls);
		dev_err(dev, "Failed to create class\n");
		goto err_class;
	}

	/* 创建设备节点 /dev/indaq */
	device_create(indev->cls, NULL, indev->devno, NULL,
		      INDAQ_DRV_NAME);

	dev_info(dev, "INDAQ probe successful! /dev/indaq (major=%d)\n",
		 MAJOR(indev->devno));

	/* ========== 初始化子设备模块(按Phase顺序) ========== */
	
	/* Phase 2: I2C 环境光/接近传感器(AP3216C) */
	ret = indaq_i2csens_init(indev);
	if (ret)
		dev_warn(dev, "I2C sensor init failed: %d\n", ret);

	/* Phase 3: IMU 惯性测量单元(ICM-20608) */
	ret = indaq_imu_init(indev);
	if (ret)
		dev_warn(dev, "IMU init failed: %d\n", ret);

	/* Phase 6: GPIO 控制模块 */
	ret = indaq_gpioctrl_init(indev);
	if (ret)
		dev_warn(dev, "GPIO control init failed: %d\n", ret);

#if IS_ENABLED(CONFIG_IIO)
	/* Phase 5: IIO 子系统接口(可选,需内核配置支持) */
	ret = indaq_iio_init(indev);
	if (ret)
		dev_warn(dev, "IIO init failed: %d\n", ret);
#endif

	/* Phase 8: DebugFS 调试文件系统 */
	ret = indaq_debug_init(indev);
	if (ret)
		dev_warn(dev, "Debug init failed: %d\n", ret);

	/* 电源管理模块 */
	ret = indaq_pm_init(indev);
	if (ret)
		dev_warn(dev, "PM init failed: %d\n", ret);

	/* Phase 7: 传感器校准模块 */
	ret = indaq_calib_init(indev);
	if (ret)
		dev_warn(dev, "Calib init failed: %d\n", ret);

	/* Phase 9: Linux 输入子系统接口 */
	ret = indaq_input_init(indev);
	if (ret)
		dev_warn(indev->dev, "Input init failed: %d\n", ret);

	return 0;

/* 错误处理:逆序清理已分配的资源 */
err_class:
	cdev_del(&indev->cdev);
err_cdev:
	unregister_chrdev_region(indev->devno, 1);
	return ret;
}

/**
 * 平台设备移除函数 - 驱动卸载时的清理入口
 * 
 * @param pdev 平台设备指针
 * @return 0 成功
 * 
 * 清理流程(与 probe 相反的顺序):
 * 1. 清除全局指针(防止子设备继续访问)
 * 2. 逆序清理各子设备模块
 * 3. 销毁环形缓冲区
 * 4. 注销字符设备和设备节点
 * 
 * @note 使用 devm_ 分配的资源会自动释放,无需手动清理
 */
static int indaq_remove(struct platform_device *pdev)
{
	struct indaq_device *indev = platform_get_drvdata(pdev);

	/* 清除全局指针,阻止子设备工作队列继续推送数据 */
	g_indev = NULL;

	/* 
	 * 逆序清理子设备模块(与 probe 中的初始化顺序相反)
	 * 
	 * 原则:后初始化的先清理,确保依赖关系正确
	 */
	indaq_i2csens_exit(indev);
	indaq_imu_exit(indev);
	indaq_gpioctrl_exit(indev);
#if IS_ENABLED(CONFIG_IIO)
	indaq_iio_exit(indev);
#endif
	indaq_input_exit(indev);
	indaq_calib_exit(indev);
	indaq_debug_exit(indev);
	indaq_pm_exit(indev);

	/* 销毁环形缓冲区,释放所有缓冲的样本 */
	indaq_ringbuf_destroy(indev->ringbuf);
	indev->ringbuf = NULL;

	/* 注销字符设备和设备节点 */
	device_destroy(indev->cls, indev->devno);
	class_destroy(indev->cls);
	cdev_del(&indev->cdev);
	unregister_chrdev_region(indev->devno, 1);

	dev_info(&pdev->dev, "INDAQ removed\n");
	return 0;
}

/* ========== 设备树匹配表 ========== */

/**
 * Open Firmware 设备树匹配表
 * 
 * 用途:
 * - 内核通过 compatible 字符串匹配驱动和设备
 * - 支持热插拔设备的自动绑定
 * 
 * 设备树示例:
 * ```
 * indaq@0 {
 *     compatible = "atomic,imx6ul-indaq";
 *     indaq,sampling-rate = <100>;
 *     ...
 * };
 * ```
 */
static const struct of_device_id indaq_of_match[] = {
	{ .compatible = "atomic,imx6ul-indaq" },
	{ /* sentinel */ }  /* 空项作为结束标记 */
};
MODULE_DEVICE_TABLE(of, indaq_of_match);  /* 导出给 modprobe 用于自动加载 */

/* ========== 平台驱动结构体 ========== */

/**
 * 平台驱动注册结构
 * 
 * 包含:
 * - probe/remove 回调函数
 * - 驱动名称(用于 sysfs)
 * - 设备树匹配表
 */
static struct platform_driver indaq_driver = {
	.probe  = indaq_probe,   /**< 设备探测回调 */
	.remove = indaq_remove,  /**< 设备移除回调 */
	.driver = {
		.name = INDAQ_DRV_NAME,              /**< 驱动名称 */
		.of_match_table = indaq_of_match,    /**< 设备树匹配表 */
	},
};

/* ========== 模块初始化/退出 ========== */

/**
 * 模块初始化函数 - insmod 时调用
 * 
 * @return 0 成功,或负值错误码
 * 
 * 初始化顺序:
 * 1. 注册平台驱动(等待设备树匹配的设备出现)
 * 2. 注册 I2C 传感器驱动(AP3216C)
 * 3. 注册 IMU 驱动(ICM-20608)
 * 
 * 错误处理:
 * - 任一步骤失败则回滚之前成功的注册
 * - 确保不会留下半初始化的状态
 */
static int __init indaq_module_init(void)
{
	int ret;

	/* 第一步:注册平台驱动 */
	ret = platform_driver_register(&indaq_driver);
	if (ret)
		return ret;

	/* 第二步:注册 I2C 传感器驱动 */
	ret = indaq_register_i2c_driver();
	if (ret)
		goto err_i2c;

	/* 第三步:注册 IMU 驱动 */
	ret = indaq_register_imu_driver();
	if (ret)
		goto err_imu;

	pr_info("INDAQ: module loaded (v%s)\n", INDAQ_DRV_VERSION);
	return 0;

/* 错误回滚 */
err_imu:
	indaq_unregister_i2c_driver();
err_i2c:
	platform_driver_unregister(&indaq_driver);
	return ret;
}

/**
 * 模块退出函数 - rmmod 时调用
 * 
 * 清理顺序(与初始化相反):
 * 1. 注销 IMU 驱动
 * 2. 注销 I2C 驱动
 * 3. 注销平台驱动(触发 remove 回调)
 */
static void __exit indaq_module_exit(void)
{
	indaq_unregister_imu_driver();
	indaq_unregister_i2c_driver();
	platform_driver_unregister(&indaq_driver);
	pr_info("INDAQ: module unloaded\n");
}

/* 声明模块入口/出口函数 */
module_init(indaq_module_init);
module_exit(indaq_module_exit);

/* ========== 模块元数据 ========== */

MODULE_LICENSE("GPL v2");                          /**< 许可证:GPL v2,符合内核模块要求 */
MODULE_AUTHOR("Your Name");                        /**< 作者信息(需替换为实际作者) */
MODULE_DESCRIPTION("i.MX6U Industrial Data Acquisition Core Driver");  /**< 模块描述 */
MODULE_VERSION(INDAQ_DRV_VERSION);                 /**< 模块版本号 */
