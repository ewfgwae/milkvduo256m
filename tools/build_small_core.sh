#!/bin/bash
# 编译小核 C906L FreeRTOS 固件 cvirtos.bin
# 用法: tools/build_small_core.sh [SDK路径]     默认 ~/duo-sdk-v2
#
# 说明:
#  - duo-sdk-v2 首次编译需要先按官方文档配置好 host-tools 交叉编译器。
#  - 若 build 树里有 root 属主文件(以前用 sudo/root 编过), 普通用户会报
#      "ninja: Error writing to build log: Permission denied"
#    直接用 root 跑本脚本即可。
set -euo pipefail

SDK="${1:-$HOME/duo-sdk-v2}"
[ -d "$SDK" ] || { echo "找不到 SDK: $SDK" >&2; exit 1; }

# riscv64 工具链(注意: 目录名是 riscv64-elf-x86_64, 实际前缀是 riscv64-unknown-elf-)
export PATH="$SDK/host-tools/gcc/riscv64-elf-x86_64/bin:$PATH"

cd "$SDK"
export TOP="$SDK"   # envsetup 的 gettop() 直接引用 $TOP, 没有会在 set -u 下炸
# SDK 的 envsetup 链路里有"故意的"可失败命令(缺 isp_tuning 目录时 gen_sensor_config.py
# 打印警告后非零退出), 且内部引用未定义变量 —— source 期间 -e/-u 都得关掉,
# 否则会在中途静默死掉。返回码只作参考(单独跑也可能非零), 关键看后面 make rtos。
set +eu
# shellcheck disable=SC1091
source build/envsetup_milkv.sh milkv-duo256m-glibc-arm64-sd
set -eu
export PATH="$SDK/host-tools/gcc/riscv64-elf-x86_64/bin:$PATH"

cd "$SDK/build"
make rtos

BIN="$SDK/freertos/cvitek/install/bin/cvirtos.bin"
echo
echo "=== 产物 ==="
ls -l "$BIN"
echo "md5: $(md5sum "$BIN" | awk '{print $1}')"
echo "build id 探针(固件内 g_lvgl_probe[14])可用 nm 查:"
echo "  $SDK/host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-nm \\"
echo "      $SDK/freertos/cvitek/install/bin/cvirtos.elf | grep -E 'g_lvgl_probe|g_dma2_probe|g_spi2_probe'"
