#!/bin/bash
# ==============================================================================
# VIO Data Logger 环境部署脚本 (A733 专用优化版)
# ==============================================================================
DV89_VID="1bcf" 
DV89_PID="d108"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}====================================================${NC}"
echo -e "${GREEN}  VIO Data Logger - 板端环境一键部署脚本 (A733)  ${NC}"
echo -e "${GREEN}====================================================${NC}"

if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}[错误] 请使用 sudo 运行此脚本！${NC}"
  exit 1
fi

# ==============================================================================
# 第一部分：安装板端原生编译所需的所有 APT 依赖 
# ==============================================================================
echo -e "\n${YELLOW}>>> [1/5] 正在检查系统依赖库与编译环境...${NC}"
# 包含了基础编译工具、USB/V4L调试工具、FFmpeg库、GLEW库
REQUIRED_PACKAGES=("build-essential" "cmake" "pkg-config" "git" "wget" "curl" "rsync" \
                   "usbutils" "v4l-utils" "libusb-1.0-0-dev" \
                   "libavcodec-dev" "libavformat-dev" "libavutil-dev" "libswscale-dev" "libglew-dev")

apt-get update
for pkg in "${REQUIRED_PACKAGES[@]}"; do
    if ! dpkg -s "$pkg" >/dev/null 2>&1; then
        echo "正在安装缺失的依赖: $pkg"
        apt-get install -y "$pkg"
    fi
done

# ==============================================================================
# 第二部分：动态生成摄像头 UDEV 规则 (完美兼容 OAK-D 二次枚举跳变)
# ==============================================================================
echo -e "\n${YELLOW}>>> [2/5] 正在注入摄像头 UDEV 规则...${NC}"
cat << EOF > /etc/udev/rules.d/99-usb-cameras.rules
# DV89 辅助摄像头固定节点与赋权
SUBSYSTEM=="video4linux", ATTRS{idVendor}=="$DV89_VID", ATTRS{idProduct}=="$DV89_PID", ATTR{index}=="0", SYMLINK+="dv89_cam", MODE="0666"
# OAK-D (Carina) 基础赋权
SUBSYSTEM=="usb", ATTRS{idVendor}=="0906", MODE="0666"
SUBSYSTEM=="usb", ATTRS{idVendor}=="03e7", MODE="0666"
EOF

# ==============================================================================
# 第三部分：集成 SD 卡热插拔挂载逻辑
# ==============================================================================
echo -e "\n${YELLOW}>>> [3/5] 正在集成 SD 卡热插拔挂载逻辑...${NC}"

# 创建挂载脚本（强制 ext4）
cat << 'EOF' > /usr/local/bin/automount_sdcard.sh
#!/bin/bash
DEVICE=$1
ACTION=$2
MOUNT_POINT="/sdcard"

if [ "$ACTION" == "add" ]; then
    # 等待设备节点就绪
    while [ ! -b "/dev/$DEVICE" ]; do sleep 0.2; done
    # 检查是否已经挂载
    if mountpoint -q $MOUNT_POINT; then
        exit 0
    fi
    mkdir -p $MOUNT_POINT
    # 强制以 ext4 格式挂载，如果不是 ext4 则失败
    mount -t ext4 -o noatime,flush,data=writeback /dev/$DEVICE $MOUNT_POINT 2>/dev/null
    if [ $? -eq 0 ]; then
        chmod 777 $MOUNT_POINT
        echo "SD card (ext4) mounted to $MOUNT_POINT" | logger -t automount
    else
        echo "Failed to mount /dev/$DEVICE: not ext4 or superblock error" | logger -t automount
        rmdir $MOUNT_POINT 2>/dev/null
    fi
elif [ "$ACTION" == "remove" ]; then
    umount -l $MOUNT_POINT 2>/dev/null
    rmdir $MOUNT_POINT 2>/dev/null
fi
EOF

chmod +x /usr/local/bin/automount_sdcard.sh

# 创建 udev 规则：匹配可移动 mmcblk 设备的第一个分区
cat << 'EOF' > /etc/udev/rules.d/99-sdcard-fixed.rules
# 匹配可移动 SD 卡（removable=1）的第一个分区
ACTION=="add", SUBSYSTEM=="block", KERNEL=="mmcblk[0-9]p1", \
    ATTRS{removable}=="1", \
    RUN+="/usr/local/bin/automount_sdcard.sh $kernel add"

ACTION=="remove", SUBSYSTEM=="block", KERNEL=="mmcblk[0-9]p1", \
    RUN+="/usr/local/bin/automount_sdcard.sh $kernel remove"
EOF

# 如果当前已有 SD 卡插入，立即手动触发一次挂载（避免重启）
for dev in $(ls /dev/mmcblk?p1 2>/dev/null); do
    devname=$(basename $dev)
    # 检查其父设备的 removable 属性
    syspath="/sys/block/${devname%p1}/device/../removable"
    if [ -f "$syspath" ] && [ "$(cat $syspath)" = "1" ]; then
        /usr/local/bin/automount_sdcard.sh $devname add
    fi
done

# ==============================================================================
# 第四部分：性能调优与拉取无锁队列
# ==============================================================================
echo -e "\n${YELLOW}>>> [4/5] 正在优化系统性能...${NC}"
for cpu_gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -f "$cpu_gov" ] && echo "performance" > "$cpu_gov"
done

# ==============================================================================
# 第五部分：激活与验证
# ==============================================================================
echo -e "\n${YELLOW}>>> [5/5] 正在激活 UDEV 配置...${NC}"
udevadm control --reload-rules
udevadm trigger
sleep 2

if mountpoint -q /sdcard; then
    echo -e "${GREEN}[成功] SD 卡已自动挂载至 /sdcard${NC}"
else
    echo -e "${YELLOW}[提示] /sdcard 当前未挂载，请插入卡后自动测试。${NC}"
fi

echo -e "\n${GREEN}====================================================${NC}"
echo -e "${GREEN}  板端环境部署完毕！直接运行 ./scripts/build.sh 即可编译！ ${NC}"
echo -e "${GREEN}====================================================${NC}"