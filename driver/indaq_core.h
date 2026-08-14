/* SPDX-License-Identifier: GPL-2.0 */
/*
 * indaq_core.h - i.MX6U 工业数据采集核心驱动头文件
 * 
 * 采用 MFD (Multi-Function Device) 风格的平台驱动架构,管理多个子设备模块
 * 包括:I2C传感器、IMU惯性测量单元、GPIO控制、IIO子系统、校准、调试等
 * 
 * 设计思路:
 * - 核心驱动负责字符设备注册、数据缓冲、用户空间接口
 * - 各子设备模块独立初始化,通过 indaq_device 结构体共享状态
 * - 采用分阶段(Phase)设计,便于逐步开发和功能扩展
 */

#ifndef __INDAQ_CORE_H__
#define __INDAQ_CORE_H__

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include "indaq_ringbuf.h"

/* ========== 驱动基本信息定义 ========== */

/** 驱动名称,用于设备节点创建和匹配 */
#define INDAQ_DRV_NAME       "indaq"

/** 驱动版本号,格式:主版本.次版本.修订号 */
#define INDAQ_DRV_VERSION    "1.0.0"

/* ========== IOCTL 命令定义 ========== */

/** IOCTL 魔术数,用于唯一标识本驱动的 ioctl 命令 */
#define INDAQ_IOC_MAGIC      'I'

/**
 * 获取设备信息
 * @param struct indaq_info* 返回设备状态信息(只读)
 */
#define INDAQ_IOCTL_GET_INFO          _IOR(INDAQ_IOC_MAGIC, 0, struct indaq_info)

/**
 * 启动数据采集
 * @note 无参数,启动后传感器开始采样并填充环形缓冲区
 */
#define INDAQ_IOCTL_START_CAPTURE     _IO(INDAQ_IOC_MAGIC,  3)

/**
 * 停止数据采集
 * @note 无参数,停止后不再接收新样本,但已采集数据仍可读取
 */
#define INDAQ_IOCTL_STOP_CAPTURE      _IO(INDAQ_IOC_MAGIC,  4)

/**
 * 设置采样率
 * @param u32 采样频率(Hz),有效范围:1-100Hz
 */
#define INDAQ_IOCTL_SET_SAMPLING_RATE _IOW(INDAQ_IOC_MAGIC, 5, u32)

/* ========== 数据结构定义 ========== */

/**
 * 设备信息结构体 - 通过 IOCTL 返回给用户空间
 * 
 * 用途:应用程序查询当前设备状态和统计信息
 */
struct indaq_info {
	u32 version;           /**< 驱动版本号(如 0x0100 表示 v1.0) */
	u32 sampling_rate;     /**< 当前采样率(单位:Hz) */
	u32 total_samples;     /**< 累计采集的样本总数 */
	u32 errors;            /**< 错误计数器(预留字段) */
};

/**
 * 核心设备结构体 - 所有子设备共享的全局状态
 * 
 * 设计理念:
 * - 作为 MFD 的核心枢纽,统一管理所有子设备
 * - 子设备通过 void* 指针存储私有数据,实现解耦
 * - 提供线程安全的数据采集、缓冲、分发机制
 * 
 * 生命周期:
 * - probe() 时分配并初始化
 * - remove() 时清理释放
 */
struct indaq_device {
	/* --- 基础设备对象 --- */
	struct platform_device *pdev;  /**< 平台设备指针,用于访问设备树资源 */
	struct device *dev;            /**< Linux 设备对象,用于日志输出和 DMA 操作 */
	
	/* --- 字符设备相关 --- */
	struct cdev cdev;              /**< 字符设备结构,关联 file_operations */
	struct class *cls;             /**< 设备类,用于自动创建设备节点 */
	dev_t devno;                   /**< 设备号(主设备号+次设备号) */

	/* --- 运行状态 --- */
	atomic_t capture_active;       /**< 原子标志:采集是否激活(0=停止, 1=运行) */
	u32 total_samples;             /**< 累计推送至用户空间的样本总数 */
	u32 sampling_rate;             /**< 当前采样率配置(单位:Hz) */

	/* --- 最新融合样本 --- */
	/**
	 * 最新的多传感器融合样本
	 * 
	 * 工作机制:
	 * - 每个子设备(workqueue)更新自己负责的字段
	 * - 其他字段保留上一次的值,实现数据融合
	 * - 每次更新后立即推送到环形缓冲区
	 */
	struct indaq_sample latest_sample;

	/* --- Phase 4: 环形缓冲与阻塞读取 --- */
	struct indaq_ringbuf *ringbuf; /**< 无锁环形缓冲区,存储待读取的样本 */
	wait_queue_head_t read_wq;     /**< 等待队列,用于阻塞 read() 直到有数据 */

	/* --- 配置锁 --- */
	/**
	 * 互斥锁,保护配置修改操作
	 * 
	 * @note 注意:此锁不保护数据路径(环形缓冲区是无锁设计的)
	 *       仅用于保护采样率等配置的并发修改
	 */
	struct mutex lock;

