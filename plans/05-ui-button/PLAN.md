# PLAN.md — 05 按钮界面：分阶段实施计划与测试流程

★ 状态：**Phase 0~4 完成并验收（2026-10-24）**；**Phase 5 画面模式「退出」按钮完成并验收
（2026-10-25 凌晨，build `0x484A000E`、探针基址 `0x8fe829d8`）**。
设计决定与文件清单见同目录 `AGENTS.md`；编译/热更/部署的通用流程引用模块 01 / 02，不重复。

---

## Phase 0 — 协议与字体（地基，先落）

改动：两份 `lcd_shm.h`（+`LCD_CMD_BTN 0x49`、`+LCD_CMD_UI 0x4A`、`stat.reserved[0]`→`ui_event`）、
`lv_conf.h`（SIMSUN 打开）。

要点：
1. 两份 `lcd_shm.h` 改完 `diff` 必须无差异。
2. `ui_event` 放 stat 线（小核独占写）内 4B，单字写原子；小核写后 `flush_dcache_range` 该 64B 线。

**验收 / 测试流程**：
- [x] 小核固件能编译过（2026-10-24 实测）：产物定长 1640640B（md5 3d5abe68…），主 LOAD 段
      filesz 0x4E308 ≈ 315KB，距应用区上限 1638400B 余量约 1.3MB —— SIMSUN16 字体数据
      （估 ≤200KB）加进去绰绰有余。注：当前无代码引用字体，--gc-sections 会裁掉它，
      Phase 1 引用后自动链入；
- [x] 字库覆盖检查（2026-10-24 完成）：SIMSUN16 实覆盖 1412 码点（偏 JIS 汉字），
      「启/摄/识/别/请/稍/试/监」等全缺，原措辞不可用；已与用户定稿**全简体方案**
      （智能看家/相机画面/待机/正在取像.../已停止/超时，再来一次），逐字验证在库。
      验证脚本思路：解析 `lv_font_simsun_16_cjk.c` 六个 cmap（连续段直取、稀疏段
      range_start+unicode_list、FULL 段 ofs 非 0），目标字逐个比对。

> Phase 0 附注：`tools/build_small_core.sh` 原来在 source envsetup 时会被自身 `set -euo
> pipefail` 弄死（SDK 链路里有故意的非零退出命令 + 未定义变量引用），已修：source 期间
> 临时 `set +eu`、兜底 `export TOP="$SDK"`。以后一键编译直接可用。

## Phase 1 — 小核 UI 页 + 状态机（核心）

改动：`lvgl_task.c`（UI 构建、状态机、`cam_exit()`）、`comm_main.c`（BTN/UI 分发）、
新建 `lcd_touch.c/h` 骨架（indev 注册，`lcd_touch_read()` 恒返回未按下）。

UI 版式（320×240，深色；中文文案一律用「措辞定稿」表，字库预检已完成）：
- 标题「智能看家」（SIMSUN16，字距加宽）+ 青色下划线细条 + 副标题 `MILK-V DUO 256M`（Montserrat14 灰）；
- 大按钮 ~230×62、圆角 31、垂直渐变（青蓝系）+ 半透明描边，按下态缩小+变暗；文案「相机画面」；
- 状态栏（SIMSUN16）：待机 / 正在取像... / 已停止 / 超时，再来一次（红）；
- 启动中 spinner（30px 弧）居中显示；左下角小字 `lcd_sender cam on / off`（Montserrat12）；
- 整页挂在一个容器对象下：进画面模式 `lv_obj_add_flag(cont, LV_FLAG_HIDDEN)` 一次性隐藏，
  退画面再显示（省得逐个对象藏）；OSD 仍在 sys 层，不受影响。

状态机纪律：
- comm 侧 `lvgl_ui_btn_trigger()` / `lvgl_ui_return_trigger()` 只置 volatile 标志，主循环 poll
  （照 `lvgl_cam_submit()` 的既有模式）；BTN 在画面模式下来时忽略（防 `cam on` 连打）。
