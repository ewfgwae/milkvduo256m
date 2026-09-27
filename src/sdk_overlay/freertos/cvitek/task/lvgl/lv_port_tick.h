/*
 * 小核(C906L)版 LVGL 的系统时钟源 (LV_TICK_CUSTOM)。
 *
 * 为什么不用 FreeRTOS 的 xTaskGetTickCount():
 *   小核的 configTICK_RATE_HZ = 200, 也就是 5ms 一格。LVGL 的动画/定时器都按
 *   tick 推进, 5ms 的粒度会让动画明显发顿, 而且 lv_tick_elaps() 测出来的耗时
 *   全是 5ms 的整数倍。
 *
 * 改成直接读 RISC-V 的 mtime:
 *   - GetSysTime() 就是 rdtime, 见 arch/riscv64/include/arch_time.h
 *   - 在 SG2002 上 mtime 由 configSYS_CLOCK_HZ = 25MHz 驱动,
 *     见 arch/riscv64/include/arch_cpu.h
 *   - 于是 LVGL 拿到的是真正的 1ms 时钟
 */
#ifndef __LV_PORT_TICK_H__
#define __LV_PORT_TICK_H__

#include <stdint.h>

#include "arch_cpu.h"       /* configSYS_CLOCK_HZ */
#include "arch_time.h"      /* GetSysTime() = rdtime */

#define LV_PORT_TICK_HZ     (configSYS_CLOCK_HZ)
#define LV_PORT_TICKS_PER_MS (LV_PORT_TICK_HZ / 1000u)
#define LV_PORT_TICKS_PER_US (LV_PORT_TICK_HZ / 1000000u)

static inline uint32_t lv_port_tick_ms(void)
{
	return (uint32_t)(GetSysTime() / LV_PORT_TICKS_PER_MS);
}

/* 微秒时间戳, 只用来量"送显 vs 渲染"这种毫秒级的区间差值(做减法),
 * 32 位在 1MHz 下 71 分钟才回绕, 对区间差值没有影响。 */
static inline uint32_t lv_port_tick_us(void)
{
	return (uint32_t)(GetSysTime() / LV_PORT_TICKS_PER_US);
}

#endif /* __LV_PORT_TICK_H__ */
