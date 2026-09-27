# 小核(C906L / FreeRTOS) 上的 LVGL v8.3
#
# 编 LVGL 的 src/  +  demos/widgets/ 与 demos/benchmark/(含各自 assets/, 就是两个
# demo 用到的图片资源) + 本目录的移植层。屏上到底跑哪一个由 lvgl_task.c 决定,
# 没被引用的那份 demo 会被链接器的 --gc-sections 丢掉, 不会占固件体积。
# LVGL 源码树不复制进 SDK, 直接引用宿主机上现成的那份 (与 Linux 侧 lvgl_port 同源),
# 需要改版本时用 -DLVGL_ROOT=... 覆盖即可。
set(LVGL_ROOT "/home/chen/lvgl_port/lvgl" CACHE PATH "LVGL 源码根目录(需含 src/ 与 lvgl.h)")

file(GLOB_RECURSE _LVGL_SRCS "${LVGL_ROOT}/src/*.c")
file(GLOB_RECURSE _DEMO_SRCS "${LVGL_ROOT}/demos/widgets/*.c"
                              "${LVGL_ROOT}/demos/benchmark/*.c")
file(GLOB _PORT_SRCS "*.c")

if (NOT _LVGL_SRCS)
    message(FATAL_ERROR "在 ${LVGL_ROOT}/src 下没找到 .c —— LVGL_ROOT 配错了")
endif()
if (NOT _DEMO_SRCS)
    message(FATAL_ERROR "在 ${LVGL_ROOT}/demos/{widgets,benchmark} 下没找到 .c")
endif()

add_library(lvgl STATIC ${_LVGL_SRCS} ${_DEMO_SRCS} ${_PORT_SRCS})

# lv_conf.h 用 LV_CONF_INCLUDE_SIMPLE 的方式找: 只认包含路径里的 "lv_conf.h"
target_compile_definitions(lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE)

target_include_directories(lvgl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}                              # lv_conf.h / lv_port_*.h / lvgl_task.h
    ${LVGL_ROOT}                                             # lvgl.h
    ${CMAKE_INSTALL_INC_PREFIX}/kernel                       # FreeRTOS.h / task.h
    ${CMAKE_INSTALL_INC_PREFIX}/arch                         # arch_helpers.h / arch_cpu.h / arch_time.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../comm/src/${RUN_ARCH}      # lcd_st7789.h (comm 的 LCD 驱动)
)

# 小核上 LVGL 只被这一个目标消费; 头文件给 comm 用 (lvgl_task.h)
target_include_directories(lvgl PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LVGL_ROOT}
)

install(TARGETS lvgl DESTINATION lib)
