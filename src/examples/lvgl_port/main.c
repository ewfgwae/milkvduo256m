/*
 * Milk-V Duo256M + ST7789 (240x320) 的 LVGL v8.3 移植
 *
 * 复用了 TFT-ST7789/st7789 里已经调好的那条最快送显路径:
 *   - 硬件 SPI2, SPI_SPEED = 46875000 -> 内核 BAUDR=4 -> 实际 46.875MHz (SoC 手册上限)
 *   - 内核 spidev bufsiz 已改为 262144, 一次 ioctl 能发完整屏
 *
 * 渲染模式: 局刷 (full_refresh = 0) + 两块 240x40 小缓冲。
 *   送显 153600 字节在 46.875MHz 下要 26.2ms(实测约 31.6ms), 也就是整屏刷新的
 *   天花板只有 30 FPS 左右。而绝大多数界面每帧只有一部分是脏的, 按脏区发就能
 *   成比例省下传输时间: 实测 benchmark 从 30 FPS 提到 53 FPS。
 *
 * 颜色字节序: ST7789 走 RGB565 大端, 所以 lv_conf.h 里开了 LV_COLOR_16_SWAP 1,
 * 使 LVGL 缓冲区里的字节序与面板一致, flush 时直接整块 memcpy 出去, 无需逐像素转换。
 *
 * 用法: ./lvgl_demo [widgets|benchmark]   默认 widgets
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <sys/resource.h>

#include <wiringx.h>
#include "lvgl.h"
#include "demos/widgets/lv_demo_widgets.h"
#include "demos/benchmark/lv_demo_benchmark.h"

/* ---------------- 硬件配置 ---------------- */
#define TFT_W           240
#define TFT_H           320

#define SPI_PORT        2           /* 硬件 SPI2: GP6=SCK, GP7=SDO, CS=GP9 由硬件控制 */
#define SPI_SPEED       46875000    /* 落到内核 BAUDR=4 -> 46.875MHz */

#define PIN_DC          20          /* -> 屏 DC  */
#define PIN_RST         21          /* -> 屏 RES */
#define PIN_BLK         16          /* -> 屏 BLK */

#define RST_0           digitalWrite(PIN_RST, LOW)
#define RST_1           digitalWrite(PIN_RST, HIGH)
#define DC_0            digitalWrite(PIN_DC, LOW)
#define DC_1            digitalWrite(PIN_DC, HIGH)
#define BLK_1           digitalWrite(PIN_BLK, HIGH)

/* ---------------- 底层 SPI ---------------- */
/* 注意: wiringX 的 SPI 读写是全双工的, 底层 spidev 会把读回的数据写回同一块内存。
 * 因此传给它的缓冲必须是可写的。直接把 const 数据(.rodata 只读段)的指针传进来,
 * copy_to_user 会失败并返回 -1 / EFAULT —— 表现为命令能发出去、参数却全部丢失。 */
static void spi_write(const void *buf, size_t len)
{
    wiringXSPIDataRW(SPI_PORT, (unsigned char *)buf, (int)len);
}

static void panel_cmd(uint8_t c)
{
    DC_0;
    spi_write(&c, 1);
}

static void panel_data(const void *buf, size_t len)
{
    DC_1;
    spi_write(buf, len);
}

/* ---------------- ST7789 初始化序列 ---------------- */
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t d[14];
} seq_t;

