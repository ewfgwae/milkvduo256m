/* 小核(C906L)侧 ST7789 刷屏
 *
 * 逻辑与 Linux 侧 TFT-ST7789/st7789/st7789.c + lvgl_port/main.c 保持一致:
 * 同一套引脚、同一套初始化表、同样先开窗口(0x2A/0x2B/0x2C)再灌像素。
 * 区别只在于: 这里不用 spidev/wiringX, 直接操作 SPI2 与 GPIOA 寄存器。
 */
#include "lcd_st7789.h"
#include "lcd_shm.h"
#include "spi.h"
#include "gpio.h"
#include "delay.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "arch_helpers.h"       /* flush_dcache_range / inv_dcache_range */

/* Duo256M 40PIN 上的接线, 与 Linux 侧一致:
 *   DC  = GP20 = XGPIOA[27]
 *   RST = GP21 = XGPIOA[26]
 *   BLK = GP16 = XGPIOA[23]
 * SCK/SDO/CS 三根线由 SPI2 控制器硬件驱动, 代码不碰
 * (CS 靠 SER 位自动片选, 所以根本不需要占一根 GPIO)。
 * gpio 编码 = (端口字母 << 8) | 引脚号, 见 driver/gpio/src/gpio.c */
#define LCD_PIN_DC      ((0xA << 8) | 27)
#define LCD_PIN_RST     ((0xA << 8) | 26)
#define LCD_PIN_BL      ((0xA << 8) | 23)

#define LCD_DC_CMD()    gpio_direction_output(LCD_PIN_DC, 0)
#define LCD_DC_DATA()   gpio_direction_output(LCD_PIN_DC, 1)

/* ---------- 屏方向: 320x240 横屏 ----------
 * ST7789 的显存天生是 240(列) x 320(行) 竖屏。要当 320x240 横屏用, 必须
 * 在 MADCTL(0x36) 里打开 MV 位 —— MV=1 时行列互换, 于是"列"变成 320、"行"
 * 变成 240, 与 lcd_shm.h 的 LCD_W=320/LCD_H=240 对齐。
 * 实测(本屏, 2026-10-03 板上验证):
 *   0x60 = MX|MV  -> 横屏, 上下方向正常
 *   0xA0 = MY|MV  -> 横屏, 画面上下颠倒
 *   若 0x60 装出来左右镜像, 改 0x20(MV 单开) 即可。
 * 开窗偏移: 本屏可见区从 (0,0) 开始, 所以偏移都是 0; 若换到带边框的屏
 * (常见 240x320 面板 X 偏 0 / Y 偏 80), 改这两个宏就行, 不用动别处。 */
#define LCD_MADCTL      0x60
#define LCD_X_OFF       0
#define LCD_Y_OFF       0

/* 大核用 devmem 读这 16 个 unsigned long, 就能知道小核跑到哪一步了 */
#define LCD_PROBE_MAGIC_HEAD    0x5A5A1234UL
#define LCD_PROBE_MAGIC_TAIL    0xA5A5C3C3UL
volatile unsigned long g_lcd_probe[16];

static void lcd_cmd(uint8_t c)
{
	LCD_DC_CMD();
	spi2_write(&c, 1);
}

/* 超过这个长度的数据块走 DMA(像素), 短包(命令参数)走 PIO:
 * DMA 每次都要重建 LLI 链, 几十字节以下不划算。 */
#define LCD_DMA_MIN     32u

static void lcd_data(const uint8_t *d, uint32_t n)
{
	LCD_DC_DATA();
	if (n >= LCD_DMA_MIN)
		spi2_write_dma(d, n);
	else
		spi2_write(d, n);
}

static void lcd_cmd_data(uint8_t c, const uint8_t *d, uint32_t n)
{
	lcd_cmd(c);
	if (n)
		lcd_data(d, n);
}

