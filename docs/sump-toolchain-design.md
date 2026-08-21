# SUMP 工具链设计文档（P6-2，手册 5.15.2/5.15.4）

> 设计定稿：2026-08-20。本文档记录协议/架构决策与调试手册，与 `protocol/README.md`
> （使用指南）互补：README 面向使用者，本文面向维护者/评审者。

## 1. 架构决策

### ADR-1：为什么用事件驱动等效采样，而非硬件全速采样

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| 硬件全速采样（专用逻辑分析仪） | 波形真实、时序精确 | 需额外硬件/FPGA/大容量缓冲，成本高 | ✗ 本项目无此硬件 |
| 软件轮询采样（读 GPIO 寄存器） | 实现简单 | 采样率受轮询周期限制（µs 级），8 通道并发抖动大，CPU 占用高 | ✗ 时序不可靠 |
| **事件驱动等效采样（本方案）** | 零额外硬件；GPIO 中断天然带高精度时间戳（ns 级）；边沿准确 | 等效采样率受事件产生率限制；静默信号保持上一电平 | ✓ **选此方案** |

**关键支撑**：P3 的 gpio_event 驱动已具备：硬件 GPIO 中断（ns 级单调时钟时间戳）+ kfifo 无丢失 + 初始快照（START 时逐通道电平）——完整的事件流基础设施，SUMP 层只做"事件 → 网格"重建。

### ADR-2：为什么选 SUMP 协议

- **SUMP**（SUMP Logic Analyzer Protocol）是开放协议，被 **PulseView/sigrok**（跨平台开源逻辑分析仪软件）原生支持；
- 相比自研协议：无需自写 PC 端查看器/协议解码器，PulseView 自带 I2C/UART/SPI 等几十种解码器；
- OLS（OpenBench Logic Sniffer）是主流 SUMP 实现，sigrok 对其驱动支持成熟，协议细节可从其源码精确对照。

### ADR-3：协议实现依据（源码级对照）

协议细节**逐条对照 libsigrok OLS 驱动源码**（`src/hardware/openbench-logic-sniffer/` 的
`protocol.c`/`api.c`/`protocol.h`）确认，不凭记忆/二手资料：

| 协议点 | 依据（sigrok 源码行为） | 本实现 |
|---|---|---|
| 握手 | 5×Reset 后发 ID，等 20ms，读 4 字节比较 `1SLO`/`1ALS` | 回复 `1ALS`（对应 SLA1） |
| metadata 整数 | `RB32` 读（大端） | 大端发送 |
| 长命令参数 | `WL32`/`WL16` 写（小端） | 小端解析 |
| 采样率 | `R = 100MHz/(divider+1)` | 同左；DEMUX 时 ×2 |
| 样本数 | `readcount=(samplecount+3)/4`，发 `readcount-1`；`max_samples>256K` 用 0x84/0x83 32 位 | 同左；metadata 声明 4MB 触发 32 位路径 |
| 样本顺序 | 设备倒序发送（最新先发），sigrok 反转后时间正序 | 倒序发送 |
| 结束判定 | 100ms 无数据超时 | 样本流连续发送（块间 <100ms） |
| flags | bit0 DEMUX / bit2-5 组禁用 / bit8 RLE | 仅解析 DEMUX；忽略 RLE（不支持） |
| 触发 | 无触发时强制 stage0 mask=0/value=0/start | 始终全量采集，由 sigrok 侧按 delaycount 切分 |

### ADR-4：为什么 metadata 声明采样内存 4MB、最大采样率 2MHz

- **4MB**：>256K 触发 sigrok 的 32 位 readcount 命令路径（0x84/0x83），
  支撑手册验收要求的 **1M 样本**（1M > 256K，若声明 ≤256K 则 sigrok 走 16 位路径，无法请求 1M）；
- **2MHz**：400kHz I2C 解码需要 ≥ 采样率（每 bit 至少 2 样本），2MHz 满足；
  真实等效采样率受 GPIO 事件率限制（远低），此声明仅为 PulseView 可选上限，
  已在 `protocol/README.md` 以**重要声明**形式写明，防止用户误判精度。

### ADR-5：为什么 sim 后端用固定时间戳（t0=1s）

自测可重复性：固定时间戳 + 固定频率方波 → selftest 可做**确定性断言**
（bit0 翻转次数 ≈ 期望值），不依赖真实时钟抖动；真实板端走 edt 后端（真实单调时钟）。

