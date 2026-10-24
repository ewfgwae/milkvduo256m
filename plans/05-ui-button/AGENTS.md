# AGENTS.md — 05 按钮界面（LVGL 中文 UI + 摄像头联动）★ 待实施

通用规范见根 `../../AGENTS.md`；小核固件流程见 `../01-smallcore-firmware/`；
大核工具流程见 `../02-bigcore-tools/`；触摸接入见 `../06-touch/`。

## 需求（2026-10-24 与用户确认的三项拍板）

1. **屏上显示一个美观的按钮，按下后执行摄像头识别**（= 启动现有链路上屏：sample_vi_fd +
   rtsp2lcd，见模块 03）。
2. **终端打命令也能触发同样的"按下"效果**（屏无触摸 / 触摸芯片未定，命令是当前唯一可靠入口）。
3. **UI 文字用中文**；"识别"暂指现有画面链路，人脸检测框上屏是后续可选（见 `../03/PLAN.md` 待办）。

## 已定设计（实施时不要另起炉灶）

- **状态机**（小核 `lvgl_task.c` 内）：`待机 →（BTN 触发）→ 按下动画 → 正在启动…(spinner) →
  首帧 LCD_CMD_CAM 到来 → 隐藏 UI 切画面；启动超时 25s → 红字"启动超时"回待机`；
  `LCD_CMD_UI` → 删画面对象、回 UI 显示"已停止"。
- **新命令**（`lcd_shm.h` 两份副本同步加）：
  - `LCD_CMD_BTN 0x49`：大核→小核，触发按钮按下动画 + 进入启动中；小核**不回 ACK**（同 CAM 纪律）。
  - `LCD_CMD_UI 0x4A`：大核→小核，退出画面模式回 UI 页。
  - （0x50..0x5F 被 SYS_CMD_INFO_* 占用，自留区间到 0x4F 为止。）
- **大核入口**：`lcd_sender cam on` = 发 BTN + `sh /root/lcdcam.sh start`；
  `cam off` = 先 `lcdcam.sh stop` 再发 UI（★ 顺序必须"先停链路再回 UI"，否则下一帧 CAM 又把 UI 盖住）。
  `lcdcam.sh` 新增 `start` 子命令：扫 /proc 防重复启动（argv[1] 匹配，同 stop 的写法）→ nohup 自身。
- **触摸通道预留**：小核按钮 CLICKED 事件 → `stat.ui_event` 计数 +1（`lcd_shm.h` 的
  `stat.reserved[0]` 改名 `ui_event`，小核独占写、按 64B 线纪律 flush）。大核守护进程轮询它
  （`plans/06` Phase 3），**不走 cmdqu**——避开 8 槽 ENOBUFS 老坑。
- **中文字体**：`lv_conf.h` 打开 `LV_FONT_SIMSUN_16_CJK 1`（覆盖约 1000 常用字）。
  ★ 风险：所需字（"摄"最可疑）可能不在字库——Phase 1 有验证步骤，缺字就换措辞
  （如"启动视频识别"），不要为此引外部字库文件。
- **build id**：`0x484A000C`；探针基址随之变，按 `../01/PLAN.md` 一.4 重查。
- **不做的事**：画面模式下的屏上"停止"按钮（没有大核守护进程时点了停不掉链路，是假按钮，
  留给 `plans/06` Phase 4 一起做）；不动 `mon`/开机自启；不动 fip。

## 涉及文件清单（实施时逐个对号）

| 文件 | 改动 |
| --- | --- |
| `src/sdk_overlay/.../task/lvgl/lv_conf.h` | `LV_FONT_SIMSUN_16_CJK 0→1` |
| `src/sdk_overlay/.../task/lvgl/lvgl_task.c` | UI 页构建 + 状态机 + build id 0x484A000C |
| `src/sdk_overlay/.../task/comm/src/riscv64/lcd_shm.h` | +LCD_CMD_BTN/UI；stat.reserved[0]→ui_event |
| `src/big_core/lcd_shm.h` | 与上者逐字节同步 |
| `src/sdk_overlay/.../task/comm/src/riscv64/comm_main.c` | 分发 BTN/UI（只置标志） |
| `src/sdk_overlay/.../task/comm/src/riscv64/lcd_touch.c/.h` | 新建：触摸驱动骨架（接口见 `../06`） |
| `src/big_core/lcd_sender.c` | +`cam on/off` 子命令 |
| `tools/lcdcam.sh` | +`start` 子命令 |