void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	uint8_t a[4];

	/* 上一块像素可能还在由 sysDMA 往 TX FIFO 里灌。开窗命令是走 PIO 写进
	 * 同一个 FIFO 的, 不等它搬完就发会把窗口命令插进像素流里, 画面直接错位。
	 * 空闲时这个调用是空操作。 */
	lcd_blit_wait();

	/* 横屏下"逻辑坐标"与"面板物理坐标"差一个偏移(见 LCD_X_OFF/LCD_Y_OFF) */
	x0 += LCD_X_OFF; x1 += LCD_X_OFF;
	y0 += LCD_Y_OFF; y1 += LCD_Y_OFF;

	lcd_cmd(0x2A);                  /* Column address set */
	a[0] = x0 >> 8; a[1] = x0 & 0xFF;
	a[2] = x1 >> 8; a[3] = x1 & 0xFF;
	lcd_data(a, 4);

	lcd_cmd(0x2B);                  /* Row address set */
	a[0] = y0 >> 8; a[1] = y0 & 0xFF;
	a[2] = y1 >> 8; a[3] = y1 & 0xFF;
	lcd_data(a, 4);

	lcd_cmd(0x2C);                  /* Memory write */
}

void lcd_fill(uint16_t color)
{
	static uint8_t line[LCD_W * 2];
	uint32_t i;

	for (i = 0; i < LCD_W; i++) {
		line[i * 2]     = color >> 8;
		line[i * 2 + 1] = color & 0xFF;
	}

	lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
	for (i = 0; i < LCD_H; i++)
		lcd_data(line, LCD_W * 2);
}

void lcd_blit(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const uint8_t *rgb565_be)
{
	lcd_set_window(x0, y0, x1, y1);
	lcd_data(rgb565_be, (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1) * 2);
}

/* 异步送显: 开窗 + 起 DMA 后立刻返回, 像素由 sysDMA 在后台搬。
 * 与 lcd_blit() 的关系: lcd_blit() = lcd_blit_wait() + 开窗 + 阻塞搬完。 */
void lcd_blit_wait(void)
{
	spi2_write_dma_wait();
}

void lcd_blit_start(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
		    const uint8_t *rgb565_be)
{
	uint32_t n = (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1) * 2u;

	/* lcd_set_window 内部会先等上一块搬完(见那里的注释), 这一步不能省 */
	lcd_set_window(x0, y0, x1, y1);

	LCD_DC_DATA();
	if (n >= LCD_DMA_MIN)
		spi2_write_dma_start(rgb565_be, n);
	else
		spi2_write(rgb565_be, n);   /* 太短: DMA 建链的固定开销不划算 */
}

void lcd_backlight(int on)
{
	gpio_direction_output(LCD_PIN_BL, on ? 1 : 0);
}

void lcd_init(void)
{
	/* SPI2: 8 位帧 / 模式 0 / 46.875MHz, 片选常有效 */
	spi2_init(CVI_SPI2_BAUDR_46875K);
	spi2_cs(1);

	gpio_direction_output(LCD_PIN_BL, 0);
	gpio_direction_output(LCD_PIN_DC, 0);
	gpio_direction_output(LCD_PIN_RST, 0);

	/* 复位时序, 与 Linux 侧一致: 拉低 1s -> 拉高 1s */
	mdelay(1000);
	gpio_direction_output(LCD_PIN_RST, 1);
	mdelay(1000);

	lcd_cmd_data(0x11, NULL, 0);                                    /* Sleep Out */
	mdelay(120);

	/* ---- 下面这张表与 Linux 侧 st7789.c 的 TFT_init() 逐条对应 ---- */
	lcd_cmd_data(0x3A, (const uint8_t[]){0x05}, 1);                 /* 65k mode */
	lcd_cmd_data(0xC5, (const uint8_t[]){0x1A}, 1);                 /* VCOM */
	lcd_cmd_data(0x36, (const uint8_t[]){LCD_MADCTL}, 1);           /* 方向: 320x240 横屏(见 LCD_MADCTL) */
	lcd_cmd_data(0xB2, (const uint8_t[]){0x05,0x05,0x00,0x33,0x33}, 5);  /* Porch setting */
	lcd_cmd_data(0xB7, (const uint8_t[]){0x05}, 1);                 /* Gate control */
	lcd_cmd_data(0xBB, (const uint8_t[]){0x3F}, 1);                 /* VCOM */
	lcd_cmd_data(0xC0, (const uint8_t[]){0x2C}, 1);                 /* Power control */
	lcd_cmd_data(0xC2, (const uint8_t[]){0x01}, 1);                 /* VDV/VRH enable */
	lcd_cmd_data(0xC3, (const uint8_t[]){0x0F}, 1);                 /* VRH set */
	lcd_cmd_data(0xC4, (const uint8_t[]){0x20}, 1);                 /* VDV set */
	lcd_cmd_data(0xC6, (const uint8_t[]){0x01}, 1);                 /* 帧率 111Hz */
	lcd_cmd_data(0xD0, (const uint8_t[]){0xA4,0xA1}, 2);            /* Power control 1 */
	lcd_cmd_data(0xE8, (const uint8_t[]){0x03}, 1);
	lcd_cmd_data(0xE9, (const uint8_t[]){0x09,0x09,0x08}, 3);       /* Equalize time */
	lcd_cmd_data(0xE0, (const uint8_t[]){0xD0,0x05,0x09,0x09,0x08,0x14,0x28,
	                                     0x33,0x3F,0x07,0x13,0x14,0x28,0x30}, 14);
	lcd_cmd_data(0xE1, (const uint8_t[]){0xD0,0x05,0x09,0x09,0x08,0x03,0x24,
	                                     0x32,0x32,0x3B,0x14,0x13,0x28,0x2F}, 14);
	lcd_cmd_data(0x20, NULL, 0);                                    /* INVOFF: 本屏不反相 */
	lcd_cmd_data(0x29, NULL, 0);                                    /* 开显示 */

	lcd_backlight(1);
}

