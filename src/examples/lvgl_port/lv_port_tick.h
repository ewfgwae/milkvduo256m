/*
 * 给 LVGL 用的系统时钟源 (LV_TICK_CUSTOM)。
 *
 * 为什么不直接用 lv_tick_inc():
 *   lv_tick_inc() 只能在 lv_timer_handler() 之外调用, 于是 handler 内部
 *   (也就是渲染 + 送显阻塞的那几十毫秒) 流逝的时间对 LVGL 是"看不见"的。
 *   所有基于 lv_tick_elaps() 的测量都会得到 0, 直接导致:
 *     - 性能监视器的 FPS 被限幅到 1000/refr_period = 62, 永远不动
 *     - CPU 占用算成 0%
 *     - LVGL 的定时器/动画调度也不准
 *   改成让 LVGL 每次实时读 clock_gettime(), 这些测量才成立。
 */

#ifndef LV_PORT_TICK_H
#define LV_PORT_TICK_H

#include <stdint.h>
#include <time.h>

static inline uint32_t lv_port_tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ULL +
                      (uint64_t)ts.tv_nsec / 1000000ULL);
}

#endif /* LV_PORT_TICK_H */
