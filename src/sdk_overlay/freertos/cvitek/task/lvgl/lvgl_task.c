/* 小核(C906L)上的 LVGL 主任务 —— 屏幕的唯一所有者
 *
 * 屏幕内容: **不再跑 lv_demo_benchmark**(2026-10-03 去掉)。上电先是启动页
 * (黑底 + 转圈 + 「正在初始化...」, 见下面"启动页"一节), 大核就绪(发来
 * LCD_CMD_UI)后才换成"智能看家" UI 待机页(标题 + 「相机画面」大按钮 + 状态栏,
 * 详见"UI 页"一节), 右下角叠一块 OSD, 显示帧率和两个核的 CPU 占用率:
 *
 *   nn.n FPS       <- 帧率(一位小数)
 *   nn% A53        <- 大核占用率
 *   nn% C906       <- 小核占用率
 *
 *   小核占用率: 用 FreeRTOS 运行时间统计 —— 空闲任务累计运行时间 / 总时间,
 *               占用率 = 100 - 空闲率。计数源是 25MHz mtime 取 10us 分辨率,
 *               见下面的 ulPortGetRunTimeCounter()。
 *   大核占用率: 走 mailbox —— 大核的 `lcd_sender mon` 周期采样 /proc/stat,
 *               然后用 LCD_CMD_MON 把值发过来(见 lcd_shm.h), comm_main.c
 *               的 cmdqu 任务收到后调 lvgl_mon_set() 存下, OSD 直接读。
 *               小核这边不再碰共享内存的监控数据。
 *   帧率: lv_port_disp.c 里每刷完一整帧给 g_lvgl_frame_cnt 加一, 这里做差求速率。
 *         和大核 Linux 侧 lvgl_demo 的 OSD 一样用 ×10 定点保留一位小数。
 *         不能取整秒: LVGL 的定时器只在 lv_timer_handler() 的边界上触发, 相邻两次
 *         回调的实际间隔总是略大于 period, 于是 frames/秒 会算成 0。
 *
 * 流程: lcd_init() 上电 ST7789(SPI2) -> lv_init() -> 注册显示驱动
 *       -> 屏底刷黑 -> 建 OSD -> 建 UI 页(整体隐藏) -> 建启动页
 *       -> 循环 lv_timer_handler()。
 *       摄像头画面由大核的 LCD_CMD_CAM 触发(cam_enter, 首帧到达时 UI 页整个
 *       隐藏); LCD_CMD_UI 退画面回 UI 页(cam_exit)。
 */
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl.h"

#include "lcd_st7789.h"
#include "arch_helpers.h"       /* flush_dcache_range */
#include "arch_time.h"          /* GetSysTime() = rdtime */
#include "lv_port_tick.h"       /* lv_port_tick_us() */
#include "lvgl_task.h"
#include "lcd_touch.h"          /* 触摸骨架(plans/06): 芯片未定, 恒返回未按下 */

void lv_port_disp_init(void);   /* lv_port_disp.c */
extern volatile unsigned int g_lvgl_frame_cnt;   /* lv_port_disp.c: 已刷完的整帧数 */
extern volatile uint32_t g_perf_flush_us;        /* lv_port_disp.c: 送显埋点 */
extern volatile uint32_t g_perf_dirty_px;
extern volatile uint32_t g_perf_flush_calls;

/* sysdma_test.c: 小核 sysDMA 自检 (Step0 排雷, 结果在 g_dma_probe[16]) */
extern void sysdma_selftest(void);

#define LVGL_TASK_STACK_WORDS   (8192)              /* 32KB; lv_demo_widgets 比较吃栈 */
#define LVGL_TASK_PRIO          (tskIDLE_PRIORITY + 2)
#define LVGL_HANDLER_PERIOD_MS  (1)                 /* 1ms 跑一轮 = 1 tick(1000Hz)。
                                                     * 以前是 5ms(当时 200Hz 下 pdMS_TO_TICKS(4)
                                                     * 会被截成 1 tick = 5ms)。送显交给 DMA 后台搬
                                                     * 之后这段 vTaskDelay 是纯空转, 压到 1 tick
                                                     * 让下一轮的渲染更早开始, 与上一块送显重叠。 */
#define OSD_REFRESH_MS          (1000)              /* OSD 每秒刷新一次 */

/* 探针: 大核用 `devmem <g_lvgl_probe> 64` 起逐字读这 16 个 unsigned long。
 * (地址随固件体积变化, 用 `nm cvirtos.elf | grep g_lvgl_probe` 查)
 * [0]  'LVGL' 魔数头        [1]  0x600D = demo+OSD 建完
 * [2]  主循环圈数            [3]  lv_tick_get() (1ms, 由 mtime 来)
 * [4]  小核 CPU %            [5]  大核 CPU % (mailbox 送来)
 * [6]  大核 heartbeat        [7]  帧率 ×10 (一位小数)
 * [8]  累计整帧数            [9]  送显耗时 us/秒
 * [10] LVGL 处理耗时 us/秒   [11] 推给屏的像素数/秒
 * [12] flush 回调次数/秒    [13] 纯渲染耗时 us/秒 (= [10] - [9])
 * [14] 固件 build id        [15] 'MONR' 魔数尾
 *
 * [9][11][12] 就是判断瓶颈的依据: 有效送显字节率 = [11]*2 字节/[9] 微秒,
 * 拿它和 46.875MHz(=5.86 字节/微秒) 比 —— 接近说明卡在 SPI 时钟,
 * 差得远说明卡在 spi2_write 的逐字节轮询开销。 */