- 按下动画：`lv_obj_add_state(btn, LV_STATE_PRESSED)` 保持 ~180ms 后移除，再进「启动中」；
  启动中按钮加 `LV_STATE_DISABLED` 防连击。
- `cam_exit()`：删 `s_cam_img`、`s_cam_req=0`、显示容器、状态回「已停止」；若正处于「启动中」
  收到 UI 命令，按取消处理直接回待机。
- 超时 25s：覆盖 lcdcam 冷启动最坏路径（服务重拉 30s 上限的场景宁可让它显示超时后仍自动进画面，
  也不把超时拉到 30s+ 让用户干等）。

**验收 / 测试流程**（WSL 编译 → 按 `../01/PLAN.md` 热更，build id 0x484A000C）：
- [x] 上电：UI 页完整（标题/按钮/状态/提示），无方块字（2026-10-24 目视通过）；
- [x] 发 BTN（Phase 2 前用板上临时脚本 `python3 /root/sendcmd.py 0x49` 直发 cmdqu，
      结构体照 `rtos_cmdqu.h` 8B packed + ioctl `0x40087201`）：按钮按下动画可见，
      spinner 转、状态「正在取像...」（目视通过）；
- [x] 起画面链路后首帧到达 → UI 消失、画面满屏、OSD 仍在（探针 `[16]=1`、33.8fps、
      2.61M px/s，与模块 03 健康值一致）；
- [x] 不起链路发 BTN → 25s 后红字「超时，再来一次」回待机（目视 + 探针：启动中
      帧率 ~515fps=spinner 在画，超时后回 1.0fps=已收）；
- [x] 停链路后发 0x4A：画面消失回 UI，「已停止」（`[16]=0`，进程收干净）；
- [x] 回归：`[14]=0x484A000C`、`[2]` 在涨、`ping` 正常；`bars` 报「等 ack 超时」=
      **0B 起的既有现象**（屏幕归 LVGL 后小核无 FLUSH 处理，本次未动该路径），非回归。

Phase 1 实施结果（2026-10-24）：新探针基址 `0x8FE82818`（probe.sh 板上/仓库均已更新）；
固件 md5 `5240a233…`，主 LOAD 段 315KB→508KB（SIMSUN16 ~190KB 链入）；改动为
`lvgl_task.c/h`（UI 页+状态机+cam_exit）、`comm_main.c`（BTN/UI 分发）、新建
`lcd_touch.c/h`（骨架，恒未按下）。板上临时脚本 `/root/sendcmd.py` 留到 Phase 2
验收完再删。

## Phase 2 — 大核入口（cam on/off + lcdcam start）

改动：`lcd_sender.c` 加 `cam` 子命令（用法注释同步）；`tools/lcdcam.sh` 加 `start`。

要点：
1. `cam on`：先 `cmd_send(LCD_CMD_BTN,1)` 再 `system("sh /root/lcdcam.sh start")`（屏上先有反馈）。
2. `cam off`：先 `system("sh /root/lcdcam.sh stop")` **等它返回**，再 `cmd_send(LCD_CMD_UI,0)`。
3. `start` 子命令：扫 /proc 只认 argv[1] 是 `/root/lcdcam.sh` 的实例，有就退出（防重复），
   没有就 `nohup sh "$0" >/dev/null 2>&1 &`；别对整条 cmdline 做 grep（历史事故见模块 02）。

**验收 / 测试流程**（部署按 `../02/PLAN.md` 一/三）：
- [x] `lcd_sender cam on`：BTN 先出（~1s），13s 时已 `[16]=1`、33.7fps、`[17]` 持续涨
      （2026-10-24 实测）；
- [x] 连打两次 `cam on`：`[lcdcam] 循环已在跑 … 不重复启动`，循环实例数 = 1；
- [x] `lcd_sender cam off`：画面退净回 UI（`[16]=0`、fps 回 1.0），lcdcam/rtsp2lcd
      已收干净（sample_vi_fd 属取流服务，`stop` 不动它，与设计一致）；
