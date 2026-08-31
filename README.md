# imx6ull-drivers — 边缘智能安防网关 + 全外设驱动工程

基于正点原子 I.MX6ULL ALPHA（EMMC 版）+ 7 寸 1024x600 触摸屏的嵌入式 Linux 综合项目，包含两个子项目：

- **项目 A · 边缘智能安防网关**（`core/lvgl/` 应用 + `cloud/` 云端服务）：LVGL 中文触摸界面，帧差粗判 + 本地人脸精判 + 云端复核（人员/类型判定 + 白名单比对）的两级入侵检测管线，告警分级声光、抓拍存证、时间轴、MQTT 上报。
- **项目 B · 全外设驱动工程**（`core/` 各驱动模块 + `protocol/`）：LED/按键中断/多通道事件采集（kfifo+tracepoint）/PWM 蜂鸣/I2C 光感/SPI 六轴/input 按键/GT9147 触摸，附 SUMP 逻辑分析仪工具链与全量自测。

| 项 | 内容 |
|---|---|
| 硬件 | i.MX6ULL（单核 Cortex-A7 792MHz）· 7 寸 RGB屏（mxsfb）· GT9147 触摸 · OV5640 CSI 摄像头 · TF 卡 · 以太网 |
| 系统 | Linux 4.1.15（Buildroot rootfs，NFS 开发模式） |
| 工具链 | arm-linux-gnueabihf-（4.9.4），ncnn 交叉编译 third_party/ncnn-armhf |
| 开发机 | Ubuntu VM（192.168.3.26，NFS rootfs + TFTP + 云端服务同机） |

---

## 一、项目结构

```
imx6ull-drivers/
├── Makefile                     # 顶层编译调度（all/core/dts/test/protocol/send/clean）
├── README.md                    # 本文件（总说明 + 使用文档）
│
├── core/                        # ── 项目 B：内核驱动模块（每模块含源码/UAPI/Makefile/dts/测试）──
│   ├── led/                     #   [P1] LED 字符设备（miscdevice+gpiod，/dev/led）
│   ├── key_event/               #   [P2] 按键中断驱动（阻塞/非阻塞/poll 三种读模式）
│   ├── gpio_event_capture/      #   [P3] 多通道事件采集（kfifo+时间戳+统计+tracepoint，ABI v2）
│   ├── pwm_beep/                #   [P4] PWM 蜂鸣器（/dev/beep_pwm0）
│   ├── ap3216c/                 #   [P4] I2C 光感/接近（regmap+iio_dev，sysfs）
│   ├── icm20608/                #   [P4] SPI 六轴 IMU（regmap+iio_dev，sysfs）
│   ├── key_input/               #   [P6-1] input 子系统按键（/dev/input/eventX）
│   ├── gt9147/                  #   [P7-1] 电容触摸驱动（中断模式，1024x600，含配置下载自愈）
│   ├── Makefile                 #   驱动批量编译（KDIR 指向内核源码树）
│   └── lvgl/                    # ── 项目 A：智能安防网关应用 ──
│       ├── Makefile             #   交叉编译（链接 ncnn；find src 自动收源）
│       ├── lv_conf.h            #   LVGL 配置（16bpp/部分刷新 60 行/内存 256KB）
│       ├── lvgl/                #   LVGL v9.2.2 源码（vendored）
│       └── src/
│           ├── main.c           #   入口：port 初始化 → UI → 桥接 → 主循环
│           ├── lvgl_port.c/h    #   LVGL 适配：fbdev + 触摸自动探测（按驱动名过滤 eventX）
│           ├── ui/              #   UI 层
│           │   ├── ui_home.c    #     主界面/弹窗/设置/相册/告警面板 + ui_events 实现
│           │   ├── ui_events.h  #     事件接口（状态机↔桥接↔UI 的唯一 API 面）
│           │   └── ui_widgets.c/h #   通用控件（卡片/按钮/模态/toast）
│           ├── dev_bridge.c/h   #   外设桥接线程+200ms 定时器：KEY0/传感器 IIO 采集 → UI；
│           │                    #     预警阈值配置；粗判命中节流提交；精判结论两级分发；
│           │                    #     告警分级声光（LED/蜂鸣）联动
│           ├── cam_feed.c/h     #   V4L2 采集线程（OV5640 640x480 YUYV）→ 630x340 RGB565
│           │                    #     NEON 查表转换 → 画布双缓冲发布；帧差粗判引擎（参数可调）
│           ├── detector.cpp/h   #   精判工作线程（nice+10）：两级管线——段1 ncnn SCRFD 人脸初判，
│           │                    #     段2 cloud_detect 云端复核定案；结论原子区 + 检出框发布
│           ├── cloud_detect.c/h #   云端复核客户端（原 TCP+HTTP POST，428KB 帧上传，2.5s 超时）
│           ├── mqtt_hub.c/h     #   MQTT 3.1.1 QoS0 零依赖客户端（事件上报，fire-and-forget）
│           ├── storage_mgr.c/h  #   抓拍持久化（TF 卡 BMP，满卡清理，重启相册回填）
│           ├── state_machine.c/h#   分区状态机（ONLINE/ARMED/ALARM/OFFLINE）
│           ├── font/            #   中文字库 SHSC 16/20px（3500 常用字大子集）
│           └── sim_main.c       #   PC 模拟器入口（SDL，开发调试用，板端不编译）
│
├── cloud/                       # ── 项目 A：云端精判服务（Python，运行于 VM/主机）──
│   ├── server.py                #   FastAPI + YOLOv8n（人员/类型检测）+ face_recognition（白名单比对）
│   ├── requirements.txt         #   依赖清单（fastapi/uvicorn/ultralytics/numpy/face_recognition）
│   ├── whitelist/               #   白名单照片目录（<姓名>.jpg，自动加载/变化自动重载）
│   ├── yolov8n.pt               #   YOLO 权重（国内镜像预下载，服务启动免外网）
│   └── README.md                #   服务部署/协议/白名单管理文档
│
├── models/                      # 板端推理模型（make send 部署到板 /root/）
│   ├── face.param               #   SCRFD-500M-opt2 ncnn 模型（ncnn-assets 官方转换）
│   └── face.bin
│
├── dts/                         # 设备树（imx6ull-southbay-emmc.dts：CSI/74HC595/触摸/网口拓扑）
├── protocol/                    # [P6-2] SUMP 逻辑分析仪工具链（edt_capture 后端 + TCP:9527 服务器）
├── benchmark/                   # 基准方法学（结果表待实测回填）
├── docs/                        # 设计文档（驱动架构决策/SUMP 设计/内核补丁档案/LVGL 网关设计书）
├── third_party/                 # ncnn 源码 + ncnn-armhf 交叉编译产物 + ncnn-assets 模型等
└── build/                       # 编译产物（git 忽略）：module/ test/ protocol/ dts/ lvgl/
```