#define LVGL_PROBE_MAGIC_HEAD   0x4C56474CUL        /* 'LVGL' */
#define LVGL_PROBE_MAGIC_TAIL   0x4D4F4E52UL        /* 'MONR' */
/* 每改一版小核固件就把这里 +1: 免烧热更(见 hotjump.S / lcd_sender rtos)之后
 * 用 `devmem <g_lvgl_probe+0x70> 32` 一眼就能确认芯片上跑的到底是哪一版
 * (g_lvgl_probe 的地址随固件体积变化, 用 nm cvirtos.elf | grep g_lvgl_probe 查)。 */
#define LVGL_PROBE_BUILD_ID     0x484A000FUL        /* 'HJ' + 序号; 0D=超时 15s, 0E=画面模式「退出」按钮, 0F=上电启动页(Phase 6) */
volatile unsigned long g_lvgl_probe[20];

/* ==================== 运行时间统计的计数源 ============================
 * 25MHz mtime / 250 = 100kHz(10us 一格)。FreeRTOS 在每次任务切换时对
 * 这个值做差, 把时间记到当时正在运行的任务头上, 所以只要单调即可。 */
unsigned long ulPortGetRunTimeCounter(void)
{
	return GetSysTime() / 250u;
}

/* ==================== 两个占用率 ==================================== */

/* 小核: 空闲任务没在跑的时间就是被占用的时间。
 * 用滑窗 delta 而不是开机累计值, 这样数值会跟着负载走。 */
static uint32_t small_cpu_pct(void)
{
	static unsigned long prev_idle, prev_total;
	static int           started;
	unsigned long idle_now  = (unsigned long)ulTaskGetIdleRunTimeCounter();
	unsigned long total_now = ulPortGetRunTimeCounter();
	unsigned long di, dt;
	uint32_t pct;

	if (!started) {                         /* 第一次只做基准 */
		prev_idle  = idle_now;
		prev_total = total_now;
		started    = 1;
		return 0;
	}

	di = idle_now  - prev_idle;
	dt = total_now - prev_total;
	prev_idle  = idle_now;
	prev_total = total_now;

	if (dt == 0)
		return 0;
	if (di > dt)                            /* 计数异常时夹一下 */
		di = dt;
	pct = 100u - (uint32_t)((di * 100u) / dt);
	return pct > 100u ? 100u : pct;
}

/* ==================== mailbox 送来的大核监控量 ====================== *
 * comm_main.c 的 cmdqu 任务收到 LCD_CMD_MON 后调 lvgl_mon_set() 把值存这里;
 * OSD 用 lvgl_mon_get() 取。heartbeat 为 0 表示大核没在采样(或已经退出),
 * 此时 OSD 把大核那一栏显示成 n/a —— 大核 Ctrl-C 时会补发一条 hb=0 的。 */
static volatile uint32_t s_mon_big_pct, s_mon_hb;

void lvgl_mon_set(uint32_t big_pct, uint32_t heartbeat)
{
	s_mon_big_pct = big_pct > 100u ? 100u : big_pct;
	s_mon_hb      = heartbeat;
}

int lvgl_mon_get(uint32_t *big_pct, uint32_t *heartbeat)
{
	*big_pct   = s_mon_big_pct;
	*heartbeat = s_mon_hb;
	return s_mon_hb != 0u;
}

/* ==================== 右下角 OSD ==================================== */

static lv_obj_t *s_osd;
static uint32_t  s_osd_prev_tick;
static unsigned int s_osd_prev_frames;

/* 最近一次算出来的值, 给探针用 (fps 是 ×10 定点) */
static volatile uint32_t s_probe_fps, s_probe_small, s_probe_big, s_probe_hb;

/* 本秒累计: 花在 lv_timer_handler() 上的时间。送显是在 handler 内部被调用到的,
 * 所以这个数里也含送显, 减去 lv_port_disp.c 的 g_perf_flush_us 才是纯渲染。 */
static uint32_t s_perf_handler_us;

/* 回执给大核的数字: 小核占用率 + 帧率×10 (打包见 lcd_shm.h 的 LCD_MON_RX_*) */
void lvgl_mon_reply(uint32_t *small_pct, uint32_t *fps_x10)
{
	*small_pct = s_probe_small;
	*fps_x10   = s_probe_fps;
}

