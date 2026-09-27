================================================================================
  Milk-V Duo256M 双核 LCD 监控设备 —— 开发指令记录
  文件位置: /root/指南/readme.txt
================================================================================

--------------------------------------------------------------------------------
【写给之后接手的人 —— 请务必照做】
--------------------------------------------------------------------------------
本文件记录"本机(PC/WSL) <-> 开发板"之间所有常用指令: 编译、拷贝、执行、烧录/热更。

请每一位改过代码、编译、部署或升级固件的人, 在动完手之后把用到的指令
追加到文末"七、变更记录"里, 格式:

    日期 | 改动人 | 改了什么 | 用到的完整指令 | 结果/备注

要求:
  1. 指令写成"可直接复制粘贴"的完整形式, 不要凭记忆简写。
  2. 只写你这次真正执行并验证成功的, 不要把没试过的写进来。
  3. 涉及固件/内存地址的改动, 一定写清楚目标地址和校验方式。

★ 安全铁律: 本设备没有任何变砖恢复手段(无 SD 读卡器、无 USB 转串口模块)。
  任何情况下都不要乱动 fip.bin; 若确有必要, 必须做字节级校验, 确认除小核固件段
  (BLCP_2ND)外其余内容逐字节不变。日常改小核固件请一律走"热更"(见第四章),
  它不碰 fip.bin、不重启系统。

================================================================================
 一、环境与路径约定
================================================================================
本机编译环境 = WSL (Ubuntu-22.04), 用户 chen:

  SDK 根目录          : ~/duo-sdk-v2
  小核固件工程         : ~/duo-sdk-v2/freertos/cvitek
  小核固件产物         : ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin
  aarch64 交叉编译器   : ~/duo-sdk-v2/host-tools/gcc/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc
  riscv64 交叉编译器   : ~/duo-sdk-v2/host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-*
  大核程序源码         : ~/lcdhost/lcd_sender.c   (编译产物 ~/lcdhost/lcd_sender)

开发板:
  IP 192.168.42.1   用户 root   密码 milkv
  本文件 : /root/指南/readme.txt
  提示   : /tmp 重启后会清空, 想长期保存的文件放 /root。

================================================================================
 二、编译指令
================================================================================

2.1 编译【小核 RISC-V C906L 固件】cvirtos.bin   —— 每次改小核代码后执行
--------------------------------------------------------------------------------
    cd ~/duo-sdk-v2
    source build/envsetup_milkv.sh milkv-duo256m-glibc-arm64-sd
    cd build
    make rtos

    # 产物:
    ls -l ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin

  说明: 必须先 source envsetup 设置好交叉编译环境, 再进 build 目录 make。
        source 只对当前 shell 生效, 每开一个新终端都要重做一次。
        rtos 源码工程真源备份在 PC 的 TFT-ST7789/rtos_lcd/ 下。

2.2 编译【大核 aarch64 Linux 程序】lcd_sender   —— 每次改大核代码后执行
--------------------------------------------------------------------------------
    GCC=~/duo-sdk-v2/host-tools/gcc/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu-gcc
    $GCC -O2 -Wall -static -o ~/lcdhost/lcd_sender ~/lcdhost/lcd_sender.c

  说明: 用 -static 静态链接, 避免板子上缺库跑不起来。

================================================================================
 三、拷贝文件到板子 /root 的指令 (scp)
================================================================================

3.1 交互式(手输密码 milkv)
--------------------------------------------------------------------------------
    scp ~/lcdhost/lcd_sender root@192.168.42.1:/root/lcd_sender
    scp ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin root@192.168.42.1:/root/cvirtos.bin

3.2 免交互(脚本/自动化里用, 密码 milkv 由 SSH_ASKPASS 提供)
--------------------------------------------------------------------------------
    printf '#!/bin/sh\necho milkv\n' > /tmp/apX.sh ; chmod +x /tmp/apX.sh
    export SSH_ASKPASS=/tmp/apX.sh
    export SSH_ASKPASS_REQUIRE=force
    setsid scp -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
        ~/lcdhost/lcd_sender root@192.168.42.1:/root/lcd_sender </dev/null