---

## 二、应用架构（项目 A）

```
[传感器] KEY0/AP3216C/ICM20608 ──IIO/input──┐
[摄像头] OV5640 CSI ──V4L2 640x480 YUYV──┐  │
                                         ▼  ▼
                                   ┌───────────────┐      ┌─────────────────────┐
                                   │  dev_bridge   │◄────►│  UI（LVGL 1024x600） │
                                   │ 采集+节流+分发 │      │  布撤防/告警/时间轴  │
                                   └──────┬────────┘      │  设置/相册/抓拍      │
                                          │ 粗判命中(冷却节流)    └─────────────────────┘
                                          ▼
                              ┌──────────────────────────┐
                              │ detector 工作线程（两级）│
                              │ 段1 SCRFD 人脸初判(本地) │────► ncnn（/root/face.*）
                              │ 段2 云端复核（可开关）    │────► cloud/server.py（VM:8000）
                              └──────────┬───────────────┘
                                         ▼ 结论（初步/最终 × 四类）
                              ┌──────────────────────────┐
                              │ dev_bridge 结论分发      │────► 告警弹窗+分级声光+自动消警
                              │ （布防/告警状态门槛）     │────► 时间轴 / MQTT 上报 / 自动抓拍
                              └──────────────────────────┘      └► storage_mgr（TF 卡 BMP）
```

**入侵检测两级管线**（08-30 定案）：

1. **帧差粗判**（cam_feed）：每 N 帧对比一次灰度差（间隔 1~30 帧、差值 5~100，设置页可调）→ 命中且布防中且过冷却（默认 60s）→ 抓最新帧提交精判。
2. **段1 · 本地初判**（SCRFD 人脸检测，ncnn @160 输入，~400-800ms）：检出人脸 → **立即 STRANGER 完整告警**（不等云端）；未检出 → toast"画面变动"轻提醒，等云端定论。
3. **段2 · 云端复核**（设置"云端复核"开才做；同一帧 RGB565 428KB 上传）：云端 YOLOv8 全类别检测 + 白名单比对 → 最终结论定案：

| 云端结论 | 设备行为 |
|---|---|
| 白名单命中（已授权） | 在告警 → **自动消警**（声光停+弹窗关+时间轴"告警解除"） |
| 陌生人（有人未录入） | 不在告警 → 升级 STRANGER 告警（兜住本地漏检的背身人） |
| 动物/物体/无人 | 在告警 → 自动消警（本地误报被压掉）；不在告警 → 时间轴记录类型 |
| 云端不可达 | 初判未检出人 → INTRUDER 告警（**不漏报**）；已告警 → 维持本地结论 |

