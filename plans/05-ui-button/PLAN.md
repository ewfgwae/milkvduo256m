# PLAN.md — 05 按钮界面：分阶段实施计划与测试流程

★ 状态：**计划已定稿，未开始实施**（2026-10-24 用户暂停动手，先出计划）。
设计决定与文件清单见同目录 `AGENTS.md`；编译/热更/部署的通用流程引用模块 01 / 02，不重复。

---

## Phase 0 — 协议与字体（地基，先落）

改动：两份 `lcd_shm.h`（+`LCD_CMD_BTN 0x49`、`+LCD_CMD_UI 0x4A`、`stat.reserved[0]`→`ui_event`）、
`lv_conf.h`（SIMSUN 打开）。

要点：
1. 两份 `lcd_shm.h` 改完 `diff` 必须无差异。
2. `ui_event` 放 stat 线（小核独占写）内 4B，单字写原子；小核写后 `flush_dcache_range` 该 64B 线。

**验收 / 测试流程**：
- [ ] 小核固件能编译过（字体增量不超应用区：产物仍是定长 1640640B，实际内容需 < 1638400B）；
- [ ] 字库覆盖检查：在 WSL 的 SDK 里 grep `lv_font_simsun_16_cjk` 的 unicode 列表，确认
      「智能监控/启动/摄像头/识别/正在/请稍候/待机/已停止/超时/重试」等字都在；缺哪个换哪个措辞
      （**在实施前完成，别等上板才看见方块**）。

## Phase 1 — 小核 UI 页 + 状态机（核心）

改动：`lvgl_task.c`（UI 构建、状态机、`cam_exit()`）、`comm_main.c`（BTN/UI 分发）、
新建 `lcd_touch.c/h` 骨架（indev 注册，`lcd_touch_read()` 恒返回未按下）。

UI 版式（320×240，深色）：
- 标题「智能监控」（SIMSUN16，字距加宽）+ 青色下划线细条 + 副标题 `MILK-V DUO 256M`（Montserrat14 灰）；
- 大按钮 ~230×62、圆角 31、垂直渐变（青蓝系）+ 半透明描边，按下态缩小+变暗；文案「启动摄像头识别」；
- 状态栏（SIMSUN16）：待机/正在启动，请稍候…/已停止/启动超时，请重试（红）；
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
- [ ] 上电：UI 页完整（标题/按钮/状态/提示），无方块字（缺字回 Phase 0 换措辞）；
- [ ] `lcd_sender cam on`（Phase 2 前可手动 `devmem`/临时命令发 0x49 代替）：按钮按下动画可见，
      spinner 转、状态「正在启动…」；
- [ ] 起画面链路后首帧到达 → UI 消失、画面满屏、OSD 仍在；
- [ ] 不起链路发 BTN → 25s 后红字「启动超时，请重试」回待机；
- [ ] `cam off`（或临时发 0x4A）：画面消失回 UI，「已停止」；
- [ ] 回归：探针 `[14]=0x484A000C`、`[2]` 在涨、黑屏待机时 `[16]=[17]=0`；再跑一次
      `lcd_sender ping` 与 bars 确认旧功能无损。

## Phase 2 — 大核入口（cam on/off + lcdcam start）

改动：`lcd_sender.c` 加 `cam` 子命令（用法注释同步）；`tools/lcdcam.sh` 加 `start`。

要点：
1. `cam on`：先 `cmd_send(LCD_CMD_BTN,1)` 再 `system("sh /root/lcdcam.sh start")`（屏上先有反馈）。
2. `cam off`：先 `system("sh /root/lcdcam.sh stop")` **等它返回**，再 `cmd_send(LCD_CMD_UI,0)`。
3. `start` 子命令：扫 /proc 只认 argv[1] 是 `/root/lcdcam.sh` 的实例，有就退出（防重复），
   没有就 `nohup sh "$0" >/dev/null 2>&1 &`；别对整条 cmdline 做 grep（历史事故见模块 02）。

**验收 / 测试流程**（部署按 `../02/PLAN.md` 一/三）：
- [ ] `lcd_sender cam on`：~1s 内屏上出按下动画，随后画面出现（全程 ≤ 10s）；
- [ ] 连打两次 `cam on`：不出现两个 lcdcam 循环（`ps` 核对）、屏不乱；
- [ ] `lcd_sender cam off`：画面退净、回 UI；`ps` 里 lcdcam/rtsp2lcd 已收干净；
- [ ] `cam on` → 拔掉摄像头链路（杀 sample_vi_fd）→ 自愈后画面恢复（不回归模块 03 的两级自愈）。

## Phase 3 — 端到端联调与回归（收口）

**测试流程**（按顺序全过才算完成）：
1. 冷启动 → UI 待机（黑底深色页 + OSD）。
2. `cam on` → 动画 → 启动中 → 画面（≤10s；超时路径单测过）。
3. 画面 60s：`[17]` +30/秒、无 ENOBUFS。
4. `cam off` → 回 UI「已停止」→ 再 `cam on` 循环 3 次（状态机不残留）。
5. 长跑 4 分钟（模块 03 三节判据全过）。
6. 重启板子：开机后仍是新固件 + UI 待机（`/mnt/data/cvirtos.bin` 已同步）。

## Phase 4 — 文档与副本收尾

- [ ] 板上 `/root/指南/` 写当日记录（命名按其 `readme.txt` 规范，类别大概率是「显示」），
      同名同步回 `sdcard/rootfs/root/指南/`；
- [ ] README 更新（新命令 `cam on/off`、新 build id 与探针基址、上电行为变为"UI 待机页"）；
- [ ] `sdcard/` 三处副本同步（cvirtos.bin×2、lcd_sender×2、lcdcam.sh×1）+ md5 比对；
- [ ] 本 PLAN 回填「实施结果」（偏差、实测数字、新探针基址），`AGENTS.md` 的 build id 表补
      `0x484A000C`；根 AGENTS 导航表状态改「稳定运行」；git 提交。

## 待确认清单

- [ ] P0：SIMSUN 字库是否覆盖「摄」等字（Phase 0 验证，缺则换措辞）。
- [ ] P1：启动超时 25s 的观感（实测后可调 15~30s）。
- [ ] P2：画面模式下的屏上「停止」按钮 → 挂到 `plans/06` Phase 4（等大核守护进程）。
- [ ] P2：人脸检测框上屏（先验证流里有没有框，见 `../03/PLAN.md` 待办）。
