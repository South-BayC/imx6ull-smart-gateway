# 基准方法学（可复现）（P6，手册 5.15.3）

`gpio_event` 多通道事件采集驱动的性能基准。本文档为**可复现方法论**，
结果表待板端实测后填写（验收时执行）。

## 环境

| 项 | 值 |
|---|---|
| 平台 | i.MX6ULL 528 MHz 单核 |
| 内核 | 4.1.15（rel_imx_4.1.15_2.1.0_ga_southbay） |
| 根文件系统 | NFS 挂载 |
| 采集驱动 | gpio_event_capture.ko（8 通道，FIFO 1024 事件） |
| 对比基线 | libgpiod 逐次事件读取 |

## 激励

- 函数发生器：**1 kHz 方波**（50% 占空比）接入通道 0
- 激励时长：10 s（连续）
- 预期边沿率：1 kHz 方波 → **2000 边沿/秒**（上升+下降各 1000）

## 指标

| 指标 | 定义 | 来源 |
|---|---|---|
| 交付事件率 | 单位时间成功交付给用户态的事件数 | `read_events` / 时长 |
| 丢失率 | 丢弃事件占产生事件的百分比 | `dropped_events / irq_events × 100%` |
| 交付年龄 p50/p99 | 事件产生（`timestamp_ns`）到用户态读取的时刻差 | 采集循环内 `clock_gettime` 差值统计 |
| CPU 占用 | 采集进程 CPU 使用率 | `top` / `pidstat` |

统计字段定义见 UAPI `struct edt_capture_stats`（`irq_events`/`queued_events`/
`read_events`/`dropped_events`/`fifo_overflow_events`/`sample_error_events`），
守恒关系：`queued_events + dropped_events == irq_events + snapshot_events`。

## 步骤

1. 加载驱动：`insmod gpio_event_capture.ko`
2. 启动采集消费者（二选一）：
   - 基准模式：自定义循环读取 `/dev/edt_capture0` 并记录交付年龄分布
   - 端到端模式：启动 `sump_server`（P6-2），PulseView 以 1 MHz 采集 10 s
3. 启动激励：函数发生器输出 1 kHz 方波至通道 0，持续 10 s
4. 停止采集，读取 `GET_STATS` 统计与交付年龄直方图
5. 重复 3 次取中位，填入结果表

## 结果表模板

| 方案 | 激励率 | 交付率 | 丢失率 | p50 年龄 | p99 年龄 | CPU% |
|---|---|---|---|---|---|---|
| gpio_event（本次实现） | 2k edge/s | 待测 | 待测 | 待测 | 待测 | 待测 |
| libgpiod 基线 | 2k edge/s | 待测 | 待测 | 待测 | 待测 | 待测 |

> 实测后填写并保留原始输出（`stats` 结构体 dump）作为证据。
> 目标：0 丢失（FIFO 1024 事件容量，2k edge/s 下无压力）、
> 交付年龄 p99 < 10 ms。