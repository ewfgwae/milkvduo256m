#!/bin/sh
# 读小核 g_lvgl_probe(0x8fe6cfe0, RV64 每项 8 字节)的关键几项
P=0x8fe6cfe0
d() { devmem $((P + $1*8)) 32 2>/dev/null; }
echo "  [0]魔数    $(d 0)   [1]就绪 $(d 1)   [14]buildid $(d 14)"
echo "  [7]fps*10  $(d 7)     [8]整帧数 $(d 8)"
echo "  [9]送显us  $(d 9)     [11]像素/s $(d 11)  [12]flush次 $(d 12)"
echo "  [16]画面模式 $(d 16)  [17]大核提交 $(d 17)  [18]小核已画 $(d 18)  [19]$(d 19)"