static const seq_t init_seq[] = {
    {0x3A, 1,  {0x05}},                                     /* 65k mode (RGB565) */
    {0xC5, 1,  {0x1A}},                                     /* VCOM */
    {0x36, 1,  {0x00}},                                     /* MADCTL: RGB 顺序, 本屏实测确认 */
    {0xB2, 5,  {0x05, 0x05, 0x00, 0x33, 0x33}},             /* Porch */
    {0xB7, 1,  {0x05}},                                     /* Gate Control */
    {0xBB, 1,  {0x3F}},                                     /* VCOM */
    {0xC0, 1,  {0x2C}},                                     /* Power Control */
    {0xC2, 1,  {0x01}},                                     /* VDV/VRH Enable */
    {0xC3, 1,  {0x0F}},                                     /* VRH */
    {0xC4, 1,  {0x20}},                                     /* VDV */
    {0xC6, 1,  {0x01}},                                     /* Frame Rate: 111Hz */
    {0xD0, 2,  {0xA4, 0xA1}},                               /* Power Control 1 */
    {0xE8, 1,  {0x03}},                                     /* Equalize time */
    {0xE9, 3,  {0x09, 0x09, 0x08}},
    {0xE0, 14, {0xD0, 0x05, 0x09, 0x09, 0x08, 0x14, 0x28,
                0x33, 0x3F, 0x07, 0x13, 0x14, 0x28, 0x30}}, /* Gamma + */
    {0xE1, 14, {0xD0, 0x05, 0x09, 0x09, 0x08, 0x03, 0x24,
                0x32, 0x32, 0x3B, 0x14, 0x13, 0x28, 0x2F}}, /* Gamma - */
    {0x20, 0,  {0}},                                        /* INVOFF: 本屏不需要反相 */
};

static void panel_init(void)
{
    unsigned int i;
    uint8_t tmp[16];                    /* 可写的中转缓冲, 见上面 spi_write 的说明 */

    RST_0; usleep(100 * 1000);
    RST_1; usleep(100 * 1000);

    panel_cmd(0x11);                    /* Sleep Out */
    usleep(120 * 1000);

    for (i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
        panel_cmd(init_seq[i].cmd);
        if (init_seq[i].len) {
            memcpy(tmp, init_seq[i].d, init_seq[i].len);
            panel_data(tmp, init_seq[i].len);
        }
    }

    panel_cmd(0x29);                    /* Display ON */
}

static void panel_set_window(int x1, int y1, int x2, int y2)
{
    uint8_t b[4];

    b[0] = x1 >> 8; b[1] = x1; b[2] = x2 >> 8; b[3] = x2;
    panel_cmd(0x2A); panel_data(b, 4);          /* Column Address Set */

    b[0] = y1 >> 8; b[1] = y1; b[2] = y2 >> 8; b[3] = y2;
    panel_cmd(0x2B); panel_data(b, 4);          /* Row Address Set */

    panel_cmd(0x2C);                            /* Memory Write */
}

/* ---------------- LVGL 显示驱动 ---------------- */
/* 局刷: 两块 240x40 的小缓冲(各 19200 字节, 共 38.4KB), 配合 full_refresh = 0,
 * LVGL 只把脏区推给屏。实测比"整屏缓冲 + full_refresh = 1"快很多:
 * benchmark 30 -> 53 FPS (它每帧其实只有约 40% 的屏幕是脏的)。
 *
 * 缓冲再加大会不会更快? 试过 80 行 / 160 行: 53.5 / 53.8 FPS, 与 40 行无实质差别。
 * 因为 flush 次数不由缓冲行数决定, 而是"每帧有几个独立脏矩形"——LVGL 是按脏矩形
 * 逐个送屏的, benchmark 每帧约 5 个独立脏块, 就算给 160 行也还是 5.3 次 flush/帧。
 * 所以维持 40 行, 省一半 RAM。 */
#define DRAW_LINES 40
static lv_color_t draw_buf1[TFT_W * DRAW_LINES];
static lv_color_t draw_buf2[TFT_W * DRAW_LINES];
static volatile unsigned int frame_cnt = 0;     /* 已刷完的帧数, 用于统计 FPS */

/* 计时埋点: 微秒级, 用于区分"渲染慢"还是"送显慢" */
static uint64_t us_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
static uint64_t t_flush_us = 0;                 /* 每秒累计: 花在 SPI 送显上的时间 */
static uint64_t t_handler_us = 0;               /* 每秒累计: 花在 lv_timer_handler 的总时间 */
static uint64_t dirty_px = 0;                   /* 每秒累计: 实际推给屏的像素数(用于算脏区占比) */
static unsigned int flush_calls = 0;            /* 每秒累计: flush 回调被调用了几次(局刷下一帧会有多次) */

