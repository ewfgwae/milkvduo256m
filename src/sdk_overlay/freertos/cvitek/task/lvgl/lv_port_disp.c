/* 小核(C906L)版 LVGL 显示驱动 —— 半屏双缓冲 + 异步(DMA)送显
 *
 * flush 不碰 SPI 寄存器, 只调 lcd_st7789.c 的两个入口:
 *   lcd_blit_wait()   等上一块从 SPI 里彻底出去
 *   lcd_blit_start()  开窗 + 把像素交给 sysDMA 后立即返回
 * 颜色: lv_conf.h 里开了 LV_COLOR_16_SWAP 1, 于是 LVGL 缓冲里两个字节的先后
 * 就是 ST7789 要的"高字节在前", 可以把缓冲当字节流整块发出去, 不必逐像素换序。
 *
 * ---- 为什么是"半屏"双缓冲, 而不是整屏(LVGL 8.3.11 的实现决定) ----
 * lv_refr.c 里有两条等待路径, 语义完全不同:
 *   ① 单缓冲, 或"两块都按整屏尺寸开"(draw_buf->size == hor_res*ver_res,
 *      代码里叫 full_sized): 等待在【渲染之前】(refr_area_part 开头) —— 画面
 *      必须等上一帧刷完才开画, 串行。
 *   ② 双缓冲且每块比屏幕小: 等待在【送显之前】(draw_buf_flush) —— 渲染下一块
 *      时上一块还在搬, 这才是真重叠。
 * 而且按整屏开就得配 full_refresh = 1, 于是每帧固定推 153600 字节; 实测脏区
 * 平均只占 ~40% 屏(每帧约 61KB), 整屏刷反而让帧率从 ~57 掉到 ~30。
 *
 * 所以这里取两块 240x160(各 76800 B): 320 行正好切 2 块, 单次送显量够大、
 * 切块数又少; 两块合计 153600 B 落在跳板后的空闲区(见 cv181x_lscript.ld 的
 * lvgl_fb_MEM_0), 完全不动应用区/LVGL 池/FreeRTOS 堆栈。
 *
 * ---- 缓冲区安全性 ----
 * flush_cb 返回时像素还在搬, LVGL 却已经可以继续了, 靠两点保证不打架:
 *   1. draw_buf_flush() 调完 flush_cb 一定会把 buf_act 切到另一块, 所以下一块
 *      绝不可能写进"正被 DMA 读"的那块缓冲;
 *   2. SPI2 只有一个 TX FIFO, 所以下一块开窗前一定先 lcd_blit_wait(),
 *      保证同一时刻只有一趟 DMA 在飞。
 */
#include "lvgl.h"

#include "lcd_st7789.h"
#include "lv_port_tick.h"       /* lv_port_tick_us() */

/* 半屏: 240 x 160 = 38400 像素 = 76800 字节。两块合计 153600 字节。 */
#define LVGL_DRAW_LINES     160
static lv_color_t s_draw_buf1[LCD_W * LVGL_DRAW_LINES] __attribute__((section(".lvgl_fb")));
static lv_color_t s_draw_buf2[LCD_W * LVGL_DRAW_LINES] __attribute__((section(".lvgl_fb")));

/* 已刷完的整帧计数, 给右下角 OSD 算 FPS 用。
 * LVGL 会把一帧拆成多次 flush_cb 调用, 只有最后一块才算一帧刷完;
 * 必须在 lv_disp_flush_ready() 之前问 lv_disp_flush_is_last() —— 那个调用会把标志清掉。 */
volatile unsigned int g_lvgl_frame_cnt;

/* ---- 送显埋点 --------------------------------------------------------
 * 改成异步送显之后, 像素搬运由 sysDMA 在后台做, CPU 只在"等上一块"和"发起
 * 本块"上被占住。所以 g_perf_flush_us 量的就是这两段的和 —— 也就等于"这一秒
 * 里 CPU 被送显占用了多久", 拿它和 g_perf_handler_us(整个 lv_timer_handler)
 * 相减仍然是纯渲染时间, 埋点的含义没变。
 * lvgl_task.c 的 OSD 定时器每秒读走并清零, 结果放到 g_lvgl_probe[9..12]。 */
volatile uint32_t g_perf_flush_us;      /* 本秒 CPU 被送显占住的时间 */
volatile uint32_t g_perf_dirty_px;      /* 本秒推给屏的像素数 */
volatile uint32_t g_perf_flush_calls;   /* 本秒 flush_cb 被调用次数(= 脏矩形数) */

static void lv_port_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
	int last = lv_disp_flush_is_last(drv);
	uint32_t t0 = lv_port_tick_us();
	uint32_t pixels = (uint32_t)(area->x2 - area->x1 + 1) *
			  (uint32_t)(area->y2 - area->y1 + 1);

	/* 先给上一块收尾: SPI2 的 TX FIFO 只有一个, 上一趟没搬完就改窗口,
	 * 命令会插进还在流的像素里。空闲时这一步是空操作。 */
	lcd_blit_wait();

	/* 开窗 + 起 DMA 后立刻返回: 本函数一返回, draw_buf_flush() 就把 buf_act
	 * 切到另一块, LVGL 马上去渲染下一块 —— 于是"渲染"与"送显"在时间上重叠。 */
	lcd_blit_start((uint16_t)area->x1, (uint16_t)area->y1,
		       (uint16_t)area->x2, (uint16_t)area->y2,
		       (const uint8_t *)px);

	g_perf_flush_us   += lv_port_tick_us() - t0;
	g_perf_dirty_px   += pixels;
	g_perf_flush_calls++;

	if (last)
		g_lvgl_frame_cnt++;

	lv_disp_flush_ready(drv);
}

void lv_port_disp_init(void)
{
	static lv_disp_draw_buf_t draw_buf;
	static lv_disp_drv_t disp_drv;

	/* 传两块 -> 走 lv_refr.c 的"双缓冲且非整屏"路径, 前后两块交替使用 */
	lv_disp_draw_buf_init(&draw_buf, s_draw_buf1, s_draw_buf2, LCD_W * LVGL_DRAW_LINES);

	lv_disp_drv_init(&disp_drv);
	disp_drv.hor_res      = LCD_W;
	disp_drv.ver_res      = LCD_H;
	disp_drv.flush_cb     = lv_port_flush;
	disp_drv.draw_buf     = &draw_buf;
	disp_drv.full_refresh = 0;          /* 0 = 局刷, flush_cb 只拿脏区 */

	lv_disp_drv_register(&disp_drv);
}