3.3 免交互登录执行(同上先做 SSH_ASKPASS 那三行)
--------------------------------------------------------------------------------
    setsid ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        root@192.168.42.1

================================================================================
 四、在板子上执行可执行文件
================================================================================

4.1 登录板子
--------------------------------------------------------------------------------
    ssh root@192.168.42.1          # 密码 milkv

4.2 加可执行权限
--------------------------------------------------------------------------------
    chmod +x /root/lcd_sender

4.3 大核程序 lcd_sender 子命令一览
--------------------------------------------------------------------------------
    /root/lcd_sender stat             打印共享内存 ctrl/stat
    /root/lcd_sender init             上电初始化屏幕(设背光, 发 LCD_CMD_INIT)
    /root/lcd_sender scan             扫描/探测 cmdqu ioctl 接口(调试用)
    /root/lcd_sender ping             发 LCD_CMD_PING 测通讯通路
    /root/lcd_sender bl 1             背光开(0 = 关)
    /root/lcd_sender fill 0xF800      整屏纯色(参数是 RGB565 十六进制)
    /root/lcd_sender bars             画 8 条彩条, 整屏刷新一次
    /root/lcd_sender anim 300         移动白条, 局部刷新, 跑 300 帧
    /root/lcd_sender mon 1000         采样大核 /proc/stat, 用 mailbox 把大核 CPU
                                      占用率发给小核 OSD 显示, 每 1000ms 一次
    /root/lcd_sender rtos <固件文件>  热更小核固件(见第五章), 免烧 fip、免重启

    后台常驻:
    nohup /root/lcd_sender mon 1000 >/dev/null 2>&1 &

4.4 大核 LVGL 刷屏 demo(用于和大核/小核刷屏性能对比)
--------------------------------------------------------------------------------
    /root/lvgl_demo

4.5 看小核状态(探针 g_lvgl_probe[16], 每项 8 字节)
--------------------------------------------------------------------------------
    # ★ 探针地址会随固件体积变化, 换了固件务必先用 nm 重查, 别照抄老地址:
    #   nm ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.elf \
    #      | grep -E 'g_lvgl_probe|g_dma_probe|g_spi2_probe'
    # 下面这组地址对应 build id 0x484A0004 的固件。
    # 读法: 第 n 项的地址 = g_lvgl_probe + n*8; 一律用 width=32 逐项读最省心
    # (本机 busybox 的 devmem 对 64 位读法结果不稳定, 32 位逐项读最可靠)。
    devmem 0x8fe6cb20 32     # [0]  'LVGL' 魔数头 (0x4C56474C)
    devmem 0x8fe6cb28 32     # [1]  0x600D = demo+OSD 已建完
    devmem 0x8fe6cb30 32     # [2]  主循环圈数(隔几秒读两次, 在涨 = 小核在跑)
    devmem 0x8fe6cb58 32     # [7]  帧率 x10 (例: 0x1BF = 447 -> 44.7 FPS)
    devmem 0x8fe6cb68 32     # [9]  送显耗时 us/秒   ┐
    devmem 0x8fe6cb70 32     # [10] LVGL 处理耗时 us/秒 ├ 定位瓶颈用(见 lvgl_task.c)
    devmem 0x8fe6cb78 32     # [11] 推给屏的像素数/秒 ┘
    devmem 0x8fe6cb90 32     # [14] 固件 build id (0x484A0004 = 'HJ' + 序号)
    devmem 0x8fe6cb98 32     # [15] 'MONR' 魔数尾 (0x4D4F4E52)
    devmem 0x8ff90000 32     # 常驻热更跳板首条指令(应为 0x30401073)
    # sysDMA 自检探针 g_dma_probe[16] (见 6.2); SPI2 送显 DMA 探针 g_spi2_probe[8] (见 6.4):
    devmem 0x8fec4c78 32     # g_dma_probe[13]  自检结果码, 0 = 成功
    devmem 0x8fec8168 32     # g_spi2_probe[5]  DMA 失败回退 PIO 次数, 0 = DMA 全通

================================================================================
 五、烧录 / 更新小核固件 —— 现在走"热更", 不用烧 fip、不用重启
