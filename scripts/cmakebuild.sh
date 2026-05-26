#!/bin/bash
# ==============================================================================
# 脚本名称: cmakebuild.sh (板端原生编译版)
# 作用: 在 A733 开发板上直接进行多核原生编译
# 用法: 
#   ./cmakebuild.sh         # 普通增量编译
#   ./cmakebuild.sh clear   # 彻底清空缓存并重新全量编译
# ==============================================================================

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==== 开始 A733 板端原生编译 ===="
cd "${PROJECT_ROOT}"

# 检测是否传入了 clear 清空参数
if [[ "$1" == "clear" || "$1" == "-c" || "$1" == "--clear" ]]; then
    echo -e "\033[1;33m[清理] 检测到 clear 参数，正在抹除旧的构建缓存...\033[0m"
    rm -rf build
    echo -e "\033[0;32m[清理] 原 build 目录已彻底粉碎！\033[0m"
fi

# 使用全新的 build 目录
mkdir -p build
cd build

# 纯净的 CMake 配置，开启 Release 极致性能优化
cmake -DCMAKE_BUILD_TYPE=Release ..

# 火力全开：调用 A733 全部核心并行编译
make -j$(nproc)

echo "==== 编译完成 ===="
echo "可执行文件位于: ${PROJECT_ROOT}/build/VioDataLogger"
file VioDataLogger