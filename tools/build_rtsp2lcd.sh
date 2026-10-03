#!/bin/bash
# 编译大核工具 rtsp2lcd —— RTSP 拉流 -> 硬件 H.264 解码(CVI_VDEC) -> RGB565 -> 共享内存。
#
# ★ 必须在 Linux/WSL 里跑(要用 aarch64 交叉编译器; 板上没有编译器)。
#
# 头文件用本仓库 duo-tdl-examples/include/system(与 CVITEK SDK 的 cvi_mpi/include
# **逐字节一致**, 已核对 cvi_vdec.h/cvi_comm_vdec.h/cvi_sys.h/cvi_vb.h/cvi_comm_vb.h/
# cvi_comm_video.h/cvi_buffer.h);
# 库用**板上原版**($HOME/boardlibs == /mnt/system/usr/lib 的副本) —— 头与库都对着
# 同一套 ABI, 不会再出现 VI/VPSS 那种 ILLEGAL_PARAM。
#
# ★ 本仓库路径里带空格, 直接 -I 会被拆坏, 所以先拷到无空格目录 $BUILD 再编。
#
# 用法: tools/build_rtsp2lcd.sh [SDK路径] [板上库目录]
#       默认 SDK=$HOME/duo-sdk-v2  板上库=$HOME/boardlibs  工作目录=$HOME/rtspbuild
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${1:-$HOME/duo-sdk-v2}"
BOARDLIBS="${2:-$HOME/boardlibs}"
BUILD="${BUILD:-$HOME/rtspbuild}"

GCC="$SDK/host-tools/gcc/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc"

[ -x "$GCC" ]       || { echo "找不到交叉编译器: $GCC" >&2; exit 1; }
[ -f "$HERE/tools/rtsp2lcd.c" ] || { echo "找不到源码" >&2; exit 1; }
[ -d "$HERE/duo-tdl-examples/include/system" ] || { echo "找不到头文件目录" >&2; exit 1; }
[ -d "$BOARDLIBS" ] || { echo "找不到板上库目录: $BOARDLIBS" >&2; exit 1; }
for so in libvdec.so libsys.so; do
	[ -f "$BOARDLIBS/$so" ] || { echo "$BOARDLIBS 里没有 $so" >&2; exit 1; }
done

echo "--- 1) 拷到无空格目录 $BUILD ---"
mkdir -p "$BUILD"
cp -f "$HERE/tools/rtsp2lcd.c"         "$BUILD/rtsp2lcd.c"
cp -f "$HERE/src/big_core/lcd_shm.h"   "$BUILD/lcd_shm.h"
rm -rf "$BUILD/inc"
mkdir -p "$BUILD/inc"
cp -a "$HERE/duo-tdl-examples/include/system" "$BUILD/inc/system"
echo "  rtsp2lcd.c / lcd_shm.h / inc/system 就绪"

echo "--- 2) 编译 ---"
OUT="$BUILD/rtsp2lcd"
# ★ -D__CV181X__ 必须带: CVITEK 头(cvi_defines.h)用它在 CV181x/CV180x 之间选架构定义,
#   不带就直接 #error "Unknown Chip Architecture!"。SG2002(Duo256M)属 CV181x 家族。
#   依据: cvi_mpi/mpi_param.mk:145  CFLAGS += -D__CV181X__
"$GCC" -march=armv8-a -O2 -Wall -Wno-comment -DNDEBUG -std=gnu11 -fsigned-char \
	-D__CV181X__ \
	-I"$BUILD/inc/system" -I"$BUILD" \
	-o "$OUT" "$BUILD/rtsp2lcd.c" \
	-L"$BOARDLIBS" -lvdec -lsys -lpthread -lrt -lm

echo
echo "=== 产物 ==="
ls -l "$OUT"
file "$OUT" 2>/dev/null || true
echo
echo "下一步:"
echo "  scp $OUT root@192.168.42.1:/root/rtsp2lcd.new"
echo "  ssh root@192.168.42.1 'killall rtsp2lcd; mv -f /root/rtsp2lcd.new /root/rtsp2lcd; chmod +x /root/rtsp2lcd'"
echo "  ssh root@192.168.42.1 'LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd /root/rtsp2lcd --dump /tmp/one.ppm'   # 先验证解码"
