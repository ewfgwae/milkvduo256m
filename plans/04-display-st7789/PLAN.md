# PLAN.md — 04 ST7789 显示：方向调整 / 性能验证 / 排障流程

事实与判据见同目录 `AGENTS.md`；改固件的编译热更流程见 `../01-smallcore-firmware/PLAN.md`。

---

## 一、屏方向调整流程（上下颠倒 / 左右镜像时）

1. 改 `src/sdk_overlay/freertos/cvitek/task/comm/src/riscv64/lcd_st7789.c` 里的 `LCD_MADCTL`
   （当前 0x60 横屏）：
   - 上下颠倒：`0x60 ^ 0x40`；左右镜像：`0x60 ^ 0x20`；旋转 180°：`0x60 ^ 0xC0`（改完实测为准）。
2. 走 `../01/PLAN.md` 的改固件流程（build id +1、热更、验证）。
3. 验证：`lcd_sender bars` 看彩条方向；进画面模式看摄像头画面方向是否正。
   注意摄像头画面方向还取决于 rtsp2lcd 的裁剪/缩放，两个环节分开验证。

## 二、刷新性能验证流程

```bash
# 1) 待机指纹(黑屏+OSD):
sh /root/probe.sh; sleep 5; sh /root/probe.sh
#    [11] 几千像素/秒, [9] ~1ms/s

# 2) 画面模式满载指纹:
nohup sh /root/lcdcam.sh >/dev/null 2>&1 &
sh /root/probe.sh; sleep 5; sh /root/probe.sh
#    [11] ≈ 2.61M 像素/秒(= 46.875MHz SPI 的 89%), [7] ≈ 337 (33.7fps)
```

异常口径（见 `AGENTS.md` 性能指纹）：
- `[11]*2/[9]` 远低于 5.86 字节/微秒 → 查 `spi2_write` 是否退化为逐字节轮询；
  `g_spi2_probe[5]`（PIO 回退次数）应为 0。
- `g_dma2_probe[3]`（DMA 真超时）应为 0；非 0 = 双判据又漏了，回看 `driver/spi/src/spi.c`。

## 三、常见症状排障流程

| 症状 | 先看什么 | 结论/动作 |
| --- | --- | --- |
| 整屏黑、无 OSD | `lcd_sender ping`；背光 `bl 1` | ping 不通 → 小核/通路问题（模块 01）；背光关了 |
| 画面定格不刷新 | 探针 `[17]` 两次对比 | 冻住 = cmdqu 打满（模块 03 五节自愈），别信 `[11]/[18]` |
| 花屏/错位 | `g_dma2_probe[3]`、`g_spi2_probe[5]`；`sysdma_remap` 槽 4/5 | DMA 判据/请求线问题（`AGENTS.md`） |
| 帧率 1~2 FPS | 同上 + `[9]` 送显耗时 | 老 bug 复发口径，对照 build 0x484A0008 的修复点 |
| 颜色反/偏色 | `rtsp2lcd --dump` 抓帧比对 | 大核转换问题（模块 02），不是屏驱动 |

## 四、待办清单

- [ ] 大核用户态例子（`src/examples/st7789/`、`lvgl_port/`）只是性能对照参考，不随主链路维护；
      动它们不需要本模块流程，但也别拿它们验证小核链路。