static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    /* 局刷时一"帧"会拆成多次 flush_cb 调用, 只有最后一块才算刷完一帧。
     * 必须在 lv_disp_flush_ready() 之前问, 那个调用会把该标志清掉。 */
    int last = lv_disp_flush_is_last(drv);
    uint64_t t0 = us_now();

    panel_set_window(area->x1, area->y1, area->x2, area->y2);
    panel_data(px, (size_t)w * h * 2);          /* 一次整块发出去 */

    t_flush_us += us_now() - t0;
    dirty_px += (uint64_t)w * h;
    flush_calls++;
    if (last) frame_cnt++;
    lv_disp_flush_ready(drv);
}

/* ---------------- 屏上 OSD: 真实 CPU 占用 ----------------
 * LVGL 自带 perf monitor 的 "CPU%" 不是 CPU 占用率, 而是"主循环占空比":
 * 它累加 lv_timer_handler() 从进入到返回的墙钟时间, 里面那几十毫秒其实全是在
 * 阻塞等 SPI/DMA 传完(进程处于 D 状态, 一个 CPU 周期都不花)。本移植下它恒显示
 * 96%, 而 top / /proc/self/stat 里进程实际只占 6~8%, 差了一个数量级。
 * 所以这里关掉它, 用 getrusage() 取真实的 utime+stime 自己算。 */
static uint64_t self_cpu_us(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (uint64_t)ru.ru_utime.tv_sec * 1000000ULL + (uint64_t)ru.ru_utime.tv_usec
         + (uint64_t)ru.ru_stime.tv_sec * 1000000ULL + (uint64_t)ru.ru_stime.tv_usec;
}

static lv_obj_t *osd_label;
static unsigned int osd_fps_x10 = 0;            /* 最近一秒刷出的整屏帧率 ×10 (LVGL 的 snprintf 不支持 %f) */
static unsigned int osd_cpu = 0;                /* 最近一秒进程真实 CPU 占用, 百分比 */

static void osd_timer_cb(lv_timer_t *timer)
{
    static uint64_t t_prev = 0, cpu_prev = 0;
    static unsigned int frame_prev = 0;
    uint64_t now = us_now(), cpu = self_cpu_us();
    uint64_t dt = now - t_prev;

    if (t_prev && dt) {
        osd_fps_x10 = (unsigned int)((uint64_t)(frame_cnt - frame_prev) * 10000000ULL / dt);
        osd_cpu = (unsigned int)((cpu - cpu_prev) * 100ULL / dt);
        lv_label_set_text_fmt(osd_label, "%u.%u FPS\n%u%% CPU",
                              osd_fps_x10 / 10, osd_fps_x10 % 10, osd_cpu);
    }
    t_prev = now;
    cpu_prev = cpu;
    frame_prev = frame_cnt;
    (void)timer;
}