static void osd_timer_cb(lv_timer_t *timer)
{
	uint32_t now   = lv_tick_get();
	uint32_t dt    = now - s_osd_prev_tick;
	unsigned int frames = g_lvgl_frame_cnt - s_osd_prev_frames;
	uint32_t hb;
	uint32_t big;
	int fresh = lvgl_mon_get(&big, &hb);
	uint32_t sml = small_cpu_pct();
	unsigned int fps_x10 = dt ? (unsigned int)((unsigned long long)frames * 10000ull / dt) : 0;

	if (fresh)
		lv_label_set_text_fmt(s_osd, "%u.%u FPS\n%u%% A53\n%u%% C906",
				      fps_x10 / 10, fps_x10 % 10, (unsigned)big, (unsigned)sml);
	else                                    /* 大核没在发 LCD_CMD_MON */
		lv_label_set_text_fmt(s_osd, "%u.%u FPS\nn/a A53\n%u%% C906",
				      fps_x10 / 10, fps_x10 % 10, (unsigned)sml);

	s_osd_prev_tick   = now;
	s_osd_prev_frames = g_lvgl_frame_cnt;

	s_probe_fps   = fps_x10;
	s_probe_big   = big;
	s_probe_small = sml;
	s_probe_hb    = hb;

	/* ---- 结算上一秒的性能埋点, 然后把累计量清零 ---- */
	{
		volatile unsigned long *p = (volatile unsigned long *)g_lvgl_probe;
		uint32_t flush_us   = g_perf_flush_us;
		uint32_t handler_us = s_perf_handler_us;

		p[9]  = flush_us;                                   /* 送显耗时 us/秒 */
		p[10] = handler_us;                                 /* LVGL 处理 us/秒 */
		p[11] = g_perf_dirty_px;                            /* 像素数/秒 */
		p[12] = g_perf_flush_calls;                         /* flush 次数/秒 */
		p[13] = handler_us > flush_us ? handler_us - flush_us : 0;

		g_perf_flush_us    = 0;
		g_perf_dirty_px    = 0;
		g_perf_flush_calls = 0;
		s_perf_handler_us  = 0;
	}
	(void)timer;
}

static void osd_create(void)
{
	s_osd = lv_label_create(lv_layer_sys());       /* 系统层, 盖在画面上面 */
	lv_obj_set_style_bg_opa(s_osd, LV_OPA_60, 0);
	lv_obj_set_style_bg_color(s_osd, lv_color_black(), 0);
	lv_obj_set_style_text_color(s_osd, lv_color_white(), 0);
	lv_obj_set_style_pad_all(s_osd, 3, 0);
	lv_obj_set_style_text_align(s_osd, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_text(s_osd, "0.0 FPS\n--% A53\n--% C906");
	lv_obj_align(s_osd, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

	s_osd_prev_tick   = lv_tick_get();
	s_osd_prev_frames = g_lvgl_frame_cnt;

	lv_timer_create(osd_timer_cb, OSD_REFRESH_MS, NULL);
}

/* ==================== UI 页(按钮界面, plans/05) ===================== *
 * 上电默认页: 标题「智能看家」+ 大按钮「相机画面」+ 状态栏。整页挂在一个
 * 容器下, 进画面模式时 LV_OBJ_FLAG_HIDDEN 一次性隐藏(逐个藏对象容易漏)。
 *
 * 状态机(全部在本任务里跑, comm 侧只置标志 —— LVGL 不是线程安全的):
 *   上电 -> 启动页(转圈, 见"启动页"一节) --UI/BTN/首帧 CAM 任一--> 离页
 *   待机 --LCD_CMD_BTN--> 按下动画(180ms) --> 启动中(spinner, 15s 超时)
 *     |首帧 LCD_CMD_CAM 到来 ----> 画面模式(UI 隐藏)
 *     |超时 15s -----------------> 回待机, 红字「超时，再来一次」
 *   画面模式 --LCD_CMD_UI--> 回 UI 页, 「已停止」
 *   启动中   --LCD_CMD_UI--> 按取消处理, 直接回待机
 * BTN 在画面模式/启动中再来一律忽略(防 lcd_sender cam on 连打)。
 *
 * 中文全部走 SIMSUN16(lv_conf.h 里 LV_FONT_SIMSUN_16_CJK=1, 只覆盖 1412 个
 * 码点) —— 文案以 plans/05 AGENTS.md 的「措辞定稿」表为准, 改字前先验证在库。 */
enum ui_state {
	UI_STANDBY = 0,             /* 待机: 按钮可按 */
	UI_PRESSING,                /* 按下动画中 */
	UI_STARTING,                /* 启动中: spinner + 等首帧 */
};

static lv_obj_t      *s_ui_cont;      /* UI 页容器(进画面模式整体隐藏) */
static lv_obj_t      *s_ui_btn;
static lv_obj_t      *s_ui_status;
static lv_obj_t      *s_ui_spinner;
static enum ui_state  s_ui_state;
static uint32_t       s_ui_state_t0;  /* 进入当前状态的时刻(lv_tick_get, ms) */

/* 画面模式的 img 对象提前到这声明: UI 状态机要用它判断"是否已在画面模式"。
 * 其余画面模式的量仍在下面那一节。退出按钮同理: ui_state_poll 要让它出
 * 按下动画, s_exit_rel 记"按着待弹起"以免重复触发。 */
static lv_obj_t *s_cam_img;
static lv_obj_t *s_cam_exit_btn;    /* 画面模式右上角「退出」按钮 */
static int       s_exit_rel;        /* 1 = 按下中, 到点弹起 */
static uint32_t  s_exit_rel_t0;
static void cam_exit(void);            /* 定义在下面"画面模式"一节 */

/* comm 侧 lvgl_ui_btn_trigger()/lvgl_ui_return_trigger() 只置这两个标志,
 * 本任务主循环 poll(照 s_cam_req 的既有模式)。 */
static volatile int s_ui_btn_req;
static volatile int s_ui_ret_req;
static volatile int s_ui_exitbtn_req;

#define UI_PRESS_MS     180u    /* 按下动画时长 */
#define UI_START_TO_MS  15000u  /* 启动超时(2026-10-24 用户拍板 15s, 原 25s): 冷启动
                                 * 实测 ~13s 已近临界, 慢一点会先报超时 —— 但首帧
                                 * 到来时仍会自动进画面, 误报只是观感问题 */
#define UI_EXIT_REL_MS  300u    /* 「退出」按钮按下动画时长(纯反馈) */

void lvgl_ui_btn_trigger(void)    { s_ui_btn_req = 1; }
void lvgl_ui_return_trigger(void) { s_ui_ret_req = 1; }
void lvgl_ui_exitbtn_trigger(void){ s_ui_exitbtn_req = 1; }

static void ui_set_status(const char *txt, lv_color_t color)
{
	lv_label_set_text(s_ui_status, txt);
	lv_obj_set_style_text_color(s_ui_status, color, 0);
}

/* 回待机: 恢复按钮可用、收起 spinner; 状态文字由调用方随后设置 */
static void ui_back_standby(void)
{
	s_ui_state = UI_STANDBY;
	lv_obj_clear_state(s_ui_btn, LV_STATE_PRESSED);
	lv_obj_clear_state(s_ui_btn, LV_STATE_DISABLED);
	lv_obj_add_flag(s_ui_spinner, LV_OBJ_FLAG_HIDDEN);
}

static void ui_starting_enter(void)
{
	s_ui_state    = UI_STARTING;
	s_ui_state_t0 = lv_tick_get();
	lv_obj_add_state(s_ui_btn, LV_STATE_DISABLED);
	lv_obj_clear_flag(s_ui_spinner, LV_OBJ_FLAG_HIDDEN);
	ui_set_status("正在取像...", lv_color_hex(0xB0BEC5));
}

/* 按钮 CLICKED = 真触摸按了一下(触摸芯片接上后才会发生, plans/06)。
 * 这里**只**给 stat.ui_event 计数 +1: 起停链路由大核守护进程(cam_btnd)
 * 轮询该字段决定, 小核绝不在 LVGL 回调里碰 cmdqu(8 槽老坑)。
 * stat 线是小核独占写, 写完 flush 整条 64B(照 lcd_stat_publish 的纪律)。 */
static void ui_btn_event_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CLICKED &&
	    s_ui_state == UI_STANDBY && s_cam_img == NULL) {
		struct lcd_shm *s = (struct lcd_shm *)LCD_SHM_PHYS;

		s->stat.ui_event++;
		flush_dcache_range((uintptr_t)&s->stat, sizeof(s->stat));
	}
}