	/* --- 子设备私有数据(按开发阶段逐步添加) --- */
	void *i2csens;      /**< I2C传感器私有数据(struct indaq_i2csens*) - Phase 2 */
	void *imu;          /**< IMU惯性测量单元私有数据(struct indaq_imu*) - Phase 3 */
	void *gpioctrl;     /**< GPIO控制模块私有数据(struct indaq_gpioctrl*) - Phase 6 */
	void *iio;          /**< IIO子系统接口(struct indaq_iio*) - Phase 5 */
	void *calib;        /**< 校准参数(struct indaq_calib_params*) - Phase 7 */
	void *input_dev;    /**< 输入设备(struct input_dev*) - Phase 9 */

	/* --- 调试支持 --- */
	struct dentry *debug_dir;  /**< debugfs 目录项,用于运行时调试 - Phase 8 */
};

/* ========== 子设备驱动注册接口 ========== */

/**
 * 注册 I2C 传感器驱动(AP3216C)
 * 
 * @return 0 成功, 负值错误码失败
 * @note 在模块初始化时调用,注册 I2C driver 结构体
 */
int indaq_register_i2c_driver(void);

/**
 * 注销 I2C 传感器驱动
 * 
 * @note 在模块退出时调用,清理 I2C driver 注册
 */
void indaq_unregister_i2c_driver(void);

/**
 * 注册 SPI/IMU 驱动(ICM-20608)
 * 
 * @return 0 成功, 负值错误码失败
 * @note 在模块初始化时调用,注册 SPI driver 结构体
 */
int indaq_register_imu_driver(void);

/**
 * 注销 SPI/IMU 驱动
 * 
 * @note 在模块退出时调用,清理 SPI driver 注册
 */
void indaq_unregister_imu_driver(void);

/* ========== 样本推送接口(由子设备工作队列调用) ========== */

/**
 * 推送 AP3216C 环境光/接近/红外传感器样本
 * 
 * @param als 环境光强度(Ambient Light Sensor),16位无符号值
 * @param ps  接近传感器(Proximity Sensor),16位无符号值,检测物体距离
 * @param ir  红外传感器(Infrared),16位无符号值
 * 
 * @note 此函数在中断上下文或工作队列中调用
 *       会自动融合到 latest_sample 并推送到环形缓冲区
 */
void indaq_push_sample(u16 als, u16 ps, u16 ir);

/**
 * 推送 ICM-20608 IMU 六轴+温度样本
 * 
 * @param ax   X轴加速度计原始数据,16位有符号值
 * @param ay   Y轴加速度计原始数据,16位有符号值
 * @param az   Z轴加速度计原始数据,16位有符号值
 * @param gx   X轴陀螺仪原始数据,16位有符号值
 * @param gy   Y轴陀螺仪原始数据,16位有符号值
 * @param gz   Z轴陀螺仪原始数据,16位有符号值
 * @param temp 温度传感器原始数据,16位有符号值
 * 
 * @note 此函数在 IMU 工作队列中调用
 *       会自动融合到 latest_sample 并推送到环形缓冲区
 */
void indaq_push_imu_sample(s16 ax, s16 ay, s16 az,
			   s16 gx, s16 gy, s16 gz, s16 temp);

/* ========== 子设备初始化/清理接口 ========== */

/**
 * 各子设备模块必须实现的初始化/清理接口
 * 
 * 调用时机:
 * - init: 在 indaq_probe() 中按顺序调用
 * - exit: 在 indaq_remove() 中逆序调用
 * 
 * 返回值约定:
 * - 0: 成功
 * - 负值错误码: 失败(probe 中会打印警告但继续)
 */

/** I2C传感器模块(AP3216C) */
int indaq_i2csens_init(struct indaq_device *indev);
void indaq_i2csens_exit(struct indaq_device *indev);

/** GPIO控制模块 */
int indaq_gpioctrl_init(struct indaq_device *indev);
void indaq_gpioctrl_exit(struct indaq_device *indev);

/** IIO子系统接口(Linux Industrial I/O) */
int indaq_iio_init(struct indaq_device *indev);
void indaq_iio_exit(struct indaq_device *indev);

/** IMU惯性测量单元(ICM-20608) */
int indaq_imu_init(struct indaq_device *indev);
void indaq_imu_exit(struct indaq_device *indev);

/** 调试文件系统(debugfs)支持 */
int indaq_debug_init(struct indaq_device *indev);
void indaq_debug_exit(struct indaq_device *indev);

/** 电源管理模块 */
int indaq_pm_init(struct indaq_device *indev);
void indaq_pm_exit(struct indaq_device *indev);

/** 传感器校准模块 */
int indaq_calib_init(struct indaq_device *indev);
void indaq_calib_exit(struct indaq_device *indev);

/** Linux 输入子系统接口 */
int indaq_input_init(struct indaq_device *indev);
void indaq_input_exit(struct indaq_device *indev);

#endif /* __INDAQ_CORE_H__ */