> 设计核心：**本地管快**（~1s 初判响应）、**云端管准**（人形+类型+白名单纠偏）、**断网管稳**（本地结论兜底）。设置页"云端复核"开关关闭 = 纯本地语义（未检出人脸即 INTRUDER）。

---

## 三、构建与部署

### 3.1 环境（Ubuntu VM，路径按实际调整）

```bash
# 内核源码树（驱动编译 KDIR）
/home/szh/linux/kernel/linux-imx-rel_imx_4.1.15_2.1.0_ga_southbay
# NFS rootfs / TFTP
/home/szh/linux/nfs/rootfs    /home/szh/linux/tftp
# 交叉编译链
arm-linux-gnueabihf-          （4.9.4，含 g++；ncnn 依赖）
```

### 3.2 一键构建

```bash
make              # 全编：core 驱动 + dts + 测试程序 + protocol + lvgl 网关应用
make core         # 只编译驱动模块
make dts          # 只编译设备树
make protocol     # SUMP 工具链（交叉版）；make host 为 PC 自测版
make clean        # 清理 build/

make send         # 部署到开发板（需 sudo 密码）：
                  #   驱动 .ko + 测试程序 + sump → NFS /lib/modules/4.1.15/
                  #   dtb → TFTP 目录
                  #   lvgl_gateway → NFS /usr/bin/（板端 rcS 自启）
                  #   models/face.param|bin → NFS /root/（SCRFD 精判模型）
```

> 注意：Makefile 的 .o 规则只跟踪 lv_conf.h，**修改 .h 头文件后需 `rm build/lvgl/src/<模块>.o` 或 touch 对应 .c 再 make**。

### 3.3 内核配置要点（重编内核不得丢失）

`VIDEO_MXC_CSI_CAMERA`（6ULL CSI 主机 mx6s_capture；**VIDEO_MXC_CAPTURE 是 IPU 栈对 6ULL 无效**）、`MXC_CAMERA_OV5640`、`SPI_GPIO + GPIO_74X164`（74HC595，ov5640 pwdn/rst 提供者）、IIO。详见 `docs/kernel-patches-20260829.md`（含 mx6s_capture FCC、ov5640 PLL 30fps 补丁）。

### 3.4 云端精判服务部署（VM）

```bash
python3 -m venv ~/cloud-venv && source ~/cloud-venv/bin/activate
# Python 3.12 注意：face_recognition 依赖 pkg_resources，需 setuptools<81
pip install "setuptools<81" -i https://pypi.tuna.tsinghua.edu.cn/simple
pip install -r cloud/requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple

# 白名单：把授权人员露脸照放入 cloud/whitelist/<姓名>.jpg（文件名=姓名）
cd cloud
uvicorn server:app --host 0.0.0.0 --port 8000
# 验证：wget -qO- http://127.0.0.1:8000/whitelist   → {"names":["SZH"]}
```

- 白名单 HTTP 接口：`GET /whitelist`（查看）、`POST /whitelist/add?name=姓名`（body=照片）
- 白名单目录有增删改**自动重载**，无需重启服务
- 比对容差 `FACE_TOLERANCE=0.6`（server.py 可调，越小越严）
- 板端服务器地址：`core/lvgl/src/cloud_detect.c` 的 `CLOUD_DETECT_SERVER`（默认 `192.168.3.26:8000`，改动需重编）
- 协议与故障处理详见 `cloud/README.md`

---

## 四、使用指南（板端 UI）

**上电即自启**（rcS → /usr/bin/lvgl_gateway）。主界面布局：顶栏（时间/联网/布防状态）| 左侧摄像头预览（630x340，30fps）+ 1x4 分区卡片 | 右侧事件时间轴 | 底栏大按钮。

### 4.1 日常操作

| 操作 | 方式 |
|---|---|
| 一键布防/撤防 | 底栏大按钮；分区卡片绿光呼吸=布防中 |
| 分区独立布防 | 点分区卡片 → 详情弹窗 → 状态按钮 |
| 告警确认（消警） | 弹窗上**长按 1 秒**（进度条防误触）；MUTE=临时静音 |
| 切换预览分区 | 顶部 4 钮（单摄分时切换，运动事件归属当前预览分区） |
| 手动抓拍 | 预览区右下"抓拍"→ 存 TF 卡（BMP）→ 相册可回看 |
| 事件时间轴 | 右侧列表（类型着色），头部"清空"一键清空 |
| 息屏与唤醒 | 10 分钟无操作自动息屏；**触摸或 KEY0 唤醒** |
| 背光亮度 | 设置弹窗滑条（联动 pwm-backlight） |