================================================================================
【结论】小核固件现在是"热更": 大核程序把新固件直接写进小核内存区, 小核自己跳过去
       重新跑。全程不动 fip.bin, 不重启系统, 不存在变砖路径。
       只有极特殊情况(见 5.3)才需要重烧 fip。

5.1 小核固件热更(日常唯一做法)
--------------------------------------------------------------------------------
    板上一条命令:
    /root/lcd_sender rtos /root/cvirtos.bin

    完整流程(本机 -> 板子):
    # 1) 本机重新编译小核固件
    cd ~/duo-sdk-v2 && source build/envsetup_milkv.sh milkv-duo256m-glibc-arm64-sd
    cd build && make rtos
    # 2) 推上去
    scp ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin root@192.168.42.1:/root/cvirtos.bin
    # 3) 板上热更
    ssh root@192.168.42.1 '/root/lcd_sender rtos /root/cvirtos.bin'

    正常输出:
      固件 /root/cvirtos.bin: 1640640 字节 = 应用区 1638400 + 常驻跳板 2240 (跳板不重写)
      小核已停在跳板 (0.0 ms), 开始往 0x8FE00000 写 1638400 字节
      /dev/mem 写入 + 读回校验通过
      已置 ctrl.reload=0x484F544A, 等新固件启动...
      热更完成: 新固件已运行 (state=1, ~200 ms)        <- 退出码 0 = 成功

    验证(隔几秒各读两次, 地址随固件变, 见 4.5):
      devmem 0x8fe6cb30 32     # [2] 主循环圈数在涨 = 新固件在跑
      devmem 0x8fe6cb90 32     # [14] 应等于本次固件的 build id

5.2 内存布局(热更为什么安全)
--------------------------------------------------------------------------------
    0x8FE00000 .. 0x8FF90000   小核应用区  (1638400 字节, 热更时原地覆盖)
    0x8FF90000 .. 0x8FFC0000   常驻热更跳板 .hotjump (约 2 KB, 永不覆盖)
    0x8FFC0000 ..              LCD 共享内存

    大核只写"应用区", 物理上碰不到跳板和共享内存; 写完还会逐字节读回校验,
    不符就不放行起跳 —— 最坏只是小核没能重启, 重启板子即回旧固件。
    跳板执行期间还会做一次全 cache 失效, 防止旧固件的脏 cache 行回写覆盖新固件。

5.3 极端情况: 重烧 fip.bin (会重启一次, 慎用!)
--------------------------------------------------------------------------------
    仅在如下情况使用:
      - 常驻跳板本身(hotjump.S)需要修改;
      - 或热更后小核彻底起不来。
    # 打包: 以板卡当前 fip 作底, 只替换 BLCP_2ND(小核固件)段
    python3 ~/duo-sdk-v2/fsbl/plat/cv181x/fiptool.py genfip \
        --OLD_FIP <板卡当前 fip.bin> \
        --BLCP_2ND ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin \
        <输出的新 fip.bin>

    # 打包后必须先做字节级校验, 确认除 BLCP_2ND 段外其余内容逐字节不变!
    # 历史: 板卡当前 fip.bin 的 md5 应为 a5a5ca0109d98e238ac82c0aae6d175a
    # 备份: 板子上历史 fip 备份在 /boot/fip.bin.bak_*

================================================================================
 六、常见故障与排查
================================================================================

6.1 【必看】热更/通讯报 "No buffer space available"、ping 发不出去
--------------------------------------------------------------------------------
  现象:
    /root/lcd_sender rtos /root/cvirtos.bin
      -> ioctl RTOS_CMDQU_SEND: No buffer space available
    或 /root/lcd_sender ping 直接失败; dmesg 里能看到 "No valid mailbox is available"。

  原因: 大小核之间的 IPCM mailbox 一共只有 8 个槽位(物理地址 0x1900400 起, 每槽
        8 字节)。若大核进程(比如后台的 `lcd_sender mon`)被 kill -9 强杀, 它已经
        发出、小核也已回复的那条消息就没人接手, 槽位的 linux_valid/rtos_valid 两位
        会一直停在 1、永不释放。8 个槽位被这种"孤儿回执"占满后, 之后任何发送都会
        返回 ENOBUFS。这不是固件坏了, 只是邮箱满了。

  修复: 清零 8 个槽位(只腾空邮箱, 不影响正在跑的小核固件):
    i=0; while [ $i -lt 8 ]; do a=$((0x1900400 + i*8)); devmem $(printf 0x%x $a) 64 0; i=$((i+1)); done
    然后 /root/lcd_sender ping 应立即恢复正常。

  预防: 停 lcd_sender 优先用 `killall lcd_sender`(发 SIGTERM 让它自己收尾);
        只有卡死才用 `killall -9 lcd_sender`, 且用完务必先清槽位再热更。

  查看槽位占用(每槽 8 字节, 非 0 即占用; 正常空闲时全 0):
    devmem 0x1900400 64 ; devmem 0x1900408 64 ; ... ; devmem 0x1900438 64

