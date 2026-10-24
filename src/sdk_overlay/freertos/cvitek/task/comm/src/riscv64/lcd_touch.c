/* lcd_touch.c —— 触摸驱动骨架(plans/06 定型号前的占位)
 *
 * plans/05 Phase 1 建: 屏上按钮的 CLICKED 链路先就位(indev 已在
 * lvgl_task.c 注册), plans/06 Phase 2 换真驱动时只改本文件的实现。
 * 芯片候选与接线见 plans/06/AGENTS.md。
 * 当前恒"未按下"。 */
#include <stdint.h>

#include "lcd_touch.h"

void lcd_touch_init(void)
{
	/* 无芯片: 将来在这里做引脚方向 / 复位时序(SPI 位邦或 I2C 位邦)。 */
}

int lcd_touch_read(int *x, int *y)
{
	(void)x;
	(void)y;
	return 0;
}