/* ==================== 共享内存接收端 ==================== */

SemaphoreHandle_t g_lcd_flush_sem;
volatile int      g_lcd_pending_fill;   /* 1 = 下一帧做 FILL, 0 = FLUSH */

static struct lcd_shm *lcd_shm(void)
{
	return (struct lcd_shm *)LCD_SHM_PHYS;
}

/* 把状态线写回 DRAM, 大核 mmap 后就能读到 */
static void lcd_stat_publish(struct lcd_shm *s)
{
	static unsigned long bytes_prev;

	s->stat.frame_cnt++;
	s->stat.ack_seq    = s->ctrl.seq;
	s->stat.last_bytes = (uint32_t)(g_spi2_bytes - bytes_prev);
	s->stat.state      = LCD_STATE_READY;
	bytes_prev         = g_spi2_bytes;
	flush_dcache_range((uintptr_t)&s->stat, sizeof(s->stat));
}

/* 控制线只读, 读之前作废本核旧 cache 行 */
static int lcd_ctrl_check(struct lcd_shm *s)
{
	inv_dcache_range((uintptr_t)&s->ctrl, sizeof(s->ctrl));
	if (s->ctrl.magic != LCD_SHM_MAGIC || s->ctrl.version != LCD_SHM_VERSION) {
		s->stat.err++;
		flush_dcache_range((uintptr_t)&s->stat, sizeof(s->stat));
		return -1;
	}
	return 0;
}

void lcd_shm_init(void)
{
	struct lcd_shm *s = lcd_shm();

	if (lcd_ctrl_check(s) != 0)
		return;

	s->stat.frame_cnt  = 0;
	s->stat.ack_seq    = 0;
	s->stat.last_ticks = 0;
	s->stat.last_bytes = 0;
	s->stat.err        = 0;
	s->stat.reload_ack = 0;         /* 热更握手: 新固件起来后清掉上次的起跳确认 */
	s->stat.state      = LCD_STATE_READY;
	flush_dcache_range((uintptr_t)&s->stat, sizeof(s->stat));

	lcd_backlight(s->ctrl.bl ? 1 : 0);
}

