# AGENTS.md — 05 按钮界面（LVGL 中文 UI + 摄像头联动）★ 已验收（稳定运行）

通用规范见根 `../../AGENTS.md`；小核固件流程见 `../01-smallcore-firmware/`；
大核工具流程见 `../02-bigcore-tools/`；触摸接入见 `../06-touch/`。

## 需求（2026-10-24 与用户确认的三项拍板）

1. **屏上显示一个美观的按钮，按下后执行摄像头识别**（= 启动现有链路上屏：sample_vi_fd +
   rtsp2lcd，见模块 03）。
2. **终端打命令也能触发同样的"按下"效果**（屏无触摸 / 触摸芯片未定，命令是当前唯一可靠入口）。
3. **UI 文字用中文**；"识别"暂指现有画面链路，人脸检测框上屏是后续可选（见 `../03/PLAN.md` 待办）。

## 措辞定稿（2026-10-24 Phase 0 字库预检后与用户确认）

SIMSUN16 实际只覆盖 **1412 个码点**（偏日文 JIS 常用汉字，简体动词缺得厉害：
「启/摄/识/别/请/稍/试/监/频/开」等全缺，繁体「監」也缺）。原计划措辞全部不可用，
**定稿为全简体方案（已逐字验证在库）**：

| 元素 | 文案 |
| --- | --- |
| 标题 | 智能看家 |
| 按钮 | 相机画面 |
| 状态-待机 | 待机 |
| 状态-启动中 | 正在取像... |
| 状态-已停止 | 已停止 |
| 状态-超时(红) | 超时，再来一次 |
| 启动页（Phase 6） | 正在初始化... |

（「正在启动」的 启/动 都不在 SIMSUN16 里，2026-10-25 逐字预检后定「正在初始化...」；
在库备选：正在等待... / 等待中...，要改只动 `boot_create()` 一行字符串。）

后续如改文案，先按 Phase 0 的脚本对字库验证再动手，别等上板才见方块。

## 已定设计（实施时不要另起炉灶）

- **状态机**（小核 `lvgl_task.c` 内）：**上电 → 启动页（黑底转圈「正在初始化...」，无超时，
  UI 页建好即整体隐藏）→（`boot_leave()` 三入口：UI / BTN / 首帧 CAM）→** `待机 →
  （BTN 触发）→ 按下动画 → 正在启动…(spinner) → 首帧 LCD_CMD_CAM 到来 → 隐藏 UI 切画面；
  启动超时 **15s**（2026-10-24 用户拍板，原 25s）→ 红字"超时，再来一次"回待机（超时后
  首帧到仍自动进画面）`；`LCD_CMD_UI` → 删画面对象、回 UI 显示"已停止"（启动页下来则
  显示"待机"）。**MON / INIT / PING 故意不离启动页**（mon 与 rtsp2lcd 互斥；INIT 是
  热更后的 state 轮询，不能让它顶掉启动页）。
- **新命令**（`lcd_shm.h` 两份副本同步加）：
  - `LCD_CMD_BTN 0x49`：大核→小核，触发按钮按下动画 + 进入启动中；小核**不回 ACK**（同 CAM 纪律）。
  - `LCD_CMD_UI 0x4A`：大核→小核，退出画面模式回 UI 页。
  - `LCD_CMD_EXIT_BTN 0x4B`（Phase 5，2026-10-25）：大核→小核，画面模式右上角「退出」按钮
    出按下动画（300ms 弹起，纯视觉**不退画面**——停链路由大核做）；非画面模式忽略；不回 ACK。
  - （0x50..0x5F 被 SYS_CMD_INFO_* 占用，自留区间到 0x4F 为止。）
- **大核入口**：`lcd_sender cam on` = 发 BTN + `sh /root/lcdcam.sh start`；
  `cam off` = 先 `lcdcam.sh stop` 再发 UI（★ 顺序必须"先停链路再回 UI"，否则下一帧 CAM 又把 UI 盖住）。
  `lcdcam.sh` 新增 `start` 子命令：扫 /proc 防重复启动（argv[1] 匹配，同 stop 的写法）→ nohup 自身。
  `lcd_sender ui` = 单独发 UI（Phase 6）：开机 `auto.sh` 热更成功(rc=0)后自动发一次让小核
  离启动页；手动热更后回按钮页也用它。
- **触摸通道预留**：小核按钮 CLICKED 事件 → `stat.ui_event` 计数 +1（`lcd_shm.h` 的
  `stat.reserved[0]` 改名 `ui_event`，小核独占写、按 64B 线纪律 flush）。大核守护进程轮询它
  （`plans/06` Phase 3），**不走 cmdqu**——避开 8 槽 ENOBUFS 老坑。
- **中文字体**：`lv_conf.h` 打开 `LV_FONT_SIMSUN_16_CJK 1`（覆盖 1412 码点，偏 JIS 汉字，
  简体动词大面积缺字）。措辞以「措辞定稿」表为准（预检已完成，见上），不要为此引外部字库文件。
- **build id**：当前 `0x484A000F`（= 0E + 上电启动页，探针基址 `0x8fe82b98`）；
  改固件后按 `../01/PLAN.md` 一.4 重查基址。
- **验收基线**（2026-10-24，Phase 3 六步全过）：冷启 cam on 首帧 15s（正踩超时线，属预期）、
  热链路 9~12s、`[17]` +30~33/s、60s 零异常行、3 轮 on/off 无残留、长跑 240s +30.6/s、
  两次重启开机即 UI 待机。板上当日记录：`/root/指南/2026.10.24-显示-屏上按钮界面与相机联动.txt`。
- **不做的事**：画面模式下退出按钮的**触摸物理按下**（CLICKED 已计 `stat.ui_event`，等
  `plans/06` 大核守护进程接管停链路）；不动 `mon`/开机自启；不动 fip。

## 涉及文件清单（实施时逐个对号）

| 文件 | 改动 |
| --- | --- |
| `src/sdk_overlay/.../task/lvgl/lv_conf.h` | `LV_FONT_SIMSUN_16_CJK 0→1` |
| `src/sdk_overlay/.../task/lvgl/lvgl_task.c` | UI 页构建 + 状态机 + 启动页/boot_leave（P6） + build id 0x484A000F |
| `src/sdk_overlay/.../task/comm/src/riscv64/lcd_shm.h` | +LCD_CMD_BTN/UI；stat.reserved[0]→ui_event |
| `src/big_core/lcd_shm.h` | 与上者逐字节同步 |
| `src/sdk_overlay/.../task/comm/src/riscv64/comm_main.c` | 分发 BTN/UI（只置标志；P6 未动，复用 0x4A） |
| `src/sdk_overlay/.../task/comm/src/riscv64/lcd_touch.c/.h` | 新建：触摸驱动骨架（接口见 `../06`） |
| `src/big_core/lcd_sender.c` | +`cam on/off` 子命令；+`ui` 子命令（P6） |
| `tools/lcdcam.sh` | +`start` 子命令 |
| `sdcard/rootfs/mnt/data/auto.sh` | 热更成功(rc=0)后发 `lcd_sender ui`（P6） |