6.2 小核 sysDMA 自检探针
--------------------------------------------------------------------------------
  小核固件启动时会调 sysdma_selftest() 做一次 4096 字节的内存到内存搬运, 用来确认
  C906L 能否驱动 SoC 的 sysDMA(DesignWare AXI DMAC, 基址 0x04330000)。结果放在
  g_dma_probe[16](地址用 `nm cvirtos.elf | grep g_dma_probe` 查):
    devmem 0x8fec4c78 32     # [13] 结果码: 0=成功; 0xE1=控制器不可见; 0xE2=超时;
                             #      0xE3=数据不对; 0xE5=通道被占
    devmem 0x8fec4c68 32     # [11] 不一致字节数, 应为 0
  说明: 借的是 sysDMA 的空闲通道 6(大核 dw_dmac 正在用 0..3), 且不碰全局 RESET/CFG,
        与 Linux 侧共存。小核读 DMAC_ID 恒为 0(本 SoC 如此), 存在性要看 DMAC_COMPVER。

6.3 【编译坑】改了源码, 热更后行为却没变
--------------------------------------------------------------------------------
  现象: 明明改了 .c 文件, 编译也过了, 热更后固件行为还是旧的(探针数值不变)。
  原因: `make rtos` 对已存在的 .obj 有时不重编(陈旧对象被直接复用)。注意: 顶层
        `~/duo-sdk-v2/build` 只是转调, 真正的对象树在 ~/duo-sdk-v2/freertos/cvitek/build/。
  处理: 删掉对应 .obj 和 libcomm.a 再编, 例如:
    cd ~/duo-sdk-v2/freertos/cvitek/build
    rm -f task/comm/CMakeFiles/comm.dir/src/riscv64/<文件名>.c.obj task/comm/libcomm.a
    cd ~/duo-sdk-v2/build && make rtos

  ★ 更省事的做法(只重编改动的那一层, 实测可靠):
    对象路径规律是 <子目录>/CMakeFiles/<目标>.dir/<源文件>.c.obj, 例如改 lvgl_task.c:
    cd ~/duo-sdk-v2/freertos/cvitek/build/task
    rm -f lvgl/CMakeFiles/lvgl.dir/lvgl_task.c.obj
    cmake --build . --target install && cmake --build . --target cvirtos.bin
    # 产物: ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin
  验证: 用反汇编确认新代码确实进了 ELF(把 <函数名>/<特征立即数> 换成你自己的):
    ~/duo-sdk-v2/host-tools/gcc/riscv64-elf-x86_64/bin/riscv64-unknown-elf-objdump \
        -d --disassemble=<函数名> ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.elf \
        | grep -i <特征立即数>

