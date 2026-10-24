#ifndef __LCD_TOUCH_H__
#define __LCD_TOUCH_H__

/* 触摸驱动骨架(plans/05 Phase 1 建, 真实现等 plans/06 定芯片型号)。
 *
 * 接口与芯片解耦: lvgl_task.c 只认下面两个函数。plans/06 Phase 2 换真驱动
 * (XPT2046 位邦 SPI / GT911 / FT6x06 位邦 I2C)时**接口不动**, 只改 lcd_touch.c。
 *
 * 坐标一律是 320x240 横屏坐标(与 lcd_shm.h 的 LCD_W/LCD_H 一致),
 * 芯片原始值到屏坐标的映射/校准在驱动内部完成。 */

/* 引脚方向 / 复位时序等一次性初始化。当前无芯片, 空实现。 */
void lcd_touch_init(void);

/* 读一次触摸状态:
 *   返回 1 = 按下, 把屏坐标写进 *x 与 *y;
 *   返回 0 = 未按下(两个出参不动)。
 * 去抖在内部做, 调用方(LVGL indev, ~30ms 一拍)拿到的直接是稳定值。
 * 当前无芯片, 恒返回 0 —— 按钮只能由大核的 LCD_CMD_BTN 触发。 */
int lcd_touch_read(int *x, int *y);

#endif /* __LCD_TOUCH_H__ */
