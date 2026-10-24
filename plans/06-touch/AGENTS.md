# AGENTS.md — 06 触摸（驱动骨架 + 大核守护）★ 待实施，芯片型号未定

通用规范见根 `../../AGENTS.md`；按钮界面（触摸的挂载点）见 `../05-ui-button/`。

## 现状与背景

- 屏是 ST7789，**本体无触摸**；用户口径"屏其实带触摸"= 模块上额外有触摸芯片，**型号未确认**
  （2026-10-24 用户选择"先做其他部分"）。
- `plans/05` Phase 1 会先落好骨架：`lcd_touch.c/.h`（`lcd_touch_init()` / `lcd_touch_read(&x,&y)`）
  恒返回"未按下"，并注册成 LVGL `LV_INDEV_TYPE_POINTER`——**真驱动只需替换这一个文件的实现**，
  按钮的 LV_EVENT_CLICKED 自动生效。
- 小核→大核的"按钮被按了"事件走共享内存 `stat.ui_event` 计数（小核写、大核轮询，
  **不走 cmdqu**，避开 8 槽 ENOBUFS 坑）；LVGL 坐标系 320×240 横屏与触摸芯片的原始坐标
  之间可能要旋转/校准。

## 三个候选芯片的实现形态（确认型号后对号入座）

| 芯片 | 类型 | 需要的线 | 驱动形态 | 校准 |
| --- | --- | --- | --- | --- |
| XPT2046 | 电阻 | T_CLK/T_CS/T_DIN/T_DO/T_IRQ（5 根） | 小核 GPIO 位邦 SPI（时钟 ≤2.5MHz，位邦 ~1MHz 足够） | ★ 需要（raw→屏幕仿射，3/5 点校准流程见 PLAN） |
| GT911/GT9xx | 电容 I2C | SDA/SCL/INT/RST（4 根） | 小核 GPIO 位邦 I2C（400kHz 量级） | 基本免校准 |
| FT6236/FT6x06 | 电容 I2C | SDA/SCL/INT/RST（4 根） | 同上，寄存器更简单 | 基本免校准 |

## 硬件纪律

- 电平 3.3V（与屏一致）。
- 已占用 GP：GP6/GP7/GP9（SPI2）、GP16（BLK）、GP20（DC）、GP21（RES）；GP8 预留 SPI2_SDI。
  挑空闲 GP 前对照官方 40PIN 图，选定的脚位**回填到本文件**。
- `driver/gpio/src/gpio.c` 当前只有 `gpio_direction_output`（读-改-写纪律）：触摸需要先扩
  `gpio_direction_input` / `gpio_get`（必要时带上拉），照既有读-改-写风格写，别整寄存器覆盖。

## 大核守护进程（cam_btnd，本模块新增件）

- 作用：轮询 `/dev/mem` 映射的 `stat.ui_event`（200ms 周期），计数变化 → 执行
  `lcd_sender cam on/off` 语义（toggle）。这是「点按钮真的起/停摄像头」的闭环。
- ★ 绝不用 cmdqu 收事件（无 waiter 的小核→大核消息会占死槽位，历史上是 ENOBUFS 的来源之一）。
- 是否进开机自启（auto.sh）**待用户拍板**——进自启 = 上电即可触摸，但多一个常驻进程。

## 坑

- GT911 的 I2C 地址由复位瞬间 INT 电平决定（0x5D/0x14 两选一），位邦时序要照数据手册的
  复位-INT 采样窗口来，别只拉 RST。
- 电阻屏 XPT2046 的 pressure/z 轴要一起读，按下判定别只看坐标非零。
- 触摸坐标到 320×240 横屏的映射取决于屏排线安装方向，以实测为准，别先验写死。