/* 画面模式「退出」按钮 CLICKED = 真触摸按了一下(触摸芯片接上后才会发生,
 * plans/06)。与主按钮同纪律: 只计 stat.ui_event, 停链路 + 回 UI 由大核
 * 守护进程做, 小核绝不在 LVGL 回调里碰 cmdqu。 */
static void cam_exit_btn_event_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_CLICKED && s_cam_img != NULL) {
		struct lcd_shm *s = (struct lcd_shm *)LCD_SHM_PHYS;

		s->stat.ui_event++;
		flush_dcache_range((uintptr_t)&s->stat, sizeof(s->stat));
	}
}

/* 触摸骨架的 indev 读回调: 坐标来自 lcd_touch.c(当前恒未按下) */
static void ui_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
	int x, y;

	if (lcd_touch_read(&x, &y)) {
		data->point.x = x;
		data->point.y = y;
		data->state   = LV_INDEV_STATE_PR;
	} else {
		data->state = LV_INDEV_STATE_REL;
	}
	(void)drv;
}

static void ui_create(void)
{
	lv_obj_t *cont, *title, *bar, *sub, *lbl, *hint;

	/* 容器: 占满整屏、全透明、不滚动 —— 只当"一藏全藏"的分组用 */
	cont = lv_obj_create(lv_scr_act());
	lv_obj_set_size(cont, LCD_W, LCD_H);
	lv_obj_center(cont);
	lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cont, 0, 0);
	lv_obj_set_style_pad_all(cont, 0, 0);
	lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
	lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

	/* 标题「智能看家」+ 青色下划线 + 副标题 */
	title = lv_label_create(cont);
	lv_obj_set_style_text_font(title, &lv_font_simsun_16_cjk, 0);
	lv_obj_set_style_text_color(title, lv_color_white(), 0);
	lv_obj_set_style_text_letter_space(title, 6, 0);
	lv_label_set_text(title, "智能看家");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

	bar = lv_obj_create(cont);
	lv_obj_set_size(bar, 100, 3);
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_set_style_bg_color(bar, lv_color_hex(0x00E5FF), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_align_to(bar, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

	sub = lv_label_create(cont);
	lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(sub, lv_palette_main(LV_PALETTE_GREY), 0);
	lv_obj_set_style_text_letter_space(sub, 1, 0);
	lv_label_set_text(sub, "MILK-V DUO 256M");
	lv_obj_align_to(sub, bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 7);

	/* 大按钮: 垂直渐变(青->蓝) + 半透明白描边; 按下态缩小一圈+变暗,
	 * 启动中 DISABLED 半透明防连击 */
	s_ui_btn = lv_btn_create(cont);
	lv_obj_set_size(s_ui_btn, 230, 62);
	lv_obj_align(s_ui_btn, LV_ALIGN_CENTER, 0, 8);
	lv_obj_set_style_radius(s_ui_btn, 31, 0);
	lv_obj_set_style_bg_color(s_ui_btn, lv_color_hex(0x00C2FF), 0);
	lv_obj_set_style_bg_grad_color(s_ui_btn, lv_color_hex(0x0069C0), 0);
	lv_obj_set_style_bg_grad_dir(s_ui_btn, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(s_ui_btn, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(s_ui_btn, lv_color_white(), 0);
	lv_obj_set_style_border_opa(s_ui_btn, LV_OPA_40, 0);
	lv_obj_set_style_border_width(s_ui_btn, 2, 0);
	lv_obj_set_style_shadow_width(s_ui_btn, 0, 0);
	lv_obj_set_style_transform_width(s_ui_btn, -6, LV_STATE_PRESSED);
	lv_obj_set_style_transform_height(s_ui_btn, -6, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(s_ui_btn, lv_color_hex(0x0077B6), LV_STATE_PRESSED);
	lv_obj_set_style_bg_grad_color(s_ui_btn, lv_color_hex(0x004E7A), LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(s_ui_btn, LV_OPA_60, LV_STATE_DISABLED);
	lv_obj_add_event_cb(s_ui_btn, ui_btn_event_cb, LV_EVENT_CLICKED, NULL);

	lbl = lv_label_create(s_ui_btn);
	lv_obj_set_style_text_font(lbl, &lv_font_simsun_16_cjk, 0);
	lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
	lv_label_set_text(lbl, "相机画面");
	lv_obj_center(lbl);

	/* 状态栏 */
	s_ui_status = lv_label_create(cont);
	lv_obj_set_style_text_font(s_ui_status, &lv_font_simsun_16_cjk, 0);
	lv_obj_align(s_ui_status, LV_ALIGN_TOP_MID, 0, 176);
	ui_set_status("待机", lv_color_hex(0xB0BEC5));

	/* 启动中 spinner: 盖在按钮中心, 待机时藏 */
	s_ui_spinner = lv_spinner_create(cont, 150, 90);
	lv_obj_set_size(s_ui_spinner, 36, 36);
	lv_obj_align(s_ui_spinner, LV_ALIGN_CENTER, 0, 8);
	lv_obj_set_style_border_width(s_ui_spinner, 0, LV_PART_MAIN);
	lv_obj_set_style_arc_width(s_ui_spinner, 4, LV_PART_MAIN);
	lv_obj_set_style_arc_color(s_ui_spinner, lv_color_hex(0x37474F), LV_PART_MAIN);
	lv_obj_set_style_arc_width(s_ui_spinner, 4, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(s_ui_spinner, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
	lv_obj_add_flag(s_ui_spinner, LV_OBJ_FLAG_HIDDEN);

	/* 左下角命令提示 */
	hint = lv_label_create(cont);
	lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
	lv_obj_set_style_text_color(hint, lv_color_hex(0x78909C), 0);
	lv_label_set_text(hint, "lcd_sender cam on / off");
	lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 6, -6);

	s_ui_cont  = cont;
	s_ui_state = UI_STANDBY;
}

/* ==================== 启动页(Phase 6) ================================ *
 * 上电默认页: 全屏黑底 + 56px 转圈 + 「正在初始化...」。大核(A53/Linux)没起来的
 * 整段时间屏上只有它 —— 完整按钮 UI 已照常建好但整体隐藏, 收到下面任一命令才
 * 揭开(boot_leave, 三个入口都调它):
 *   LCD_CMD_UI  auto.sh 热更成功后发 / lcd_sender ui / cam off 末步 -> 待机页
 *   LCD_CMD_BTN cam on 当开机首个命令 -> 先出 UI 页再播按下动画
 *   首帧 CAM    不 BTN 直接起 rtsp2lcd -> 直进画面(启动页不能残留盖在画面上)
 * MON / INIT / PING **故意不离页**: mon 是诊断工具且与 rtsp2lcd 互斥, 不在开机
 * 路径上; INIT 是 lcd_sender rtos 热更后的 state 轮询, 不能让它把启动页顶掉。
 * 无超时 —— 大核没起来就永远转圈, 这是需求语义本身。
 *
 * 文案「正在初始化...」: 「正在启动」的 启/动 都不在 SIMSUN16(2026-10-25 逐字
 * 预检), 已验证在库的备选: 正在等待... / 等待中...。 */
static lv_obj_t *s_boot_cont;

static void boot_create(void)
{
	lv_obj_t *cont, *spin, *lbl;

	/* 全屏不透明黑底容器, 建在 UI 页之后 = 盖在 UI 页之上(sys 层 OSD 之下) */
	cont = lv_obj_create(lv_scr_act());
	lv_obj_set_size(cont, LCD_W, LCD_H);
	lv_obj_center(cont);
	lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(cont, 0, 0);
	lv_obj_set_style_pad_all(cont, 0, 0);
	lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
	lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

	/* 转圈: 「启动中」那只 spinner 的放大版, 同一套青色系 */
	spin = lv_spinner_create(cont, 150, 90);
	lv_obj_set_size(spin, 56, 56);
	lv_obj_align(spin, LV_ALIGN_CENTER, 0, -14);
	lv_obj_set_style_border_width(spin, 0, LV_PART_MAIN);
	lv_obj_set_style_arc_width(spin, 6, LV_PART_MAIN);
	lv_obj_set_style_arc_color(spin, lv_color_hex(0x37474F), LV_PART_MAIN);
	lv_obj_set_style_arc_width(spin, 6, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(spin, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);

	lbl = lv_label_create(cont);
	lv_obj_set_style_text_font(lbl, &lv_font_simsun_16_cjk, 0);
	lv_obj_set_style_text_color(lbl, lv_color_hex(0xB0BEC5), 0);
	lv_obj_set_style_text_letter_space(lbl, 2, 0);
	lv_label_set_text(lbl, "正在初始化...");
	lv_obj_align_to(lbl, spin, LV_ALIGN_OUT_BOTTOM_MID, 0, 14);

	s_boot_cont = cont;
}

/* 幂等: 启动页还在时才动手 —— 删容器、揭开按钮 UI 页 */
static void boot_leave(void)
{
	if (s_boot_cont != NULL) {
		lv_obj_del(s_boot_cont);
		s_boot_cont = NULL;
		lv_obj_clear_flag(s_ui_cont, LV_OBJ_FLAG_HIDDEN);
	}
}

/* 状态机推进: 每轮主循环在 lv_timer_handler() 之前跑(改对象只管改,
 * 真正重画在 handler 里发生), 先处理画面 poll 再处理这里 —— 首帧 CAM
 * 抢先把 UI 藏掉的话, 同轮后到的 BTN 会被正确忽略。 */
static void ui_state_poll(void)
{
	uint32_t now = lv_tick_get();

	/* LCD_CMD_UI: 退画面回 UI 页; 启动中收到 = 大核放弃启动, 按取消处理;
	 * 启动页状态下收到 = 大核就绪, 揭开按钮页(状态保持「待机」—— cam 没跑过) */
	if (s_ui_ret_req) {
		s_ui_ret_req = 0;
		boot_leave();
		if (s_cam_img != NULL) {
			cam_exit();              /* 内部把状态收干净, 显示"已停止" */
		} else if (s_ui_state != UI_STANDBY) {
			ui_back_standby();
			ui_set_status("待机", lv_color_hex(0xB0BEC5));
		}
	}

	/* LCD_CMD_BTN: 触发按下动画; 画面模式/启动中/动画中来的一律忽略;
	 * 启动页状态下收到(cam on 当开机首个命令) = 先出 UI 页再播动画 */
	if (s_ui_btn_req) {
		s_ui_btn_req = 0;
		if (s_cam_img == NULL && s_ui_state == UI_STANDBY) {
			boot_leave();
			lv_obj_add_state(s_ui_btn, LV_STATE_PRESSED);
			s_ui_state    = UI_PRESSING;
			s_ui_state_t0 = now;
		}
	}

	/* LCD_CMD_EXIT_BTN: 画面模式「退出」按钮出按下动画(纯视觉, 不退画面
	 * —— 停不停链路由大核决定, 这里只给反馈); 按下中/非画面模式忽略 */
	if (s_ui_exitbtn_req) {
		s_ui_exitbtn_req = 0;
		if (s_cam_img != NULL && s_exit_rel == 0) {
			lv_obj_add_state(s_cam_exit_btn, LV_STATE_PRESSED);
			s_exit_rel    = 1;
			s_exit_rel_t0 = now;
		}
	}
	if (s_exit_rel && now - s_exit_rel_t0 >= UI_EXIT_REL_MS) {
		lv_obj_clear_state(s_cam_exit_btn, LV_STATE_PRESSED);
		s_exit_rel = 0;
	}

	switch (s_ui_state) {
	case UI_PRESSING:
		if (now - s_ui_state_t0 >= UI_PRESS_MS) {
			lv_obj_clear_state(s_ui_btn, LV_STATE_PRESSED);
			ui_starting_enter();         /* 内部记新的 t0 */
		}
		break;
	case UI_STARTING:
		if (now - s_ui_state_t0 >= UI_START_TO_MS) {
			ui_back_standby();
			ui_set_status("超时，再来一次", lv_palette_main(LV_PALETTE_RED));
		}
		break;
	default:
		break;
	}
}

/* ==================== 画面模式(摄像头) ============================== *
 * 大核: 把一帧 RGB565(大端) 写进共享内存 fb 区 -> 发 LCD_CMD_CAM。
 * comm_main.c 的 cmdqu 任务收到后只调 lvgl_cam_submit() "记一笔", **不碰 LVGL**:
 * LVGL 不是线程安全的, 所有渲染都必须在下面这个任务里做。于是分工是——
 * cmdqu 侧只写 s_cam_req/s_cam_seq(单字写, 天然原子), 本任务每轮开头 poll 一次。 */
static lv_img_dsc_t     s_cam_dsc;
static volatile int     s_cam_req;      /* 1 = 收到过 LCD_CMD_CAM */
static volatile uint32_t s_cam_seq;     /* 大核提交的帧号(统计用) */
static volatile uint32_t s_cam_done;    /* 本任务已处理的帧数(统计用) */

void lvgl_cam_submit(void)
{
	s_cam_req = 1;
	s_cam_seq++;
}

static void cam_enter(void)
{
	lv_obj_t *scr;
	lv_obj_t *lbl;

	/* -1) 启动页下首帧直接到达(没经过 BTN, 直接起了 rtsp2lcd): 先撕启动页,
	 *     否则它会盖在画面之上(画面对象是后建的, z 序在启动页上面, 但
	 *     OSD 之下还压着一块全屏黑底, 必须删掉)。 */
	boot_leave();

	/* 0) 首帧到达 = 切画面: UI 页整个藏掉, 状态机收干净(给 cam_exit 一个
	 *    干净起点; 若是从"启动中"直切画面, spinner 也就是在这收的)。 */
	lv_obj_add_flag(s_ui_cont, LV_OBJ_FLAG_HIDDEN);
	ui_back_standby();

	/* 1) 屏底纯黑: 没有画面的地方是黑的, 不再是红/绿/蓝。
	 *    (init 时已经设过一次, 这里再设一遍是为了防止主题改动把它覆盖) */
	scr = lv_scr_act();
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	/* 2) lv_img 直接引用共享内存那块 RGB565, 一个字节都不拷贝。
	 *    LV_COLOR_DEPTH=16 且 LV_COLOR_16_SWAP=1, 而 fb 里存的正是"高字节在
	 *    前"的 RGB565 —— 与 ST7789 要的字节序、与 LVGL 的 lv_color_t 布局
	 *    三者一致, 所以能直接当 LV_IMG_CF_TRUE_COLOR 用。 */
	s_cam_dsc.header.always_zero = 0;
	s_cam_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
	s_cam_dsc.header.w           = LCD_W;
	s_cam_dsc.header.h           = LCD_H;
	s_cam_dsc.data_size          = (uint32_t)LCD_W * LCD_H * 2u;
	s_cam_dsc.data               = (const uint8_t *)(LCD_SHM_PHYS + LCD_SHM_FB_OFF);

	s_cam_img = lv_img_create(scr);
	lv_img_set_src(s_cam_img, &s_cam_dsc);
	lv_obj_center(s_cam_img);

	/* 画面模式右上角「退出」按钮(用户 2026-10-24 拍板): 半透明深底 + 红描边,
	 * 在 img 之后创建 = 画在画面之上、sys 层 OSD 之下。屏没触摸, 指令
	 * LCD_CMD_EXIT_BTN(cam off 第一步)让它出按下动画; 真触摸 CLICKED 只计
	 * stat.ui_event, 由大核守护进程(plans/06)接管。 */
	s_cam_exit_btn = lv_btn_create(scr);
	lv_obj_set_size(s_cam_exit_btn, 88, 34);
	lv_obj_align(s_cam_exit_btn, LV_ALIGN_TOP_RIGHT, -8, 8);
	lv_obj_set_style_radius(s_cam_exit_btn, 17, 0);
	lv_obj_set_style_bg_color(s_cam_exit_btn, lv_color_hex(0x263238), 0);
	lv_obj_set_style_bg_opa(s_cam_exit_btn, LV_OPA_70, 0);
	lv_obj_set_style_border_color(s_cam_exit_btn, lv_palette_main(LV_PALETTE_RED), 0);
	lv_obj_set_style_border_width(s_cam_exit_btn, 2, 0);
	lv_obj_set_style_shadow_width(s_cam_exit_btn, 0, 0);
	lv_obj_set_style_transform_width(s_cam_exit_btn, -3, LV_STATE_PRESSED);
	lv_obj_set_style_transform_height(s_cam_exit_btn, -3, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(s_cam_exit_btn, lv_color_hex(0x8E0000), LV_STATE_PRESSED);
	lv_obj_add_event_cb(s_cam_exit_btn, cam_exit_btn_event_cb, LV_EVENT_CLICKED, NULL);

	lbl = lv_label_create(s_cam_exit_btn);
	lv_obj_set_style_text_font(lbl, &lv_font_simsun_16_cjk, 0);
	lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
	lv_label_set_text(lbl, "退出");
	lv_obj_center(lbl);
}

/* 大核 LCD_CMD_UI: 退出画面模式, 删画面对象, 回 UI 页显示"已停止"。
 * 大核侧的纪律是先把链路停干净(lcdcam.sh stop)再发 UI —— 顺序反了的话
 * 下一帧 CAM 又会切回画面模式(不会崩, 但两边状态对不上)。 */
static void cam_exit(void)
{
	if (s_cam_img != NULL) {
		lv_obj_del(s_cam_img);
		s_cam_img = NULL;
	}
	if (s_cam_exit_btn != NULL) {   /* 连按下状态一起收掉, 防 poll 弹起时悬空 */
		lv_obj_del(s_cam_exit_btn);
		s_cam_exit_btn = NULL;
	}
	s_exit_rel = 0;
	s_cam_req = 0;              /* 不清的话下一轮 poll 立刻又 cam_enter */

	lv_obj_clear_flag(s_ui_cont, LV_OBJ_FLAG_HIDDEN);
	ui_back_standby();
	ui_set_status("已停止", lv_color_hex(0xB0BEC5));
}

static void lvgl_cam_poll(void)
{
	if (!s_cam_req)
		return;

	if (s_cam_img == NULL)
		cam_enter();

	/* 大核刚写进去的新像素在 DRAM 里, 本核 cache 里可能还留着上一帧的旧行。
	 * 送显前整块作废一次即可: 之后 LVGL 读到的要么是刚作废后重新取的新数据,
	 * 要么是从 DRAM 新取的(这块内存只有大核写, 小核不会把它弄脏)。 */
	inv_dcache_range((uintptr_t)(LCD_SHM_PHYS + LCD_SHM_FB_OFF),
			 (size_t)LCD_W * LCD_H * 2u);
	lv_obj_invalidate(s_cam_img);
	s_cam_done++;
}

/* ==================== 任务 ========================================== */

static void prvLvglTask(void *pvParameters)
{
	volatile unsigned long *p = (volatile unsigned long *)g_lvgl_probe;

	(void)pvParameters;

	p[0]  = LVGL_PROBE_MAGIC_HEAD;
	p[14] = LVGL_PROBE_BUILD_ID;
	p[15] = LVGL_PROBE_MAGIC_TAIL;
	flush_dcache_range((uintptr_t)p, sizeof(g_lvgl_probe));

	lcd_init();                                     /* ST7789 复位时序 + SPI2 初始化 */

	sysdma_selftest();                              /* Step0 排雷: sysDMA 内存到内存搬运 */

	lv_init();
	lv_port_disp_init();

	/* 本固件不带 demo: 上电就是黑屏。LVGL 默认(system, 亮色主题)会把当前
	 * screen 刷成白色, 所以这里必须显式改黑 —— 只有右下角的 OSD 看得见。 */
	{
		lv_obj_t *scr = lv_scr_act();

		lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	}

	osd_create();

	ui_create();                                /* plans/05: 按钮界面待机页 */

	/* Phase 6: 上电默认只显启动页 —— UI 页建好即整体藏起, boot_leave() 揭开 */
	lv_obj_add_flag(s_ui_cont, LV_OBJ_FLAG_HIDDEN);
	boot_create();

	/* 触摸骨架(plans/06 芯片定型号前的占位): 注册指针型 indev, 读回调
	 * 恒返回未按下 —— 按钮 CLICKED 链路先就位, 真驱动只改 lcd_touch.c。 */
	{
		static lv_indev_drv_t drv;

		lcd_touch_init();
		lv_indev_drv_init(&drv);
		drv.type    = LV_INDEV_TYPE_POINTER;
		drv.read_cb = ui_touch_read_cb;
		lv_indev_drv_register(&drv);
	}

	p[1] = 0x600D;                                  /* UI 页 + OSD 建完的标记 */
	flush_dcache_range((uintptr_t)p, sizeof(g_lvgl_probe));

	for (;;) {
		uint32_t t0;

		/* 画面模式: 必须排在 lv_timer_handler() 之前 —— 这里只做"作废 cache
		 * + 把 img 标脏", 真正的重画发生在 handler 内部。
		 * UI 状态机紧随其后(BTN/UI 标志的处理见 ui_state_poll)。 */
		lvgl_cam_poll();
		ui_state_poll();

		t0 = lv_port_tick_us();
		lv_timer_handler();
		s_perf_handler_us += lv_port_tick_us() - t0;   /* 埋点: 本秒 LVGL 处理耗时 */

		p[2]++;                                     /* 主循环圈数 */
		p[3] = (unsigned long)lv_tick_get();        /* 1ms 时钟(mtime) */
		p[4] = s_probe_small;                       /* 小核占用率 % */
		p[5] = s_probe_big;                         /* 大核占用率 % */
		p[6] = s_probe_hb;                          /* 大核 heartbeat */
		p[7] = s_probe_fps;                         /* 帧率 ×10 (一位小数) */
		p[8] = (unsigned long)g_lvgl_frame_cnt;     /* 累计整帧数 */
		p[16] = (s_cam_img != NULL) ? 1ul : 0ul;    /* 画面模式标记 */
		p[17] = (unsigned long)s_cam_seq;           /* 大核提交的帧号 */
		p[18] = (unsigned long)s_cam_done;          /* 已处理的画面帧数 */
		p[19] = 0x43414D52UL;                       /* 'CAMR' 魔数尾 */
		flush_dcache_range((uintptr_t)p, sizeof(g_lvgl_probe));

		vTaskDelay(pdMS_TO_TICKS(LVGL_HANDLER_PERIOD_MS));
	}
}

void lvgl_task_start(void)
{
	xTaskCreate(prvLvglTask, "LVGL", LVGL_TASK_STACK_WORDS, NULL,
		    LVGL_TASK_PRIO, NULL);
}
