#!/bin/sh
# P6_accept_sump.sh —— P6-2 SUMP 工具链验收（手册 5.15.2）
# 覆盖验收清单: ①协议自测(sim 全链路) ②采集源就绪 ③sump_server 启动+监听 ④PulseView 连接引导
# 用法: ./P6_accept_sump.sh
# 要求: 与 sump_server/sump_selftest 同目录；板端模式需要 gpio_event_capture 已加载
# 说明: ④ PulseView 三项验收（8通道波形/I2C解码/1M样本）需人工在 PC 端执行

PASS=0; FAIL=0
PORT=9527
MOD_DIR=/lib/modules/4.1.15
DEV=/dev/edt_capture0

echo "===== P6-2 SUMP 工具链验收脚本 ====="
echo ""

# ---------- ① 前置：可执行文件 ----------
echo "[①] 前置检查: sump_server / sump_selftest"
if [ -x ./sump_server ]; then
    echo "  [PASS] ./sump_server 存在"
    PASS=$((PASS+1))
else
    echo "  [FAIL] 找不到 ./sump_server（请先 make protocol 并在同目录运行）"
    FAIL=$((FAIL+1))
fi
if [ -x ./sump_selftest ]; then
    echo "  [PASS] ./sump_selftest 存在"
    PASS=$((PASS+1))
else
    echo "  [FAIL] 找不到 ./sump_selftest"
    FAIL=$((FAIL+1))
fi
if [ $FAIL -gt 0 ]; then
    echo ""
    echo "结果: FAIL=$FAIL（缺少可执行文件，先编译部署）"
    exit 1
fi

# ---------- ② 协议自测（sim 模式，无需硬件） ----------
echo ""
echo "[②] 协议自测（sim 合成源，模拟 sigrok 完整握手 + 1M 样本采集）"
killall -9 sump_server 2>/dev/null   # 防御：SIGKILL 清理残留实例（SIGTERM 对 accept 阻塞中的旧版无效）
./sump_server --sim --port $PORT --max-window 3000 > /tmp/sump_sim.log 2>&1 &
SERVER_PID=$!
sleep 1
# 检测服务器是否存活（bind 失败/端口冲突会立即退出）
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "  [FAIL] sump_server(sim) 启动失败，日志如下:"
    cat /tmp/sump_sim.log
    FAIL=$((FAIL+1))
else
    ./sump_selftest --host 127.0.0.1 --port $PORT --samples 1048576
    RC=$?
    kill $SERVER_PID 2>/dev/null
    wait $SERVER_PID 2>/dev/null
    if [ $RC -eq 0 ]; then
        echo "  [PASS] 协议自测全部通过（握手/metadata/采集/波形频率）"
        PASS=$((PASS+1))
    else
        echo "  [FAIL] 协议自测失败（见上方输出）"
        FAIL=$((FAIL+1))
    fi
fi

# ---------- ③ 采集源就绪（板端模式） ----------
echo ""
echo "[③] 采集源检查: gpio_event_capture 驱动 + 设备节点"
if lsmod | grep -q gpio_event_capture; then
    echo "  [PASS] gpio_event_capture 已加载"
    PASS=$((PASS+1))
else
    echo "  [FAIL] gpio_event_capture 未加载，先: insmod $MOD_DIR/gpio_event_capture.ko"
    FAIL=$((FAIL+1))
fi
if [ -e "$DEV" ]; then
    echo "  [PASS] 设备节点 $DEV 存在"
    PASS=$((PASS+1))
else
    echo "  [FAIL] $DEV 不存在（驱动加载后应出现；检查 dts gpio-event-capture 节点状态）"
    FAIL=$((FAIL+1))
fi

