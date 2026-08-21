# 架构决策：自研 gpio_event vs input 子系统（P6，手册 5.15.1）

> 项目 B 驱动架构决策记录：为什么 P3 用自研字符设备、P6 又引入 input 子系统。

## 背景

本项目的按键事件采集经历了三代演进：

| 阶段 | 驱动 | 实现方式 | 消费方式 |
|------|------|----------|----------|
| P2 | `key_event` | 自研 misc 字符设备 + 自定义 UAPI | 自建 `key_test` 工具 |
| P3 | `gpio_event_capture` | 自研字符设备 + kfifo 多通道 | `capture_cli` / `capture_bench` |
| P6 | `key_input` | **标准 input 子系统** | `evtest` / Qt / evdev 直接消费 |

P6 不是 P2/P3 的替代，而是**并存**——两类接口服务不同场景。

## 对比表（手册 5.15.1 要求）

| 维度       | gpio_event（自研）          | input 子系统               |
|-----------|----------------------------|---------------------------|
| 接口       | 自定义 UAPI（ioctl/read/poll） | 标准 input API（input_event 结构） |
| 生态       | 自建工具链（cli/bench）      | evtest / Qt / evdev 直接消费 |
| 适用场景   | 高精度时间戳/自定义事件/多通道采集 | 标准按键/事件输入（系统级消费） |
| 时间精度   | 内核 ktime 纳秒时间戳，事件级 | 时间戳为 input_event 内嵌 timeval（微秒级） |
| 事件模型   | 自定义事件结构（可扩展任意字段） | 固定 (type, code, value) 三元组 |
| 多通道     | 原生支持（kfifo 多通道）     | 单设备多 key 位（capability 位图） |
| 学习价值   | 完整驱动链路（中断/同步/阻塞/IOCTL） | 子系统框架（input 核心层/事件层/设备层） |
| 依赖       | 无（纯字符设备框架）         | 依赖 input 核心层 + evdev 处理器 |
| 调试工具   | 自研 capture_cli             | evtest / hexdump / evemu-record |

**结论**：两者并存——采集高精度事件用自研（gpio_event），标准输入消费用 input（key_input）。

## 为何 P6 需要 input 子系统

1. **生态即用**：Qt 的 `QKeyEvent`、桌面 evdev、tslib、`/dev/input` 体系天然消费 input 事件；
   自研接口每对接一个应用都要写适配层。
2. **系统集成**：项目 A（Qt 应用）与项目 B 对接时，按键走 input 子系统是标准路径，
   无需自定义协议。
3. **学习价值**：input 子系统是 Linux 驱动四大核心子系统之一（input/pwm/i2c/spi），
   掌握其 `input_allocate_device → input_set_capability → input_register_device` 流程，
   为后续 LCD 触摸、摄像头 v4l2 等子系统驱动打基础。

## GPIO1_IO18 互斥策略

KEY0（GPIO1_IO18）被三代驱动共用，dts 中三者互斥（运行时只能加载其一）：

| 驱动 | dts 节点 | 默认状态 | 加载条件 |
|------|----------|----------|----------|
| key-event（P2） | `key-event` | disabled | 验证 P2 时启用 |
| gpio-event-capture（P3） | `gpio-event-capture` | **okay** | 当前主用（P3 已验收） |
| key-input（P6） | `key-input` | disabled | 验证 P6 时启用 |

验证 P6 步骤（二选一）：
- **dts 切换**：`key-input` 改 `okay` + `gpio-event-capture` 改 `disabled` → 重编 dtb → 重启 → `insmod key_input.ko`
- **运行时切换**：`rmmod gpio_event_capture` → 改 dts 后重启（dts 节点必须为 okay 才会 probe）

## 实现要点（P6 key_input.c）

1. **全 devm 资源管理**：`devm_gpiod_get` / `devm_input_allocate_device` /
   `devm_request_irq`，驱动 detach 自动释放，无需 remove 回调。
2. **gpiod 逻辑电平**：`gpiod_get_value` 返回逻辑电平（GPIO_ACTIVE_LOW 已翻转），
   按下=1 直接上报，避免按下/释放颠倒（P2 的 `level==0` 语义是裸电平视角）。
3. **按键码设备树化**：`linux,code` 属性指定（默认 KEY_ARMED=227），
   改键值无需改代码。
4. **标准事件上报**：`input_report_key` + `input_sync_key`，由 input 核心层
   自动分发到 evdev 客户端（/dev/input/eventX）。

## 相关文件

- 驱动：`core/key_input/key_input.c`
- 测试：`core/key_input/test/key_input_test.c`（自动探测 eventX）
- 设备树：`dts/imx6ull-southbay-emmc.dts`（key-input 节点）
- 对比：本文档