#!/bin/bash
# 编译大核 aarch64 Linux 程序 lcd_sender
# 用法: tools/build_big_core.sh [SDK路径] [输出文件]
#       默认 SDK=~/duo-sdk-v2   默认输出=<仓库>/build/lcd_sender
set -euo pipefail

SDK="${1:-$HOME/duo-sdk-v2}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${2:-$HERE/build/lcd_sender}"

GCC="$SDK/host-tools/gcc/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc"
[ -x "$GCC" ] || { echo "找不到 aarch64 交叉编译器: $GCC" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
# -static: 板上 buildroot 根文件系统不一定有对应动态库, 静态链接最省事
"$GCC" -O2 -Wall -static -o "$OUT" "$HERE/src/big_core/lcd_sender.c"

echo "=== 产物 ==="
ls -l "$OUT"
file "$OUT" 2>/dev/null || true
