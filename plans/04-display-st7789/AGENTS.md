# AGENTS.md — 04 ST7789 显示（接线 / SPI2+sysDMA / 方向 / 性能）

通用规范见根 `../../AGENTS.md`；小核驱动代码归属见 `../01-smallcore-firmware/AGENTS.md`。

## 硬件事实

- 屏：ST7789 240×320 SPI，**无触摸**（触摸是模块上额外芯片的事，见 `../06-touch/`）。
- ★ 电平**只能 3.3V**，切勿接 5V。
- 当前按 **320×240 横屏**用（`lcd_st7789.c` 的 `LCD_MADCTL = 0x60`）；320×240×2 == 240×320×2，
  共享内存布局不随方向变。
- SPI 时钟实测 ~46.875MHz（= 5.86 字节/微秒，性能判据的基准）。

## 接线（Duo256M 40PIN；大核 spidev 例子与小核寄存器直驱用同一组物理引脚）

| 屏 | 功能 | GP | 物理脚 | SG2002 焊盘 |
| --- | --- | --- | --- | --- |
| SCL | SPI2 时钟 | GP6 | 9 | SD1_CLK |
| SDA | SPI2 数据出 | GP7 | 10 | SD1_CMD |
| CS | 片选 | GP9 | 12 | SD1_D3 |
| DC | 数据/命令 | GP20 | 26 | XGPIOA[27] |
| RES | 复位 | GP21 | 27 | XGPIOA[26] |
| BLK | 背光 | GP16 | 21 | XGPIOA[23] |
| VCC | 电源 | 3V3(OUT) | 36 | — |
| GND | 地 | GND | 任意 | — |

- GP6~GP9 默认就是 SPI2 功能（FSBL 的 `cvi_board_init.c` 做 pinmux），不用额外配。
- `SPI2_SDI`（GP8，脚 11）屏不回读可不接；接上更稳。
- 小核侧 CS 靠 SPI2 的 SER 位自动片选，固件不占 GPIO。

## 驱动架构事实

- **屏完全由小核独占**：`lvgl_task.c` 是唯一所有者，SPI2 寄存器 + sysDMA 直驱，不经 Linux
  spidev / GPIO 子系统。大核只能「写共享内存 + 发 mailbox 命令」。
- 送显：半屏双缓冲 + sysDMA 异步（`lcd_blit_wait()` → `lcd_blit_start()` 配对调用）。
- **SPI2 的 DMA 请求线**必须 `tx = 5`（板级 DTS `dmas = <&dmac 4 …> <&dmac 5 …>`）。
  自查：`cat /proc/device-tree/sysdma_remap/ch-remap`，槽 4 应为 `0x14`、槽 5 应为 `0x15`；
  填错会让 DMA 通道永远卡在「已使能、等握手」。
- **DMA 完成判据是两条并用**：中断位置位 **或**（`DMAC_CH_EN` 通道位=0 且 `CH_LLP`=0）——
  中断位约 35% 概率读不到但硬件已收摊；另有连续超时熔断回退 PIO。都在 `driver/spi/src/spi.c`。
- 大核侧：`linux_5.10/drivers/spi/spidev.c` 的 `bufsiz` 4096 → 262144（大核用户态整屏刷才够）。

## 性能指纹（回归判据）

- 待机（黑屏+OSD）：`[11]` 每秒几千像素、`[9]` ~1ms/s。
- 画面模式：`[11]` ≈ 2.61M 像素/秒 ≈ SPI 时钟的 89%，`[7]` ≈ 33.7fps（33fps 反复推同一张也达峰）。
- 有效送显字节率 = `[11]×2 字节 / [9] 微秒`，与 5.86 字节/微秒比：接近 = 卡 SPI 时钟（正常），
  差得远 = 卡 spi2_write 逐字节轮询（异常）。

## 坑

- 帧率掉到 1~2 FPS 的老 bug 已修（build 0x484A0008，双判据+熔断）；复发先读 `g_dma2_probe[3]`
  超时次数（应为 0）与 `g_spi2_probe[5]` PIO 回退次数（应为 0）。
- 屏上下颠倒/左右镜像 → 改 `LCD_MADCTL` 热更一次即可，不用动别的。
- 背光由 `LCD_CMD_BL` / `lcd_sender bl 0|1` 控制，上电默认开。
