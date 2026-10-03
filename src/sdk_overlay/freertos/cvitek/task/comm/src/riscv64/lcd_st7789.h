#ifndef __LCD_ST7789_H__
#define __LCD_ST7789_H__

#include <stdint.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "lcd_shm.h"

/* 屏尺寸(LCD_W/LCD_H)统一由 lcd_shm.h 给(320x240 横屏), 这里不再重复定义 ——
 * 两边各写一份最容易"改了一处漏一处"。本文件下面已经 include 了 lcd_shm.h。 */

/* ST7789 常用色 (RGB565) */
#define LCD_RED     0xF800
#define LCD_GREEN   0x07E0
#define LCD_BLUE    0x001F
#define LCD_WHITE   0xFFFF
#define LCD_BLACK   0x0000

void lcd_init(void);
void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void lcd_fill(uint16_t color);
/* rgb565_be: 大端字节序的 RGB565 像素流 (ST7789 要的就是这个顺序) */
void lcd_blit(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const uint8_t *rgb565_be);

/* 异步送显(给 LVGL 双缓冲做"渲染/送显重叠"用):
 *   lcd_blit_wait()  等上一块从 SPI 里彻底出去(含移位器), 重复调用是空操作;
 *   lcd_blit_start() 开窗 + 把像素交给 sysDMA 后立即返回, 搬运在后台。
 * 调用约定: wait() -> start()。start 内部开窗时也会自己 wait 一次, 所以就算
 * 漏调外层 wait 也不会把命令插进还在搬的像素流里(只是量不到等待耗时)。 */
void lcd_blit_wait(void);
void lcd_blit_start(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, const uint8_t *rgb565_be);
void lcd_backlight(int on);

/* 建一个小核侧的刷屏任务: 跑 ST7789 初始化, 之后只等大核的 FLUSH/FILL 命令。
 * (默认**不**被 comm_main.c 启动 —— 现在屏幕归 LVGL 独占, 见 lvgl_task.c) */
void lcd_task_start(void);

/* ---- 共享内存接收端 (大核通过 cmdqu 下命令, 像素走共享内存) ---------- */
/* 刷屏信号量: 大核发 FLUSH/FILL 命令时唤醒 LCD 任务(避免与 cmdqu 任务抢 SPI) */
extern SemaphoreHandle_t g_lcd_flush_sem;
/* 下一次被唤醒时做 FILL(1) 还是 FLUSH(0) */
extern volatile int      g_lcd_pending_fill;

void lcd_shm_init(void);    /* LCD_CMD_INIT: 校验 magic/version, 清状态线 */
void lcd_shm_flush(void);   /* LCD_CMD_FLUSH: 按 ctrl 矩形把 fb 刷到屏上 */
void lcd_shm_fill(void);    /* LCD_CMD_FILL: 用 ctrl.color 填满整屏 */

#endif /* __LCD_ST7789_H__ */
