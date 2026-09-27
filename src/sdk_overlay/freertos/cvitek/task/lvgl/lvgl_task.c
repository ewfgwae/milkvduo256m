/* 小核(C906L)上的 LVGL 主任务 —— 屏幕的唯一所有者
 *
 * 屏幕内容: 跑 LVGL 自带的 lv_demo_benchmark(屏上动态跑分), 右下角叠一块 OSD,
 * 显示帧率和两个核的 CPU 占用率:
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
 *       -> lv_demo_benchmark() -> 建 OSD -> 循环 lv_timer_handler()。
 */
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "lvgl.h"
#include "demos/benchmark/lv_demo_benchmark.h"

#include "lcd_st7789.h"
#include "arch_helpers.h"       /* flush_dcache_range */
#include "arch_time.h"          /* GetSysTime() = rdtime */
#include "lv_port_tick.h"       /* lv_port_tick_us() */
#include "lvgl_task.h"

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
#define LVGL_PROBE_BUILD_ID     0x484A0008UL        /* 'HJ' + 序号 */
volatile unsigned long g_lvgl_probe[16];

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
	s_osd = lv_label_create(lv_layer_sys());       /* 系统层, 盖在 demo 上面 */
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

	lv_demo_benchmark();                            /* 屏上跑的动态 demo */
	osd_create();

	p[1] = 0x600D;                                  /* demo + OSD 建完的标记 */
	flush_dcache_range((uintptr_t)p, sizeof(g_lvgl_probe));

	for (;;) {
		uint32_t t0 = lv_port_tick_us();

		lv_timer_handler();
		s_perf_handler_us += lv_port_tick_us() - t0;   /* 埋点: 本秒 LVGL 处理耗时 */

		p[2]++;                                     /* 主循环圈数 */
		p[3] = (unsigned long)lv_tick_get();        /* 1ms 时钟(mtime) */
		p[4] = s_probe_small;                       /* 小核占用率 % */
		p[5] = s_probe_big;                         /* 大核占用率 % */
		p[6] = s_probe_hb;                          /* 大核 heartbeat */
		p[7] = s_probe_fps;                         /* 帧率 ×10 (一位小数) */
		p[8] = (unsigned long)g_lvgl_frame_cnt;     /* 累计整帧数 */
		flush_dcache_range((uintptr_t)p, sizeof(g_lvgl_probe));

		vTaskDelay(pdMS_TO_TICKS(LVGL_HANDLER_PERIOD_MS));
	}
}

void lvgl_task_start(void)
{
	xTaskCreate(prvLvglTask, "LVGL", LVGL_TASK_STACK_WORDS, NULL,
		    LVGL_TASK_PRIO, NULL);
}