void lcd_shm_flush(void)
{
	struct lcd_shm *s = lcd_shm();
	uint32_t x0, y0, x1, y1, off, nbytes;
	TickType_t t0;

	if (lcd_ctrl_check(s) != 0)
		return;

	if (s->ctrl.flags & 1u) {
		x0 = 0; y0 = 0; x1 = LCD_W - 1; y1 = LCD_H - 1;
	} else {
		x0 = s->ctrl.x0; y0 = s->ctrl.y0;
		x1 = s->ctrl.x1; y1 = s->ctrl.y1;
	}
	/* 夹到屏内, 防大核写坏参数导致越界读 */
	if (x0 >= LCD_W) x0 = LCD_W - 1;
	if (y0 >= LCD_H) y0 = LCD_H - 1;
	if (x1 >= LCD_W) x1 = LCD_W - 1;
	if (y1 >= LCD_H) y1 = LCD_H - 1;
	if (x0 > x1 || y0 > y1) {
		s->stat.err++;
		flush_dcache_range((uintptr_t)&s->stat, sizeof(s->stat));
		return;
	}

	off    = ((uint32_t)y0 * LCD_W + x0) * 2u;
	nbytes = (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1) * 2u;

	/* 像素是大核刚写进 DRAM 的, 本核 cache 里的旧行必须先作废 */
	inv_dcache_range((uintptr_t)(s->fb + off), nbytes);

	s->stat.state = LCD_STATE_BUSY;
	flush_dcache_range((uintptr_t)&s->stat, sizeof(s->stat));

	t0 = xTaskGetTickCount();
	lcd_set_window((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1);
	lcd_data(s->fb + off, nbytes);

	s->stat.last_ticks = (uint32_t)(xTaskGetTickCount() - t0);
	lcd_stat_publish(s);
}

void lcd_shm_fill(void)
{
	struct lcd_shm *s = lcd_shm();
	TickType_t t0;

	if (lcd_ctrl_check(s) != 0)
		return;

	t0 = xTaskGetTickCount();
	lcd_fill((uint16_t)(s->ctrl.color & 0xFFFFu));
	s->stat.last_ticks = (uint32_t)(xTaskGetTickCount() - t0);
	lcd_stat_publish(s);
}

/* ==================== 刷屏任务 ==================== */

static void prvLcdTask(void *pvParameters)
{
	volatile unsigned long *p = (volatile unsigned long *)g_lcd_probe;
	TickType_t t0;
	uint32_t n = 0;

	(void)pvParameters;

	p[0] = LCD_PROBE_MAGIC_HEAD;
	p[1] = 0;
	p[12] = LCD_PROBE_MAGIC_TAIL;
	flush_dcache_range((void *)p, sizeof(g_lcd_probe));

	lcd_init();

	p[1] = 0x600D;                          /* init 走完的标记 */
	p[2] = spi2_read_reg(0x08);             /* SSIENR */
	p[3] = spi2_read_reg(0x00);             /* CTRL0  */
	p[4] = spi2_read_reg(0x14);             /* BAUDR  */
	flush_dcache_range((void *)p, sizeof(g_lcd_probe));

	for (;;) {
		int did = 0;

		t0 = xTaskGetTickCount();

		/* 只处理大核下发的 FLUSH/FILL。没有大核接管时**什么也不画**:
		 * 屏上保持黑底, 不再刷红/绿/蓝心跳(画面模式由 LVGL 那边负责)。 */
		if (xSemaphoreTake(g_lcd_flush_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
			if (g_lcd_pending_fill)
				lcd_shm_fill();
			else
				lcd_shm_flush();
			n++;
			did = 1;
		}

		if (did)
			p[5] = xTaskGetTickCount() - t0;   /* 本帧耗时(tick, 1ms) */
		p[6]  = n;                          /* 心跳已刷次数 */
		p[7]  = g_spi2_bytes;               /* 累计推入 SPI 的字节数 */
		p[8]  = spi2_read_reg(0x28);        /* SR */

		/* ctrl/stat 只读: 先作废本核旧 cache 行, 读到的才是 DRAM 真值
		 * (大核 /dev/mem 写进来的 ctrl.magic 因此能从 p[11] 看到) */
		inv_dcache_range((uintptr_t)lcd_shm(), 128);
		p[9]  = lcd_shm()->stat.frame_cnt;
		p[10] = lcd_shm()->stat.ack_seq;
		p[11] = lcd_shm()->ctrl.magic;

		p[12] = LCD_PROBE_MAGIC_TAIL;
		flush_dcache_range((void *)p, sizeof(g_lcd_probe));
	}
}

void lcd_task_start(void)
{
	g_lcd_flush_sem = xSemaphoreCreateBinary();
	xTaskCreate(prvLcdTask, "LCD", 1024, NULL, tskIDLE_PRIORITY + 2, NULL);
}