6.4 小核 SPI2 送显-DMA 探针
--------------------------------------------------------------------------------
  小核刷屏的 spi2_write_dma() 每搬一块数据就把统计写进 g_spi2_probe[8]
  (地址用 `nm cvirtos.elf | grep g_spi2_probe` 查; build id 0x484A0004 对应 0x8fec8140)。
  第 n 项的地址 = 基址 + n*8:
    devmem 0x8fec8140 32     # [0] 魔数 'SDM2' (0x53444D32), 不匹配说明地址过期
    devmem 0x8fec8148 32     # [1] DMA 调用次数 (隔几秒读两次, 在涨 = 确实在走 DMA)
    devmem 0x8fec8150 32     # [2] DMA 累计字节数
    devmem 0x8fec8158 32     # [3] 最近一次返回码 0 = 成功 (非 0 见 6.5)
    devmem 0x8fec8160 32     # [4] SPI FIFO 深度(实测 16)
    devmem 0x8fec8168 32     # [5] ★ DMA 失败回退 PIO 的次数, 0 = SPI2 送显已全程走 DMA
    devmem 0x8fec8170 32     # [6] 写进 DMATDLR 的水位(8)
    devmem 0x8fec8178 32     # [7] 目的端 MSIZE 编码(2 = 一次 8 项)
  判据: [5]==0 且 [3]==0 即 DMA 通路完全生效, 一次都没退回逐字节轮询。
  参考寄存器(基址 0x04330000 的 sysDMA, 小核借的是通道 7):
    devmem 0x04330820 64     # ch7 CFG: 正常应含 SRC_PER=DST_PER=5 (5 = SPI2_TX 请求线)
    devmem 0x04330018 64     # ch7 CH_EN: 刷新期间读到 bit7=1(0x80) 是正常的(正在搬);
                             #           长时间空闲时若仍为 1, 说明通道卡住, 需查 6.5

6.5 【重要】SPI2 送显 DMA 不生效(DMA 一次都没搬成)
--------------------------------------------------------------------------------
  现象: g_spi2_probe[1](DMA 次数) 不涨、[5](回退 PIO 次数) 一直涨、[3] 非 0;
        ch7 CH_EN(0x04330018) 的 bit7 恒为 1、BLOCK_TS 不动 —— 通道"已使能、等握手"卡死。
  根因(已修): SPI2 的 DMA 请求线映射必须对。板级 DTS 写的是
        dmas = <&dmac 4 1 1>   /* rx: SPI2_RX */
               <&dmac 5 1 1>;  /* tx: SPI2_TX */
    即 4 = SPI2_RX、5 = SPI2_TX。送显只发不收, 请求线必须用 5;
    若误填 4, TX-only 时 RX FIFO 永不进数据, 请求线永不置起, 通道就永远卡在等握手。
  自查: 板子上 `cat /proc/device-tree/sysdma_remap/ch-remap` 应看到
        槽位 4=0x14(=20=CVI_SPI2_RX)、槽位 5=0x15(=21=CVI_SPI2_TX)。
  另外: 启动 DMA 前要先"中止"可能残留的脏通道(CH_EN 的 ABORT 位在 bit32, 写使能在 bit40),
        只有中止不掉才判"通道被占"(返回 0xE5)。

================================================================================
 七、变更记录 (后来者请往下追加)
================================================================================
格式: 日期 | 改动人 | 改了什么 | 完整指令 | 结果

2026-09-28 | (本会话) | Step0 排雷: 小核新增 sysdma_test.c, 启动时做一次 sysDMA
    内存到内存搬运自检, 结果写 g_dma_probe[16] (走热更部署, 零风险) |
    cd ~/duo-sdk-v2 && source build/envsetup_milkv.sh milkv-duo256m-glibc-arm64-sd
    cd build && make rtos
    scp ~/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin root@192.168.42.1:/root/cvirtos.bin
    ssh root@192.168.42.1 '/root/lcd_sender rtos /root/cvirtos.bin' |
    结果: build id 0x484A0003; g_dma_probe[13](结果码)=0、[11](不一致字节数)=0,
    即小核能完整驱动 sysDMA(借空闲通道 6, 不动 Linux dw_dmac)。fip 未动、系统未重启。
    注意: 探针地址随之变化, g_lvgl_probe=0x8fe6c660、g_dma_probe=0x8fec4750。

2026-09-28 | (本会话) | 记两处排障经验: IPCM 8 槽位耗尽导致 ENOBUFS(见 6.1);
    make rtos 复用陈旧对象导致改完不生效(见 6.3) |
    i=0; while [ $i -lt 8 ]; do a=$((0x1900400 + i*8)); devmem $(printf 0x%x $a) 64 0; i=$((i+1)); done |
    清槽位后 ping/热更立即恢复。

