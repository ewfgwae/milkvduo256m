#!/bin/sh
# 2026-09-30 修: 原来直接调 PATH 里的 sample_vi_fd, 而 /root 下有个同名的
# 23048 字节残缺版会把它遮蔽(在 /root 里跑就失效), 故改为绝对路径。
export LD_LIBRARY_PATH=/mnt/system/lib:/mnt/system/usr/lib:/mnt/system/usr/lib/3rd

exec /mnt/system/usr/bin/ai/sample_vi_fd /mnt/cvimodel/scrfd_768_432_int8_1x.cvimodel
