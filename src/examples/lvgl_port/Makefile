# Milk-V Duo256M + ST7789 (240x320) LVGL v8.3 交叉编译
#
# 用法 (在 WSL 里执行):
#   make -C /home/chen/dx/TFT-ST7789/lvgl_port -j20
#
# 可覆盖的变量:
#   LVGL_ROOT  LVGL 源码根目录(需含 src/ 与 demos/)   默认 /home/chen/lvgl_port/lvgl
#   DX         duo-examples-main 的无空格符号链接      默认 /home/chen/dx
#   BUILD      中间目标文件目录                        默认 /home/chen/lvgl_port/build
#   OUT        产物路径                                默认 /home/chen/lvgl_port/lvgl_demo

LVGL_ROOT ?= /home/chen/lvgl_port/lvgl
DX        ?= /home/chen/dx
TC        ?= $(DX)/host-tools/gcc/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu
SYSROOT   ?= $(TC)/aarch64-linux-gnu/libc
BUILD     ?= /home/chen/lvgl_port/build
OUT       ?= /home/chen/lvgl_port/lvgl_demo

CROSS   := $(TC)/bin/aarch64-linux-gnu-
CC      := $(CROSS)gcc

# -I. 用于找到本目录下的 lv_conf.h (配合 -DLV_CONF_INCLUDE_SIMPLE)
# -MMD -MP 生成 .d 依赖文件: 少了它, 改了 lv_conf.h / lv_port_tick.h 后 make
# 不会重编 LVGL 那 200 来个 .o, 会拿着旧配置去链接, 出的问题非常难查。
CFLAGS  := -march=armv8-a -O3 -DNDEBUG -fno-strict-aliasing \
           -MMD -MP \
           -DLV_CONF_INCLUDE_SIMPLE \
           -I. -I$(LVGL_ROOT) \
           -I$(DX)/include/system \
           -I$(SYSROOT)/usr/include

LDFLAGS := -L$(DX)/libs/system/glibc_arm64 \
           -L$(SYSROOT)/lib -L$(SYSROOT)/usr/lib \
           -lwiringx

LVGL_SRCS := $(shell find $(LVGL_ROOT)/src -name '*.c' 2>/dev/null)
DEMO_SRCS := $(shell find $(LVGL_ROOT)/demos/widgets $(LVGL_ROOT)/demos/benchmark -name '*.c' 2>/dev/null)

PORT_OBJS := $(patsubst %.c,$(BUILD)/port/%.o,$(wildcard *.c))
LVGL_OBJS := $(patsubst $(LVGL_ROOT)/%.c,$(BUILD)/lvgl/%.o,$(LVGL_SRCS) $(DEMO_SRCS))
OBJS      := $(PORT_OBJS) $(LVGL_OBJS)

all: $(OUT)

$(BUILD)/port/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/lvgl/%.o: $(LVGL_ROOT)/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(OUT): $(OBJS)
	@$(CC) -o $@ $(OBJS) $(LDFLAGS)
	@echo "=== 产物 ==="
	@ls -l $@
	@$(CROSS)readelf -d $@ | grep -E "NEEDED|RPATH|RUNPATH" || true

clean:
	rm -rf $(BUILD) $(OUT)

# 由 -MMD 生成的依赖文件 (首次全量编译前还不存在, 所以用 -include)
-include $(OBJS:.o=.d)

.PHONY: all clean