2026-09-28 | (前人) | 小核固件更新改为"常驻跳板 + 原地覆盖"热更, 从此免烧 fip、免重启 |
    /root/lcd_sender rtos /root/cvirtos.bin |
    验证通过: 换 build id 0x484A0001 的新固件热更成功,
    探针 probe[2] 圈数增长、probe[7] = 44.7 FPS, fip 未动、系统未重启。

2026-09-28 | (本会话) | Step1 修复: "SPI2 送显挂 sysDMA"之前完全没生效。根因是 DMA 请求线
    填错 —— 板级 DTS 写明 4=SPI2_RX、5=SPI2_TX, 送显只发不收, 必须用 5; 误填 4 会让通道
    永远卡在"已使能、等握手"。另加了"启动前先中止脏通道"(CH_EN 的 ABORT 位=32, 写使能=40),
    并按内核 slave_sg(M2P) 路径重配 CTL(删掉照 cyclic 误抄的 ARLEN/AWLEN)。
    改动: 小核 driver/spi/src/spi.c; task/lvgl/lvgl_task.c 仅把 build id +1 (0x484A0003 ->
    0x484A0004, 遵守"每改一版 +1"的约定)。详见 6.4 / 6.5 |
    # 1) 增量编译小核(先删旧 .obj, 见 6.3)
    cd /home/chen/duo-sdk-v2/freertos/cvitek/build/task && \
        rm -f lvgl/CMakeFiles/lvgl.dir/lvgl_task.c.obj && \
        cmake --build . --target install && cmake --build . --target cvirtos.bin
    # 2) 推到板子
    scp /home/chen/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin \
        root@192.168.42.1:/root/cvirtos.bin
    # 3) 板上: 先清 8 个 IPCM 槽位(见 6.1), 再热更
    i=0; while [ $i -lt 8 ]; do devmem $((0x1900400 + i*8)) 64 0; i=$((i+1)); done
    /root/lcd_sender ping
    /root/lcd_sender rtos /root/cvirtos.bin
    # 4) 验证
    devmem 0x8fe6cb90 32     # [14] build id 应 = 0x484A0004
    devmem 0x8fec8140 32     # [0]  应 = 0x53444D32 ('SDM2')
    devmem 0x8fec8158 32     # [3]  返回码应 = 0
    devmem 0x8fec8168 32     # [5]  回退 PIO 次数应 = 0
    devmem 0x04330820 64     # ch7 CFG 应含 SRC_PER=DST_PER=5
    |
    结果: 热更 200.1 ms、退出码 0; build id 读回 0x484A0004; g_spi2_probe[5]=0、[3]=0、
    [1](DMA 次数) 4 秒内 561 -> 1965 持续增长; ch7 CFG=0x7F8052810000000F(SRC_PER=DST_PER=5)。
    即 SPI2 送显已 100% 走 sysDMA、零 PIO 回退。fip 未动、系统未重启。
    本版探针地址: g_lvgl_probe=0x8fe6cb20, g_spi2_probe=0x8fec8140, g_dma_probe=0x8fec4c10。
    本版固件: 1640640 字节, md5=b63ef15c6ecbe0ea59121e4853824eec;
    常驻跳板末 2240 字节 md5=205ead534d848aea08b4e4cf2a03a8f1(与上一版逐字节相同 -> 热更安全)。

