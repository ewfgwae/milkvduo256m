# PLAN.md — 01 小核固件：改动流程与测试流程

写给下一次要改小核固件的会话。事实与规矩见同目录 `AGENTS.md`，本文件只给流程与判据。
当前板上跑的是 build `0x484A000C`（UI 按钮页，探针基址 `0x8FE82818`）。

---

## 一、改小核固件的标准流程（每次都照这个顺序，不要跳步）

1) **改源码**：只改 `src/sdk_overlay/freertos/…` 下对应文件；`lvgl_task.c` 的
   `LVGL_PROBE_BUILD_ID` +1。若改了 `lcd_shm.h`，`src/big_core/lcd_shm.h` 同步改（diff 两份必须一致）。

2) **同步 overlay 进 SDK 并编译**（WSL，root；板名是连字符形式）：

```bash
wsl -u root
bash ~/wb_sync_rtos.sh          # 等价: cd ~/duo-sdk-v2 && source build/envsetup_milkv.sh \
                                 #        milkv-duo256m-glibc-arm64-sd && cd build && make rtos
```

3) **★ 增量重编的坑**：`make rtos` 有时复用陈旧 `.obj`，改了没生效就删掉对应目标重编
   （以 `lvgl_task.c` 为例）：

```bash
cd ~/duo-sdk-v2/freertos/cvitek/build/task
rm -f lvgl/CMakeFiles/lvgl.dir/lvgl_task.c.obj
cmake --build . --target install && cmake --build . --target cvirtos.bin
```

4) **查新探针地址**（每版必做，地址随体积变）：

```bash
~/duo-sdk-v2/host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-nm \
    ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.elf | grep -E 'g_lvgl_probe|g_dma2_probe|g_spi2_probe'
```

5) **热更上板**（先收进程、清 8 槽，否则 ENOBUFS；报 ENOBUFS 后它会「看起来继续跑」但其实一个字节没写）：

```bash
ssh root@192.168.42.1 'killall lcd_sender 2>/dev/null; killall rtsp2lcd 2>/dev/null'
ssh root@192.168.42.1 'i=0; while [ $i -lt 8 ]; do devmem $((0x1900400+i*8)) 64 0; i=$((i+1)); done'
scp ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin root@192.168.42.1:/root/cvirtos.bin
ssh root@192.168.42.1 '/root/lcd_sender rtos /root/cvirtos.bin'
```

   判据（正常回显）：`写入 + 读回校验通过` → `热更完成: 新固件已运行 (state=1, ~200 ms)`，rc=0。

6) **验证**：按下面「二、回归测试流程」跑；至少核对 `[14] == 新 build id`、`[0]='LVGL'`。

7) **收尾三同步**（缺一不可，否则下次开机/换卡回旧版）：

```bash
ssh root@192.168.42.1 'mv -f /root/cvirtos.bin /mnt/data/cvirtos.bin; sync'   # 开机 auto.sh 热更源
# 仓库两份副本:
cp <产物> sdcard/rootfs/mnt/data/cvirtos.bin
cp <产物> sdcard/rootfs/root/cvirtos.bin
# 板上 /root/probe.sh 的 P= 基址改成第 4 步查到的新地址（或用 PROBE_BASE= 覆盖）
```

---

## 二、回归测试流程（探针判据表）

探针字段（`g_lvgl_probe[20]`，devmem 一次 32 位、地址步长 **8**）：

| 下标 | 含义 | 健康值 |
| --- | --- | --- |
| [0] | 'LVGL' 魔数 | 0x4C56474C |
| [1] | 0x600D = 初始化完成 | 0x600D |
| [2] | 主循环圈数 | 隔几秒读两次，**在涨** |
| [7] | 帧率 ×10 | 黑屏待机 <1；画面模式 ≈ 337（33.7fps） |
| [4] / [5] / [6] | 小核% / 大核% / 大核 heartbeat | 画面模式小核 ~30-60%；[6]=0 正常（mon 已摘） |
| [11] | 推屏像素/秒 | 画面模式 ≈ 2.61M（= SPI 时钟的 89%） |
| [14] | ★ build id | 必须等于本版期望值 |
| [15] | 'MONR' 魔数尾 | 0x4D4F4E52 |
| [16] | 画面模式标记 | 0=待机黑屏，1=画面已进入 |
| [17] | ★ 大核提交帧号 | **画面在动的唯一可信判据**，≈ +30/秒 |
| [18] | 小核已画帧数 | 在涨（但 cmdqu 打满时也涨，别单独信它） |
| [19] | 'CAMR' 魔数尾 | 0x43414D52 |

回归步骤：

1. **冷启动指纹**（上电或热更后）：屏 = 纯黑底 + 右下角 OSD；`[0]/[1]/[14]/[15]` 全对，
   `[16]=[17]=[18]=0`，`[11]` 几千像素/秒量级。
2. **通路**：`ssh root@192.168.42.1 '/root/lcd_sender ping'` 能打出 ctrl/stat 两行
   （`ctrl.magic` 应为 0x4C434431）。
3. **画面链路回归**（依赖模块 03）：`nohup sh /root/lcdcam.sh &` → 12 秒内 `[16]=1`、
   `[17]` 持续 +30/秒、`[7]≈337`。
4. **长跑**：`sh /root/longrun.sh 240`，判据：异常行 0、帧率稳定源帧率附近、无「流结束/断开」。

---

## 三、待办清单

- [ ] `plans/05` 按钮界面实施（build `0x484A000C`，新命令 BTN/UI）。
- [ ] `plans/06` 触摸驱动接入时，`driver/gpio` 需扩 `gpio_direction_input` / `gpio_get`
      （当前只有 output，读-改-写纪律见 `gpio.c` 注释）。
- [ ] 探针基址与 build id 的对照表随每版回填到 `AGENTS.md`（已有 08/0A/0B 三条）。
