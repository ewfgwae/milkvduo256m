# AGENTS.md — 01 小核固件（cvirtos.bin / 热更 / cmdqu / LVGL / 探针）

通用规范见根 `../../AGENTS.md`；显示硬件细节见 `../04-display-st7789/AGENTS.md`；
大核对端工具见 `../02-bigcore-tools/AGENTS.md`。

## 源文件地图（都在 `src/sdk_overlay/freertos/cvitek/` 下，按 SDK 相对路径）

| 文件 | 职责 |
| --- | --- |
| `task/lvgl/lvgl_task.c` | ★ 屏幕唯一所有者：UI / OSD / 画面模式 / 探针数组 `g_lvgl_probe[20]` |
| `task/lvgl/lv_conf.h` | LVGL 配置（字体、控件开关；改字体/控件先看这里） |
| `task/lvgl/lv_port_disp.c` | LVGL 显示驱动（半屏双缓冲 + sysDMA 异步送显埋点） |
| `task/comm/src/riscv64/comm_main.c` | cmdqu 命令分发（`prvCmdQuRunTask`）+ mailbox ISR |
| `task/comm/src/riscv64/lcd_shm.h` | ★ 共享内存协议（与 `src/big_core/lcd_shm.h` 逐字节一致，改一处同步两处） |
| `task/comm/src/riscv64/lcd_st7789.c/.h` | ST7789 驱动（含 `lcd_blit_start/wait` 异步送显） |
| `task/comm/src/riscv64/hotjump.S` | 常驻热更跳板（固定 0x8FF90000，不随固件更新） |
| `task/comm/src/riscv64/sysdma_test.c` | 启动时 sysDMA 自检 |
| `driver/spi/src/spi.c` | 小核直驱 SPI2 + sysDMA（DMA 完成双判据 + 超时熔断回退 PIO） |
| `driver/gpio/src/gpio.c` | GPIO（读-改-写；当前只有 output 接口，触摸需要 input 时再扩） |
| `kernel/include/riscv64/FreeRTOSConfig.h` | tick **1000Hz**（按 tick 算时间单位是 1ms） |
| `scripts/cv181x_lscript.ld` | 链接脚本：`.hotjump` / `.lvgl_fb`(NOLOAD) 段，应用区让出 0x70000 |

## 内存布局（热更为什么安全，改固件前先背下来）

```
0x8FE00000 .. 0x8FF90000   小核应用区（1638400B，热更时原地覆盖）
0x8FF90000 .. 0x8FFC0000   常驻热更跳板 .hotjump（永不覆盖）
0x8FF94000 .. 0x8FFC0000   LVGL 双缓冲 .lvgl_fb（NOLOAD，不进固件镜像）
0x8FFC0000 ..              LCD 共享内存（ctrl 64B + stat 64B + fb 153600B）
```

## 任务结构与线程安全纪律

- `lvgl_task`：优先级 `tskIDLE_PRIORITY+2`，栈 8192 字（32KB），主循环 1ms 一轮。
- `prvCmdQuRunTask`（CMDQU）：优先级 `+5`，队列深度 30。
- ★ **LVGL 不是线程安全的**：cmdqu 任务收到命令**只置 volatile 标志/计数**（单字写天然原子），
  渲染统一在 LVGL 任务的主循环 poll 做。新命令照 `LCD_CMD_CAM → lvgl_cam_submit()` 的样子接。
- ★ **`LCD_CMD_CAM` 绝不回 ACK**——每秒几十帧都回执会把 8 个 mailbox 槽位打满。

## 命令与协议规矩

- `ip_id` 固定 `IP_SYSTEM=6`；`cmd_id` 必须 ≤ 0x5F（Linux 驱动放行上限），**0x50..0x5F 已被
  SYS_CMD_INFO_\* 占用**，自留区间是 0x40..0x4F（已用到 0x48，0x49/0x4A 已在 `plans/05` 预定给
  BTN/UI）。
- 回执的 `cmd_id` 必须与请求一致（Linux 驱动按 (ip_id, cmd_id, block) 匹配唤醒 SEND_WAIT）。
- 缓存一致性靠「分线」：ctrl 线大核独占写、stat 线小核独占写，各自只 flush 自己那条 64B 线；
  画面帧由小核送显前 `inv_dcache_range` 整块 fb。

## build id 与探针（每一版固件的两件必做事）

- 每改一版小核固件，`lvgl_task.c` 的 `LVGL_PROBE_BUILD_ID` **+1**。已记录：
  `0x484A0008` 帧率修复 → `09` → `0A`（带 demo + CAM 模式）→ `0B`（去 demo，当前）→
  `0C` 已预定给按钮界面（`plans/05`）。
- 探针数组 `g_lvgl_probe[20]`（字段表见 PLAN.md 二）；**地址随固件体积变**，每版必须
  `nm cvirtos.elf | grep g_lvgl_probe` 重查；读探针步长必须按 **8**（RV64）。
  已知基址：`0x484A000A → 0x8fe6cfe0`，`0x484A000B → 0x8fe544a0`（去 demo 后 .bss 整段挪了 ~90KB）。

## 已知坑

- `make rtos` 复用陈旧 `.obj`：改完源码行为没变，删对应 `.obj` 再编（命令见 PLAN.md 一.3）。
- build 树里有 root 属主文件，普通用户编报 `Permission denied`——用 `wsl -u root` 跑。
- 板名必须写连字符形式 `milkv-duo256m-glibc-arm64-sd`，下划线形式报「板名 not found」。
- 换固件后探针地址跳变：还按老地址读到的是垃圾，很容易误判成「固件没起来」。