static void osd_create(void)
{
    osd_label = lv_label_create(lv_layer_sys());   /* 系统层, 盖在 demo 上面 */
    lv_obj_set_style_bg_opa(osd_label, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(osd_label, lv_color_black(), 0);
    lv_obj_set_style_text_color(osd_label, lv_color_white(), 0);
    lv_obj_set_style_pad_top(osd_label, 3, 0);
    lv_obj_set_style_pad_bottom(osd_label, 3, 0);
    lv_obj_set_style_pad_left(osd_label, 3, 0);
    lv_obj_set_style_pad_right(osd_label, 3, 0);
    lv_obj_set_style_text_align(osd_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(osd_label, "0.0 FPS\n0% CPU");
    lv_obj_align(osd_label, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_timer_create(osd_timer_cb, 1000, NULL);
}

/* ---------------- 单调时钟 ---------------- */
static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

int main(int argc, char **argv)
{
    char model[] = "milkv_duo256m";
    const char *which = (argc > 1) ? argv[1] : "widgets";
    static lv_disp_draw_buf_t draw_buf;
    static lv_disp_drv_t disp_drv;
    uint32_t t_report, frames_at_report;

    /* ---- 1. wiringX + GPIO + SPI2 ---- */
    if (wiringXSetup(model, NULL) == -1) {
        printf("wiringXSetup failed\n");
        return -1;
    }
    pinMode(PIN_RST, PINMODE_OUTPUT);
    pinMode(PIN_DC, PINMODE_OUTPUT);
    pinMode(PIN_BLK, PINMODE_OUTPUT);

    if (wiringXSPISetup(SPI_PORT, SPI_SPEED) < 0) {
        printf("wiringXSPISetup failed (SPI%d @ %d Hz)\n", SPI_PORT, SPI_SPEED);
        return -1;
    }
    BLK_1;

    /* ---- 2. 面板上电初始化 ---- */
    panel_init();
    /* 先清成黑屏, 避免 LVGL 起来前显示花屏 */
    {
        static lv_color_t black[TFT_W * TFT_H];   /* 静态 -> 全 0 -> 黑 */
        panel_set_window(0, 0, TFT_W - 1, TFT_H - 1);
        panel_data(black, sizeof(black));
    }

    /* ---- 3. LVGL 初始化 ---- */
    lv_init();

    lv_disp_draw_buf_init(&draw_buf, draw_buf1, draw_buf2, TFT_W * DRAW_LINES);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res     = TFT_W;
    disp_drv.ver_res     = TFT_H;
    disp_drv.flush_cb    = disp_flush;
    disp_drv.draw_buf    = &draw_buf;
    disp_drv.full_refresh = 0;          /* 0 = 局刷: flush_cb 只拿到脏区, 不再是整屏 */
    lv_disp_drv_register(&disp_drv);

    /* ---- 3.5 屏上 OSD (真实 CPU 占用) ---- */
    osd_create();

    /* ---- 4. 选 demo ---- */
    if (strcmp(which, "benchmark") == 0) {
        printf("run: lv_demo_benchmark()\n");
        lv_demo_benchmark();
    } else {
        printf("run: lv_demo_widgets()\n");
        lv_demo_widgets();
    }
    fflush(stdout);

    /* ---- 5. 主循环 ----
     * 不再手动 lv_tick_inc(): LVGL 通过 LV_TICK_CUSTOM 自己实时读 clock_gettime,
     * 这样它在 handler 内部测到的耗时才是真的 (否则 perf monitor 会恒显示 62 FPS / 0% CPU)。 */
    t_report = tick_ms();
    frames_at_report = frame_cnt;

    while (1) {
        uint32_t now;

        {
            uint64_t r0 = us_now();
            lv_timer_handler();
            t_handler_us += us_now() - r0;
        }

        now = tick_ms();
        if (now - t_report >= 1000) {
            unsigned int f = frame_cnt - frames_at_report;
            double ms = (double)(now - t_report);
            printf("FPS: %.2f  (%u 帧/秒, %.1f ms/帧) | 送显 %.1f ms/s | LVGL 处理 %.1f ms/s | 空转 %.1f ms/s"
                   " | 脏区 %.1f%%/帧 (%.1f 次 flush/帧) | 屏上 OSD(真实): %u.%u FPS / %u%% CPU\n",
                   (double)f * 1000.0 / ms,
                   f,
                   ms / (f ? (double)f : 1.0),
                   (double)t_flush_us / 1000.0,
                   (double)t_handler_us / 1000.0,
                   ms - (double)t_handler_us / 1000.0,
                   (double)dirty_px * 100.0 / ((double)TFT_W * TFT_H * (f ? f : 1)),
                   (double)flush_calls / (f ? (double)f : 1.0),
                   osd_fps_x10 / 10,
                   osd_fps_x10 % 10,
                   osd_cpu);
            fflush(stdout);
            t_report = now;
            frames_at_report = frame_cnt;
            t_flush_us = 0;
            t_handler_us = 0;
            dirty_px = 0;
            flush_calls = 0;
        }

        usleep(1000);
    }

    return 0;
}