- [x] `cam on` → 杀 sample_vi_fd → 自愈恢复：25s 后 `[17]` 已继续 +33fps 涨
      （模块 03 两级自愈不回归）。

Phase 2 实施结果（2026-10-24）：`lcd_sender` 新增 `cam on/off`（md5 `9fa74ddb…`，
板上 /root 与 /mnt/data 及仓库两副本同步）；`lcdcam.sh` 新增 `start` 子命令
（md5 `b7cb3803…`，板上与仓库副本同步）。板上临时脚本 sendcmd.py 已删。

## Phase 3 — 端到端联调与回归（收口）

**测试流程**（按顺序全过才算完成）：
1. 冷启动 → UI 待机（黑底深色页 + OSD）。
2. `cam on` → 动画 → 启动中 → 画面（≤10s；超时路径单测过）。
3. 画面 60s：`[17]` +30/秒、无 ENOBUFS。
4. `cam off` → 回 UI「已停止」→ 再 `cam on` 循环 3 次（状态机不残留）。
5. 长跑 4 分钟（模块 03 三节判据全过）。
6. 重启板子：开机后仍是新固件 + UI 待机（`/mnt/data/cvirtos.bin` 已同步）。

Phase 3 实施结果（2026-10-24 晚，build 0x484A000D = 超时 25s→15s 后复测，六步全过）：
- 超时改 15s → 重编（md5 `29d29d8a…`，定长 1640640B，**探针基址不变 0x8FE82818**——只改
  常量没动体积）→ 热更 200ms → `[14]=0x484A000D`。
- 超时单测（板上临时 sendbtn.py 直发 BTN、无链路，测完即删）：+10s 仍 515fps（spinner
  在画），+17s 回 1.0fps（已收）——15s 生效，`[16]` 全程 0。
- 步 1/6 两次重启：均 44s SSH 回来、`[14]=0x484A000D`、UI 待机（fps 1.0、`[16]=0`）、
  `[2]` 在涨——开机 auto.sh 热更源已生效。
- 步 2 首帧计时：**冷启动（sample_vi_fd 也要起）恰 15s，正踩超时线**，终态正确进画面
  （极端情况屏上先闪超时红字再自动进画面，属 15s 定值已接受的观感）；链路热时 9~12s。
- 步 3：60s `[17]` +1809 = **+30.15/s**，lcdcam.log 零异常行，33.7fps / 2.61M px/s。
- 步 4：3 轮 on/off 全净——每轮 off 后 `[16]=0`、fps 回 1.0、lcdcam/rtsp2lcd 实例 0。
- 步 5 长跑 240s：16 个采样点 `[17]` 每 15s 稳 +458（+30.6/s）、fps 33.7、无 ENOBUFS。
  ★ 期间暴露 longrun.sh 缺陷：它用 `killall lcdcam.sh` 清场打不中 `sh /root/lcdcam.sh`
  （busybox killall 按 argv[0]，根 AGENTS 已有记载），lcdcam 循环把 rtsp2lcd 重新拉起并
  占住 RTSP 单客户端名额正常供流（`[17]` 一直涨的正是它），longrun 自己的实例被挤在门外
  刷停顿行——**流没断，是 longrun 的监测瞎了**。按"只看 [17]"纪律判定通过；后续跑
  longrun 前先 `cam off` 清场（已写进指南 1.6；修 longrun.sh 属模块 03，本轮不动）。

## Phase 4 — 文档与副本收尾

- [x] 板上 `/root/指南/` 写当日记录（`2026.10.24-显示-屏上按钮界面与相机联动.txt`，
      含 UI 行为/命令/指纹/longrun 坑/sendbtn.py 片段），已 scp 到板并 md5 比对一致；
- [x] README 更新（上电行为改「UI 待机页」、6.2 表加 `cam on/off`、6.5 加一条命令入口、
      build id 表补 0C/0D、p2 拷贝表的 build id/md5 更新）；