2026-09-28 | (本会话) | Step2 修复: 【帧率 1~2 FPS】。加完 sysDMA 送显后帧率反而从 ~44 掉到 1~2。
    根因: sysdma_wait_done() 只认 CH_INTSTATUS 的中断位, 但 DW DMAC 搬完后该中断位
    实测约 35% 概率读不到(硬件其实已经收摊: DMAC_CH_EN 的通道位自清 + CH_LLP 已到链尾 0),
    于是成功的搬运被误判为超时, 每次白等满 guard(实测单次 720ms)。实测 1292 次送显中 840 次
    "超时"。修法: 判据两条并用 —— 中断位【或】(CH_EN 通道位=0 且 CH_LLP=0); guard 从
    8000000 降到 2000000; 另加"连续 3 次真超时就把送显熔断回退 PIO"的保险(避免最坏情况卡死)。
    改动: 小核 driver/spi/src/spi.c; task/lvgl/lvgl_task.c 仅把 build id 0x484A0007 ->
    0x484A0008(遵守"每改一版 +1"的约定)。 |
    # 1) 增量编译小核(先删旧 .obj, 见 6.3)
    cd /home/chen/duo-sdk-v2/freertos/cvitek/build/task && \
        rm -f lvgl/CMakeFiles/lvgl.dir/lvgl_task.c.obj && \
        rm -f ../driver/CMakeFiles/spi.dir/src/spi.c.obj && \
        cmake --build . --target install && cmake --build . --target cvirtos.bin
    # 2) 推到板子
    scp /home/chen/duo-sdk-v2/freertos/cvitek/install/bin/cvirtos.bin \
        root@192.168.42.1:/root/cvirtos.bin
    # 3) 板上: 先清 8 个 IPCM 槽位(见 6.1), 再热更
    i=0; while [ $i -lt 8 ]; do devmem $((0x1900400 + i*8)) 64 0; i=$((i+1)); done
    /root/lcd_sender rtos /root/cvirtos.bin
    # 4) 验证
    devmem 0x8fe6ce90 32     # [14] build id 应 = 0x484A0008
    devmem 0x8fe6ce58 32     # [7]  帧率 x10, 应 ~0x33A (=826 -> 82.6 FPS)
    devmem 0x8fec3958 32     # g_dma2_probe[3] 真超时次数, 应 = 0
    devmem 0x8fec3a08 32     # g_dma2_probe[25] 单次最长等待 us, 修前 720315, 修后 ~13858
    |
    结果: 热更 200.1 ms、退出码 0; build id 读回 0x484A0008; fps_x10=0x33A(82.6 FPS)、
    4 秒内帧数 699 -> 1019; tmoN=0、maxdt 从 720315us 降到 13858us。fip 未动、系统未重启。
    本版探针地址: g_lvgl_probe=0x8fe6ce20, g_dma2_probe=0x8fec3940, g_spi2_probe=0x8fec3a20;
    常用偏移: buildid +0x70、fps_x10 +0x38、frames +0x40、cpu_small +0x20、loop +0x10;
    tmoN=g_dma2_probe+0x18、[24]=+0xc0、maxdt=+0xc8; dmaN=g_spi2_probe+0x08、rc=+0x18、
    回退N=+0x28。
    本版固件: 1640640 字节, md5=a921a56507a896960e6dded0aa0d7887(应用区 1638400 + 跳板 2240)。

2026-09-28 | (本会话) | 打包: 本工程全部落成可复现的 SD 卡镜像与源码仓库(仓库名 duo256m-lcd-rtos)。
    板上新增 /mnt/data/{auto.sh, cvirtos.bin, lcd_sender} 三个文件实现"开机自动热更小核
    固件 + 起 CPU 监控"; /etc/init.d/S99user 未改(原厂脚本本来就会调 $USERDATAPATH/auto.sh)。
    注意 /mnt/data/lcd_sender 必须与 /root/lcd_sender 同版本 —— 旧版没有 rtos 子命令,
    auto.sh 会打 "未知子命令: rtos" 并跳过热更(不影响启动, 只是屏幕跑的是 fip 里那版)。 |
    cp -f /root/lcd_sender /mnt/data/lcd_sender
    ls -l /boot/fip.bin /boot/boot.sd /mnt/data/auto.sh /mnt/data/cvirtos.bin /mnt/data/lcd_sender
    md5sum /boot/fip.bin /boot/boot.sd /mnt/data/auto.sh /mnt/data/lcd_sender /mnt/data/cvirtos.bin |
    结果: 板上 md5 —— fip.bin=a5a5ca0109d98e238ac82c0aae6d175a(1928192, 与历史记录一致)、
    boot.sd=7dd5a251414c10172e42a644f7177666(3338772)、auto.sh=d2f2084b8933889af89d801803978090、
    lcd_sender=2fcfe334988a4d578eb472978b3ddbee(5053208)、cvirtos.bin=a921a56507a896960e6dded0aa0d7887。
    整卡镜像(前 897 MiB, 含 MBR + p1 FAT 128M + p2 ext4 768M)见仓库 image/。
