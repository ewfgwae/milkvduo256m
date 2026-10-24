# PLAN.md — 05 按钮界面：分阶段实施计划与测试流程

★ 状态：**Phase 0~4 完成并验收（2026-10-24）**；**Phase 5 画面模式「退出」按钮完成并验收
（2026-10-25 凌晨，build `0x484A000E`、探针基址 `0x8fe829d8`）**；**Phase 6 上电启动页
完成并验收（2026-10-25 凌晨，build `0x484A000F`、探针基址 `0x8fe82b98`）**。
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

## Phase 6 — 上电启动页：大核未就绪时只显「转圈 + 正在初始化...」（★ 计划书，未实施）

用户需求（2026-10-25）：大核没启动时小核不再直接显示完整按钮 UI，改成一个转圈 +
「正在启动」文字的启动页，观感更好。**本节是计划，尚未动任何代码。**

### 字库预检（2026-10-25 已做，只读）

用户点名文案「正在启动」在 SIMSUN16 里**不可用**——实测缺「启」和「动」（「正」「在」
在库）。用 Phase 0 同款解析思路（解析 `lv_font_simsun_16_cjk.c` 的 cmap：连续段直取、
稀疏段 range_start+unicode_list、FULL 段 ofs 非 0）对候选逐字验证：

| 候选文案 | 结果 |
| --- | --- |
| 正在启动... | ✗ 缺「启」「动」 |
| 载入中... / 加载中... / 读取中... | ✗ 缺「载」「读」 |
| 系统准备中 / 等待就绪 | ✗ 缺「准」「备」「绪」 |
| **正在初始化...** | ✓ **推荐**（语义最接近「正在启动」） |
| 正在等待... / 等待中... | ✓ 备选 |

ASCII 的 "..." 与现有「正在取像...」同路数，无碍。

### 设计决定（实施时不要另起炉灶）

- **不新增协议命令，复用 `LCD_CMD_UI 0x4A`**。小核侧对"启动页状态"单独解释：收到
  UI = 大核就绪 → 撕启动页、显示 UI 待机页，状态文字「待机」（不是「已停止」——
  camera 从没跑过）。`lcd_shm.h` 两份副本**一个字都不用改**，无跨核协议风险，
  `comm_main.c` 也不动（0x4A 分发已存在）。
- **小核实现走"UI 页照建、整体隐藏 + 启动页覆盖层"（最小改动）**：`ui_create()` 照旧
  上电建好整页，随后加 `LV_OBJ_FLAG_HIDDEN` 藏起；另建全屏不透明黑底容器
  `s_boot_cont`（z 序：UI 页之上、sys 层 OSD 之下），内放 56px spinner（青色，样式照
  「启动中」那只放大）+ SIMSUN16 文案居中。右下角 OSD **保留**（诊断用）。
  新增 `boot_leave()`（幂等，`s_boot_cont` 非空才动手）：删启动页容器、取消 UI 页
  隐藏。**三个离页入口都调它**：
  1. `ui_state_poll()` 的 UI 分支（`lcd_sender ui` / `cam off` 末步）；
  2. BTN 分支开头（`cam on` 当开机首个命令：先出 UI 页再播按下动画）；
  3. `cam_enter()` 开头（不 BTN 直接起 rtsp2lcd：首帧 CAM 直进画面，启动页不能
     残留在画面之上）。
  EXIT_BTN 在启动页照旧忽略（本就要求画面模式）；**MON/INIT/PING 不离页**（有意：
  mon 是诊断工具且与 rtsp2lcd 互斥，不在正常开机路径上；`lcd_sender rtos` 热更后
  本来就会连发 INIT 轮询 state，不能让它把启动页顶掉）。
- **启动页无超时**：大核没起来就永远转圈——这是需求本身（SD 坏 / Linux 不起 = 转圈
  = 语义正确），不做"N 秒后自动切 UI"。
- **build id**：`0x484A000F`；探针基址热更后 `nm cvirtos.elf | grep g_lvgl_probe`
  重查并更新 `tools/probe.sh`（仓库 + 板上两份）。
- **大核侧两处小改**：
  - `lcd_sender.c` 加 `ui` 子命令（≈6 行：`cmd_send(LCD_CMD_UI,0)` + 提示语）——
    `cam off` 最后一步就是发 UI，逻辑同源；
  - `auto.sh`：`lcd_sender rtos` **成功（rc=0）后**追加 `/mnt/data/lcd_sender ui`。
    只在成功后发：失败时屏上跑的是 fip 里的老固件（0x484A0009），它不认识 0x4A，
    default 分支会把命令原样回发，虽无害但没必要。
- **冷启动观感（先说清，避免误会）**：上电到热更完成前（约 30~40s）屏上跑的是
  fip.bin 里的老固件，仍显示老 UI 待机页——这段只有重烧 fip 才能改（变砖风险，
  铁律不动）。实际看到的是：老 UI 页 → 热更完成瞬间启动页闪现 1~2s → `ui` 到达切
  UI 待机页。启动页**常驻**的场景是：热更失败 / Linux 没起来 / 开发中手动热更后
  （此时 `lcd_sender ui` 或任何 cam 命令即可离页）。

### 涉及文件清单（实施时逐个对号）

| 文件 | 改动 |
| --- | --- |
| `src/sdk_overlay/.../task/lvgl/lvgl_task.c` | 启动页容器 + `boot_leave()` 三入口 + build id `0x484A000F` |
| `src/big_core/lcd_sender.c` | `ui` 子命令 |
| `sdcard/rootfs/mnt/data/auto.sh` | 热更成功后发 `lcd_sender ui`（板上 `/mnt/data/auto.sh` 同名同步） |
| `tools/probe.sh` | 新探针基址 + 头注释 0F 行（仓库 + 板上） |
| `lcd_shm.h` ×2、`comm_main.c` | **不改**（复用 0x4A 既有分发） |