# ---------- ④ sump_server 板端模式启动 + TCP 监听 ----------
echo ""
echo "[④] sump_server 板端模式启动 + TCP 监听确认"
if [ -e "$DEV" ]; then
    killall -9 sump_server 2>/dev/null   # 防御：清理 ② 可能残留的实例，避免端口冲突
    ./sump_server --device $DEV --port $PORT --max-window 5000 > /tmp/sump_dev.log 2>&1 &
    SERVER_PID=$!
    sleep 1
    if ! kill -0 $SERVER_PID 2>/dev/null; then
        echo "  [FAIL] sump_server 启动失败，日志如下:"
        cat /tmp/sump_dev.log
        FAIL=$((FAIL+1))
    else
        # 端口检测：优先 ss → netstat → /proc/net/tcp（busybox 兜底）
        PORT_HEX=$(printf "%04X" $PORT)
        LISTEN=0
        if command -v ss >/dev/null 2>&1; then
            ss -tln 2>/dev/null | grep -q ":$PORT " && LISTEN=1
        fi
        if [ "$LISTEN" = "0" ] && command -v netstat >/dev/null 2>&1; then
            netstat -tln 2>/dev/null | grep -q ":$PORT " && LISTEN=1
        fi
        if [ "$LISTEN" = "0" ]; then
            grep -q ":${PORT_HEX} " /proc/net/tcp 2>/dev/null && LISTEN=1
            grep -q ":${PORT_HEX} " /proc/net/tcp6 2>/dev/null && LISTEN=1
        fi
        if [ "$LISTEN" = "1" ]; then
            echo "  [PASS] TCP :$PORT 监听中（sump_server 板端模式就绪）"
            PASS=$((PASS+1))
        else
            echo "  [FAIL] TCP :$PORT 未监听（查看 sump_server 启动输出）"
            FAIL=$((FAIL+1))
        fi
        echo "  -> 已自动停止 (PID=$SERVER_PID)；PulseView 验收请另开终端手动启动:"
        echo "     ./sump_server --device $DEV --port $PORT --verbose"
        sleep 1
        kill $SERVER_PID 2>/dev/null
        wait $SERVER_PID 2>/dev/null
        echo "  [PASS] sump_server 可正常启动/退出"
        PASS=$((PASS+1))
    fi
else
    echo "  [SKIP] 设备节点缺失，跳过板端模式（③ 已 FAIL）"
fi

# ---------- ⑤ PulseView 连接引导 ----------
echo ""
echo "[⑤] PulseView 人工验收引导（PC 端）"
echo "  ------------------------------------------------"
echo "  0. 前置：按需采集 dts 已部署（gpio-event-capture=okay，4 通道；"
echo "     触摸屏/LED/PWM 等外设功能全部保留，无需卸载任何模块）"
echo "     insmod /lib/modules/4.1.15/gpio_event_capture.ko"
echo "     ls /dev/edt_capture0"
echo "  1. 启动 sump_server: ./sump_server --device $DEV --port $PORT"
echo "  2. PC 端 PulseView: File → Connect to Device... → 类型 TCP"
echo "     → 地址 tcp://<板端IP>:$PORT"
echo "  3. 验收 a: 8 通道波形显示（通道0=KEY0 按键、通道1=JP6 pin17 接信号、"
echo "     通道2-3 接下方 I2C 信号；通道4-7 未配置=恒低平线）"
echo "  4. 验收 b: AP3216C I2C 解码——飞线: AP3216C SDA→通道2(GPIO1_IO00),"
echo "     SCL→通道3(GPIO1_IO02)，板端跑 ap3216c_test & 持续读数，"
echo "     PulseView 通道2/3 上应解码出 I2C 读写帧"
echo "  5. 验收 c: 采样率≤2MHz + 样本数 1M 采集，验证无丢包"
echo "     （板端 GET_STATS: queued+dropped == irq+snapshot 守恒）"
echo "  ------------------------------------------------"
echo "  【采集通道接线表（按需采集，4 通道）】"
echo "    ch0 GPIO1_IO18 KEY0(板载)   ch1 GPIO1_IO01 JP6 pin17"
echo "    ch2 GPIO1_IO00 JP6 GPIO_00  ch3 GPIO1_IO02 JP6 GPIO_02"
echo "  【说明】PulseView 显示 8 通道，通道4-7 未配置为恒低平线；"
echo "     触摸屏(IO09)/LED(IO03)/PWM(IO04/08) 引脚不再用作采集通道，功能正常"
echo "  ------------------------------------------------"

echo ""
echo "===== P6-2 验收汇总: PASS=$PASS FAIL=$FAIL ====="
echo "（①②③④ 为自动化项；⑤ PulseView 三项需人工在 PC 端确认后回报）"
[ $FAIL -eq 0 ] || echo "!!! 存在失败项，见上方输出 !!!"
exit $FAIL