### 4.2 设置弹窗

- **识别设置**：`云端复核` 开关（开=两级管线，默认；关=纯本地：未检出人脸即 INTRUDER）
- **检测设置**：帧差间隔（1~30 帧）/ 灰度差阈值（5~100）——粗判灵敏度
- **预警设置**：四位置 × 触发源 × 数据通道 × 阈值 × 比较方式 × 冷却时间（传感器预警）
- **定时布防**：时间段自动布防；**音量/网络状态/关于** 等

### 4.3 告警分级（设计 2.2）

高（窗/周界）=LED 快闪+长鸣；中（仓库）=中闪+断鸣；低（门）=慢闪+短鸣。消警后自动解除静音。

### 4.4 验收场景（云端复核开 + 布防）

| # | 操作 | 预期 | 串口标志 |
|---|---|---|---|
| 1 | 白名单内的人露脸挥手 | STRANGER 立即告警 → 1~2s 后**自动消警** | `faces=1` → `cloud=known (person)` |
| 2 | 未录入的人露脸挥手 | 告警维持 | `cloud=stranger (person)` |
| 3 | 画面无人时开关灯触发 | 不告警，时间轴"云端复核：判定无人员" | `faces=0` → `cloud=none` |
| 4 | `ifconfig eth0 down` 断网触发 | 无人脸→INTRUDER；有人脸→维持 | `cloud=unreachable` |
| 5 | 关"云端复核"再挥手 | 纯本地：检出脸 STRANGER / 无人脸 INTRUDER | 无 cloud 行 |
| 6 | 宠物/移动物入镜 | 不告警，时间轴"判定为动物活动" | `cloud=none` |

---

## 五、故障排查

| 现象 | 处理 |
|---|---|
| 开机黑屏（背光亮、串口 fps 行在打印） | App 启动已自动 unblank 自愈；仍黑则重插屏幕排线两端，`dmesg \| grep -iE "mxsfb\|lcd"` |
| 串口刷 `mxs wait for vsync timeout` | fb 处于 blank（息屏）时正常现象，触摸唤醒即恢复；持续刷屏则查显示路径 |
| 无 `/dev/video0` | 内核缺 `VIDEO_MXC_CSI_CAMERA`（见 3.3，勿用 VIDEO_MXC_CAPTURE） |
| `[DET] load /root/face.param failed` | 模型未部署：`make send` 或手动拷 models/face.* 到板 /root/；缺失时精判回退"有运动即告警"语义 |
| 云端复核全部 fail-safe 告警 | VM 服务没起/防火墙：`ss -tlnp \| grep 8000`、板端 `ping 192.168.3.26` |
| 白名单不生效 | `wget -qO- http://127.0.0.1:8000/whitelist` 确认已加载；照片需露脸正面照 |
| 抓拍丢失 | TF 卡未挂/满卡自动清理（剩余 <50MB 清最旧）；相册重启自动回填 |
| 触摸无响应 | `dmesg \| grep gt9147` 确认加载与中断；App 按驱动名自动探测 eventX |

---

## 六、项目 B 进度（驱动工程，全部验收通过）

- [x] P1 LED 字符设备（验收 4/4） · P2 按键中断三种读模式（5/5） · P3 gpio_event 企业级驱动（7/7 + 内核 ftrace）
- [x] P4 PWM/I2C/SPI 子系统驱动 beep_pwm/ap3216c/icm20608（8/8）
- [x] P6-1 input 按键驱动（PASS=10） · P6-2/P6-3 SUMP 工具链 + 板端验收（PASS=7，1M 样本零丢失）
- [x] P7-1 GT9147 触摸（中断模式 + 配置自愈） · P5 OV5640 采用内核自带驱动接入
- 驱动使用与验收脚本见各模块 `test/` 与 `docs/`；SUMP 指南见 `protocol/README.md`

## 七、文档索引

| 文档 | 内容 |
|---|---|
| `项目进度.md`（仓库上级目录） | 总账本：进度/验收/决策(ADR)/问题记录（**跨会话必读**） |
| `cloud/README.md` | 云端服务部署、协议表、白名单管理 |
| `docs/kernel-patches-20260829.md` | 内核补丁档案（CSI/ov5640/配置清单） |
| `docs/superpowers/specs/` | LVGL 智能安防网关设计书 |
| `docs/driver-architecture-decision.md` | 驱动架构决策（gpio_event vs input 等） |
| `protocol/README.md` / `benchmark/README.md` | SUMP 使用指南 / 基准方法学 |
