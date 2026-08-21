# SUMP 工具链（P6-2，手册 5.15.2）

将 imx6ull 的 `gpio_event` 多通道边沿事件采集驱动暴露为 **SUMP 逻辑分析仪协议**，
供 PulseView（sigrok 图形前端）通过 TCP 连接查看 8 通道波形。

## 架构

```
┌────────────┐   /dev/edt_capture0   ┌──────────────┐   SUMP/TCP   ┌─────────────┐
│ gpio_event │ ────────────────────► │ sump_server  │ ◄──────────► │  PulseView  │
│  驱动(内核) │   16 字节边沿事件      │  (用户态)     │  tcp://IP:9527│  (PC 端)     │
└────────────┘                       └──────────────┘             └─────────────┘
```

| 文件 | 作用 |
|---|---|
| `capture_source.h/.c` | 数据源抽象：edt_capture 后端（真实采集）+ sim 后端（合成信号，自测/演示） |
| `event_reconstructor.h/.c` | 边沿事件流 → 固定采样率时间网格重建 |
| `sump_server.c` | SUMP 协议服务器（TCP:9527），协议状态机 |
| `sump_selftest.c` | 协议自测客户端（模拟 sigrok 握手/采集流程） |

## 构建

```bash
# 顶层一键构建（含 protocol 交叉编译，产物在 build/protocol/）
make

# 仅 protocol；PC 本机自测版（--sim 无需板端）
make -C protocol
make -C protocol host

# 部署到板端 NFS rootfs
make send
```

## 板端运行

```bash
# 先加载采集驱动（若未加载）
insmod gpio_event_capture.ko
# 启动 SUMP 服务器（8 通道全部采集）
./sump_server --device /dev/edt_capture0 --port 9527
# 查看调试日志
./sump_server --device /dev/edt_capture0 --verbose
```

## PC 端连接（PulseView）

1. 安装 PulseView（Windows/Linux）：https://sigrok.org/wiki/PulseView
2. 打开 PulseView → `File` → `Connect to Device...`
3. 连接类型选 **TCP**，地址填板端 IP 与端口：`tcp://192.168.x.x:9527`
4. 设备识别为 **OLS-compatible** 设备，8 通道可用
5. 选择采样率（建议 ≤ 2 MHz）与样本数（最多 1M 样本）
6. 点击 **Run** 采集，8 通道波形即时显示

采集完成后可在 PulseView 中对 SDA/SCL 等通道启用 **I2C 协议解码器**。

### 已知限制（TCP 实时连接）

> libsigrok **官方预编译包均未编译串口 TCP 传输**（serial_tcpraw），包括：
> - Windows 安装包：`tcp-raw/host/port` 资源被误路由到 libserialport（报"找不到指定的路径"）
> - Ubuntu apt 包（libsigrok4t64）：同样缺失该传输，探测静默失败
>
> 因此 PulseView/sigrok-cli 预编译版**无法通过 TCP 连接本服务器**。解决途径：
> - 从源码编译 libsigrok（OLS 驱动 + TCP 传输全量启用），即可正常实时连接
> - 或使用 `./P6_accept_sump.sh` 自动化验收（PASS=7 FAIL=0，协议/采集链路全验证）
>
> 本限制为第三方上位机软件的构建配置问题，与本工具链实现无关。

### 自测（无需板端）

```bash
# 编译 PC 版（顶层 host 目标，产物在 build/protocol/）
make host
# 或仅编译协议目录: make -C protocol host

# 终端 1 启动合成源服务器
./build/protocol/sump_server_host --sim --port 9527 --verbose
# 终端 2 运行协议自测（模拟 sigrok 完整握手 + 1M 样本采集）
./build/protocol/sump_selftest_host --host 127.0.0.1 --port 9527 --samples 1048576
```

## 重要声明（等效采样率限制）

> 本工具是**事件驱动等效采样**，不是硬件全速采样。
> 逻辑分析仪通道由 GPIO 中断边沿事件重建：只有当信号产生边沿中断时，
> 采样网格才会更新，网格其余点保持上一电平。
> **等效采样率受 GPIO 事件产生率限制**，远低于声明的 2 MHz 上限。
>
> - **适用**：低速协议调试（I2C ≤ 400 kHz、UART ≤ 115200 baud、按键/GPIO 逻辑分析）
> - **不适用**：高速同步采样（SPI 时钟 > 1 MHz、高速并行总线、需要精确时序毛刺检测的场景）
> - 采样率选项 2 MHz 仅为 PulseView 可选上限，实际波形精度取决于被测信号的事件密度；
>   高频信号将被欠采样为"保持电平"台阶，无法还原真实波形
>
> 1M 样本 × 低采样率时采集窗口可达数十秒，请合理选择采样率与样本数。

## 协议实现要点（对照 libsigrok OLS 驱动）

- 握手：5×Reset → ID → 回复 `1ALS`（对应 SLA1）
- metadata 键值流：字符串（0x00-0x1F）、32 位大端整数（0x20-0x3F）、8 位整数（0x40-0x5F）
- 长命令 5 字节参数**小端**；metadata 整数**大端**（sigrok 非对称读写）
- 采样率：`R = 100MHz/(divider+1)`，DEMUX 标志时 ×2
- 样本数：sigrok 发送 `readcount-1`（样本数/4 向上取整），设备返回 `readcount*4` 样本
- 样本**倒序发送**（最新在前），sigrok 反转后时间正序输出
- 发送须连续（块间间隔 < 100 ms，sigrok 超时判定）
- 触发：本实现始终全量采集（sigrok 无触发时强制立即触发；有触发由 sigrok 侧切分）