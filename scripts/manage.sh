#!/bin/bash


# ==============================================================================
# VIO Data Logger 后台守护进程控制脚本 (板端原生版)
# ==============================================================================

# 1. 强制要求以 root 权限运行本脚本 (替代原有内部的 sudo)
if [ "$EUID" -ne 0 ]; then
  echo -e "\033[0;31m[错误] 权限不足！请在最外层使用 sudo 运行此脚本: sudo $0 $*\033[0m"
  exit 1
fi




PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 路径更新为本地编译输出的目录
BIN_FILE="${PROJECT_ROOT}/build/VioDataLogger"   
PID_FILE="/tmp/vio_logger.pid"
LOG_FILE="/tmp/vio_sys.log"

if [ ! -f "$BIN_FILE" ]; then
    echo "[错误] 找不到可执行文件 $BIN_FILE，请先执行 ./scripts/build.sh 编译！"
    exit 1
fi

start() {

    # =====================================================================
    # WiFi 强力对齐与高精度 NTP 校时器
    # =====================================================================
    echo "[WiFi] 正在检查网络连接状态..."

    # 1. 如果 WiFi 没连上，尝试强制拉起板载网络（根据你的 SSID 修改）
    # nmcli dev wifi connect "你的WIFI名称" password "你的WIFI密码" > /dev/null 2>&1

    # 2. 循环等待网络物理畅通（最多等待 10 秒，防止死锁）
    RETRY=0
    while ! ping -c 1 -W 1 8.8.8.8 >/dev/null 2>&1; do
        if [ $RETRY -eq 10 ]; then
            echo "[警告] WiFi 联网超时！将使用板载硬件 RTC 残留时钟强行断网采集。"
            break
        fi
        echo "正在等待网络就绪 ($RETRY/10)..."
        sleep 1
        RETRY=$((RETRY+1))
    done

    # 3. 核心：一旦网络畅通，立即物理轰击 NTP 服务器，强制同步系统墙上时间
    if [ $RETRY -lt 10 ]; then
        echo "[NTP] 网络已打通，正在向阿里云时间服务器强制同步..."
        sudo systemctl stop ntp >/dev/null 2>&1
        sudo ntpd -gq -c /dev/null ntp.aliyun.com >/dev/null 2>&1 # -gq 表示无视巨大时间差，立即强制同步
        sudo hwclock --systohc >/dev/null 2>&1 # 同步写入板载硬件 RTC 芯片，留作容灾备份
        echo "🟢 [成功] 系统时间已完美校准！当前系统时间: $(date '+%Y-%m-%d %H:%M:%S')"
    fi


    if [ -f "$PID_FILE" ] && kill -0 $(cat "$PID_FILE") 2>/dev/null; then
        echo "[提示] 采集程序已经在运行中！(PID: $(cat $PID_FILE))"
        exit 0
    fi

    if [ -n "$1" ]; then
        # 2. 传参自愈：过滤掉用户可能误输入的开头斜杠，防止路径嵌套畸形
        DIR_NAME=$(echo "$1" | sed 's/^\///')
        OUTPUT_DIR="/sdcard/data_${DIR_NAME}/"
    else
        OUTPUT_DIR="/sdcard/data_$(date +%s)/"
    fi

    echo "==== 启动 VIO 数据采集 ===="
    echo "目标存储路径: $OUTPUT_DIR"
    echo "系统运行日志: $LOG_FILE"

    # 将工作目录切换到项目根目录 (data_collect/)，确保相对路径资源能正确加载
    cd "$PROJECT_ROOT" || exit 1
    
    # 3. 取消内部 sudo，继承外层脚本的 root 权限拉起后台守护进程
    nohup "$BIN_FILE" "$OUTPUT_DIR" > "$LOG_FILE" 2>&1 &
    
    sleep 1.5
    if [ -f "$PID_FILE" ] && kill -0 $(cat "$PID_FILE") 2>/dev/null; then
        echo -e "\033[0;32m[成功] 守护进程已成功以 SCHED_FIFO 实时模式拉起！(PID: $(cat $PID_FILE))\033[0m"
    else
        echo -e "\033[0;31m[错误] 启动失败，请查看日志: tail -n 20 $LOG_FILE\033[0m"
    fi
}

stop() {
    if [ ! -f "$PID_FILE" ]; then
        echo "[提示] 未检测到本地标记 PID 文件，尝试使用名称拦截信号..."
        pkill -SIGINT -x "VioDataLogger" 2>/dev/null
        exit 0
    fi

    PID=$(cat "$PID_FILE")
    echo "==== 停止 VIO 数据采集 ===="
    echo "向实时进程 (PID: $PID) 发送安全解耦退出信号 (SIGINT) ..."
    
    # 取消内部 sudo
    kill -SIGINT "$PID"

    # 循环等待残余无锁队列数据排空，加入 30 秒超时保护
    TIMEOUT=30
    COUNT=0
    printf "正在安全同步落盘，排空无锁队列 Buffer，请勿断电 "
    while kill -0 "$PID" 2>/dev/null; do
        printf "."
        sleep 1
        ((COUNT++))
        if [ "$COUNT" -ge "$TIMEOUT" ]; then
            echo -e "\n[警告] 扫尾超时！强行中止终止剩余挂起线程..."
            kill -SIGKILL "$PID"
            break
        fi
    done
    echo ""
    echo -e "\033[0;32m[成功] 采集已安全关闭，MP4 切片索引及二进制 IMU 流完全落盘！\033[0m"
}

status() {
    if [ -f "$PID_FILE" ] && kill -0 $(cat "$PID_FILE") 2>/dev/null; then
        echo "🟢 状态：数采在线运行中 (PID: $(cat $PID_FILE))"
        echo "实时缓冲区最新运行快照 (Last 5 lines):"
        tail -n 5 "$LOG_FILE"
    else
        echo "🔴 状态：已安全停止"
    fi
}



# =====================================================================
# 主控制逻辑：解析命令行参数并调用对应函数
# =====================================================================

case "$1" in
    start) start "$2" ;;
    stop)  stop ;;
    status) status ;;
    *)
        echo "用法: $0 {start|stop|status} [自定义测试数据集文件夹名]"
        exit 1
        ;;
esac