## 2. 协议状态机（sump_server.c）

```
IDLE ──ID(0x02)──► 回复 "1ALS" ──► IDLE
IDLE ──META(0x04)─► 回复 metadata 键值流 ──► IDLE
IDLE ──divider/flags/readcount/delaycount──► 记录参数 ──► IDLE
IDLE ──RUN(0x01)──► [采集→重建→倒序发送样本流] ──► IDLE
任何状态 ──RESET(0x00)──► 参数清零 ──► IDLE
```

命令分类：短命令（1 字节）与长命令（1 字节 opcode + 4 字节参数小端）。
长命令判定 `0x80-0x9F` 与 `0xC0-0xCF`。未知命令忽略（协议健壮性）。

## 3. 采集与重建流程（run_acquisition）

1. **定采样率**：`R = 100MHz/(divider+1)`，DEMUX 时 ×2
2. **定样本数**：`readcount*4`（readcount 已恢复 +1），上限 16M（内存保护）
3. **定窗口**：`窗口 = 样本数 × (1e9/R) 纳秒`，受 `--max-window`（默认 10s）保护
4. **采集事件**：读事件直到 ①事件时间跨度 ≥ 窗口，或 ②事件静止 200ms，或 ③窗口上限
   - 首次事件（快照）时间戳 = t0
5. **重建网格**：快照初始化 8 通道电平 → 逐网格点推进边沿事件 → 打包样本字节（bit ch = level[ch]）
   - 同网格单元多边沿：取最后生效，`collisions` 计数（欠采样证据）
   - 越窗事件：`events_beyond` 计数
6. **倒序发送**：`samples[N-1] → samples[0]`（最新在前）——OLS 约定，sigrok 反转后时间正序
7. **统计守恒**：`queued+dropped == irq+snapshot` 由驱动 GET_STATS 验证（P3 已实现）

## 4. 性能与内存

| 参数 | 值 | 说明 |
|---|---|---|
| 网格缓冲 | 样本数 × 1 字节 | 1M 样本 = 1MB；上限 16M = 16MB |
| 事件缓冲 | 动态增长，上限 4M 事件 | 4M × 16B = 64MB 上限 |
| 重建复杂度 | O(事件数 + 样本数) | 两遍扫描，1M 样本 + 4 万事件 ≈ 毫秒级 |
| 发送带宽 | 1 字节/样本 | 1M 样本 = 1MB，百兆网 < 100ms 连续发送 |

## 5. 调试手册（故障排查）

| 现象 | 排查 |
|---|---|
| PulseView 连接不上 | 板端 `sump_server` 是否运行；`ss -tlnp \| grep 9527` 确认监听；防火墙；IP 是否可达（`ping`） |
| 连接后识别为"未知设备" | 握手失败：ID 回复是否 `1ALS`；用 `sump_selftest` 先验证协议 |
| 连接后设备名显示异常 | metadata 格式：字符串须 NUL 结尾，整数须大端 |
| 采集无波形/全平线 | 被测信号是否产生边沿中断（`cat /proc/interrupts` 计数增加）；GPIO 是否配置为输入且使能中断（dts 检查）；**等效采样限制**：高频信号事件率超上限时波形失真属预期 |
| 波形时间反向 | 样本未倒序发送（检查 `run_acquisition` 倒序逻辑） |
| 样本数不足/超时提前结束 | 窗口保护（`--max-window`）触发；事件静止判定（200ms）过早——低速信号可增大 `--max-window` 或提高采样率 |
| 1M 样本内存不足 | 16M 上限保护触发——降低样本数 |
| 板端 CPU 占用高 | 事件率过高（高频输入）导致重建/发送忙；属预期，降低采样率/样本数 |

## 6. 局限与后续

- **不支持 RLE/噪声滤波/高级触发**：sigrok 默认不启用，PulseView 基本使用无影响；后续可扩展
- **不支持 demux 模式**（flags bit0）：多组 4 通道时分复用，8 通道单组场景不需要
- **单客户端**：sigrok 单连接模型，多客户端需排队（当前 accept 串行处理）
- **触发切分依赖 sigrok 侧**：设备始终全量采集，预触发/触发点标记由 sigrok 按 delaycount 计算