- [x] `sdcard/` 副本同步 + md5 比对：cvirtos.bin×2 = `29d29d8a…`（= 板上 /mnt/data）、
      lcd_sender×2 = `9fa74ddb…`、lcdcam.sh = `b7cb3803…`，全一致；
- [x] 本 PLAN 回填（见上）、`AGENTS.md` 的 build id 补 `0x484A000D`、根 AGENTS 导航表
      05 状态改「稳定运行」、`tools/probe.sh` 头注释补 0D 行；git 提交。

## Phase 5 — 画面模式「退出」按钮（2026-10-24 拍板，原 P2 项提前落地指令侧）

用户要求：识别画面上加退出按钮，且可用指令"视为按下"。方案（与用户确认）：升级 `cam off`
为触发者 + 右上角 88×34 小按钮。

改动：`lcd_shm.h`×2（+`LCD_CMD_EXIT_BTN 0x4B`，纯视觉反馈、不回 ack、非画面模式忽略）、
`lvgl_task.c/h`（`cam_enter` 建右上角半透明深底红边「退出」按钮（字库预检：退/出 在库，
关/闭/结 不在）、收到 0x4B 出按下动画 300ms 自动弹起、`cam_exit` 连按钮一起删、CLICKED 计
`stat.ui_event` 给 plans/06 触摸用；build id → `0x484A000E`）、`comm_main.c`（分发 0x4B）、
`lcd_sender.c`（`cam off` 三步：EXIT_BTN → lcdcam stop → UI）。

**验收 / 测试流程**（2026-10-25 00:0x 实测全过）：
- [x] `cam on` → 画面满屏 + 右上角「退出」按钮（视觉项，用户目视确认；探针 `[16]=1`、
      `[17]` +26~29/s）；
- [x] 画面中直发 0x4B → 只闪按下动画**不退画面**（`[16]` 保持 1、`[17]` 继续涨——防直发
      造成大小核状态错位）；
- [x] `cam off` → 退出按钮按下动画 → `[16]=0`、fps 回 1.0、lcdcam/rtsp2lcd 实例 0；
- [x] 待机发 0x4B → 忽略不崩（`[2]` 在涨、`[7]` 保持 1.0fps）；
- [x] 回归：第 2 轮 on/off 干净；`[14]=0x484A000E`；新探针基址 `0x8fe829d8`
      （probe.sh 板上/仓库已更新）。

Phase 5 实施结果（2026-10-25）：固件 md5 `0e6fd865…`、lcd_sender md5 `2c734cba…`，
板 /mnt/data 与仓库副本一致。附带发现并处置 SDK build 树损坏（22:57 一次不明清零事件把
cvirtos.elf/map、install/lib/liblvgl.a、多个 .ninja_* 与 lvgl_task.c.obj 清成 0 字节——
.cvIRTOS.bin/.dis 完好）：删 `freertos/cvitek/build` 全量重建 + 补 install/lib/liblvgl.a
后恢复。板上指南：`/root/指南/2026.10.25-显示-识别画面退出按钮.txt`。

## 待确认清单

- [x] P0：SIMSUN 字库是否覆盖「摄」等字 —— **不覆盖**（缺 启/摄/识/别/请/稍/试/监 等），
      已换全简体措辞定稿（见 AGENTS.md「措辞定稿」），逐字验证通过。
- [x] P1：启动超时定值 —— **用户拍板 15s**（2026-10-24，原 25s）。实测冷启动首帧恰 15s
      踩线（可能先闪超时红字再自动进画面）、热链路 9~12s 无感；已按 15s 全量回归。
- [x] P2：画面模式下的屏上「停止」按钮 —— **指令侧已做（Phase 5，「退出」按钮 + cam off
      触发）**；触摸物理按下仍挂 `plans/06` Phase 3/4（CLICKED 已计 `stat.ui_event`，等
      大核守护进程接管）。
- [ ] P2：人脸检测框上屏（先验证流里有没有框，见 `../03/PLAN.md` 待办）。
