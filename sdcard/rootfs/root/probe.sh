#!/bin/sh
# 读小核 g_lvgl_probe 的关键几项。
#
# ★ 基址随固件体积变化, 换固件后必须重查:
#     nm <SDK>/freertos/cvitek/install/bin/cvirtos.elf | grep g_lvgl_probe
#   也可以在板上用 /root/rtos_base.sh 或直接改下面的 P。
#   build id 对照:
#     0x484A0008 / 0x484A0009 / 0x484A000A -> 0x8fe6cfe0
#     0x484A000B (去掉 lvgl demo)          -> 0x8fe544a0
#
# ★ 小核是 RV64, unsigned long = 8 字节 —— 步长必须是 8, 4 字节读出来全错位。
P=${PROBE_BASE:-0x8fe544a0}
d() { devmem $((P + $1*8)) 32 2>/dev/null; }
echo "  base=$P"
echo "  [0]魔数    $(d 0)   [1]就绪 $(d 1)   [14]buildid $(d 14)"
echo "  [2]主循环  $(d 2)   [3]tick_ms $(d 3)"
echo "  [7]fps*10  $(d 7)     [8]整帧数 $(d 8)"
echo "  [9]送显us  $(d 9)     [11]像素/s $(d 11)  [12]flush次 $(d 12)"
echo "  [16]画面模式 $(d 16)  [17]大核提交 $(d 17)  [18]小核已画 $(d 18)  [19]$(d 19)"