### 实施步骤（小步可验证）

1. 只改小核 `lvgl_task.c`（build id 0F）→ WSL 编译（定长 1640640B）+ `nm` 查基址 →
   热更 → 单测启动页与三个离页入口（测试 1~5，期间可用板上临时脚本直发 0x4A/0x49）。
2. 大核 `lcd_sender ui` 子命令 → 编译部署 → 测试 2 复测（不再依赖临时脚本）。
3. `auto.sh` 追加 ui → scp + md5 比对 → 测试 7（重启两轮）。
4. 收尾：sdcard 三件（cvirtos.bin×2、lcd_sender×2、auto.sh）md5 三方一致；README
   上电行为；板上 `/root/指南/` 当日记录；`plans/01/PLAN.md` 热更标准流程补一句
   「热更后要回 UI 页发 `lcd_sender ui`」；本 PLAN 回填实施结果；`AGENTS.md` 状态机
   补「启动页」初态。

### 验收 / 测试流程（2026-10-25 凌晨实测全过；1~6 用板上 python 直发 0x4A/0x49/0x4B，
### 2 又用新 `lcd_sender ui` 复测）

- [x] 1 热更后不发任何命令：`[14]=0x484A000F`、`[2]` 在涨（0x90C→0xF16/3s）、
      `[7]=503fps`（spinner 在画）、`[16]=[17]=0`；静置 60s 仍 503fps 不自切。
- [x] 2 `lcd_sender ui`：`[7]` 回 `0xA`（1.0fps）= UI 待机页；连发两次幂等
      （直发 0x4A 与 `lcd_sender ui` 两种方式各测过）。
- [x] 3 热更后首个命令直发 BTN(0x49)：`[7]=491fps`（启动中 spinner 在画）、
      `[16]=0` 全程；无链路 15s 超时后 `[7]` 收回 1.0fps（超时回归一并过）。
- [x] 4 热更后直接 `sh /root/lcdcam.sh start`（首帧 CAM 无 BTN）：`[16]=1`、
      `[17]` +142/5s（+28/s）、`[7]=337`（33.7fps）——直进画面，启动页无残留。
- [x] 5 启动页直发 0x4B：忽略不崩（`[7]` 仍 ~509fps、`[2]` 在涨）；随后 0x4A
      离页回待机 1.0fps。
- [x] 6 启动页 `mon 1000`：仍转圈（`[7]` 504fps 不离页，有意）、`[5]=30%、[6]=5`
      有数；killall 后 `[6]=0`（末条 hb=0 落地）。
- [x] 7 回归：cam on/off 3 轮全净（每轮 `[16]=1`、`[17]`+140~143/5s、off 后
      `[16]=0`/1.0fps/无 rtsp2lcd+lcdcam 残留）；**重启 ×2**：每次 SSH ~50s 回来、
      `[14]=0x484A000F`、1.0fps UI 待机、`/tmp/lcd_reload.log` 尾行「UI 已发出」，
      重启 2 后补测 cam on（`[17]`+150/5s）/off 干净。
- [x] 8 收尾：sdcard 副本 md5 三方一致（cvirtos.bin×2 `24c5fcd3…`、lcd_sender×2
      `5fa812e8…`、auto.sh `4d1ac645…` = 板上）；probe.sh 基址更新（板上+仓库
      `7ba2cb35…`）；README、板上指南（追加进 2026.10.25 那份，md5 `5f3d89e8…`
      两边一致）、plans/01 补 ui 一句、本 PLAN 与 AGENTS 回填；git 提交。

### 待用户拍板 → 已按默认落地

- 文案：**「正在初始化...」**（实施时用户未另指定，按推荐项落地；备选
  「正在等待...」/「等待中...」实测在库，后续要改只动 `boot_create()` 一行字符串）。
- OSD：**保留**（启动页右下角照显 fps/C906，mon 时 A53 也有数）。

Phase 6 实施结果（2026-10-25 凌晨）：固件 md5 `24c5fcd3…`（定长 1640640B），新探针
基址 `0x8fe82b98`（probe.sh 板上/仓库均已更新）；`lcd_sender` md5 `5fa812e8…`（+`ui`
子命令）；`auto.sh` md5 `4d1ac645…`（热更成功才发 ui，防 fip 老固件收到不认识的 0x4A）。
改动仅 `lvgl_task.c`（启动页 + `boot_leave()` 三入口 + build id 0F）、`lcd_sender.c`、
`auto.sh`、`probe.sh`；`lcd_shm.h` 两份与 `comm_main.c` 未动（复用 0x4A）。板上指南
追加在 `/root/指南/2026.10.25-显示-识别画面退出按钮.txt`（1.4/1.5 节 + 变更记录末条）。

## 待确认清单

- [x] P0：SIMSUN 字库是否覆盖「摄」等字 —— **不覆盖**（缺 启/摄/识/别/请/稍/试/监 等），
      已换全简体措辞定稿（见 AGENTS.md「措辞定稿」），逐字验证通过。
- [x] P1：启动超时定值 —— **用户拍板 15s**（2026-10-24，原 25s）。实测冷启动首帧恰 15s
      踩线（可能先闪超时红字再自动进画面）、热链路 9~12s 无感；已按 15s 全量回归。
- [x] P2：画面模式下的屏上「停止」按钮 —— **指令侧已做（Phase 5，「退出」按钮 + cam off
      触发）**；触摸物理按下仍挂 `plans/06` Phase 3/4（CLICKED 已计 `stat.ui_event`，等
      大核守护进程接管）。
- [ ] P2：人脸检测框上屏（先验证流里有没有框，见 `../03/PLAN.md` 待办）。
