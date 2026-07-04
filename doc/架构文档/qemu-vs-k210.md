# QEMU 与 K210 真板适配差异

## 1. 为什么要区分 QEMU 和 K210

QEMU 和 K210 都运行 RISC-V 内核，但它们的启动方式、外设、中断、存储设备和串口行为都有差异。

QEMU 的价值：

- 构建和启动快。
- 便于调试通用内核逻辑。
- 适合验证进程、系统调用、FAT32、用户程序等平台无关代码。

K210 的价值：

- 能验证真实硬件驱动。
- 能暴露串口、SPI、I2C、SD 卡、DMA、中断等真板问题。
- 更贴近嵌入式驱动开发岗位。

因此本项目同时保留两个平台。

## 2. 编译差异

`Makefile` 通过 `platform` 参数区分平台：

```sh
make build platform=qemu
make build platform=k210
```

QEMU 下会定义 `QEMU` 宏：

```make
ifeq ($(platform), qemu)
CFLAGS += -D QEMU
endif
```

代码中用 `#ifdef QEMU` 和 `#ifndef QEMU` 切换平台相关路径。

## 3. 入口文件差异

| 平台 | 入口文件 | 入口符号 |
|------|----------|----------|
| QEMU | `kernel/entry_qemu.S` | `_entry` |
| K210 | `kernel/entry_k210.S` | `_start` |

两个入口文件都完成：

- 根据 hart id 设置启动栈。
- 调用 `main()`。
- 如果 `main()` 返回，则进入死循环。

差异主要在入口符号和链接脚本配合。

## 4. 链接地址差异

| 平台 | 链接脚本 | 起始地址 |
|------|----------|----------|
| QEMU | `linker/qemu.ld` | `0x80200000` |
| K210 | `linker/k210.ld` | `0x80020000` |

链接地址必须和 Bootloader 或平台加载内核的位置匹配。地址错误时，常见现象是：

- 内核没有任何输出。
- 跳转后立即异常。
- 代码和数据访问地址不符合预期。

## 5. Bootloader 差异

QEMU 和 K210 使用不同的 RustSBI：

| 平台 | RustSBI |
|------|---------|
| QEMU | `bootloader/sbi-qemu` |
| K210 | `bootloader/sbi-k210` |

K210 上还需要注意 RustSBI 对外部中断的默认配置。本项目在 K210 下调用 `sbi_set_mie()`，用于打开 M-mode 外部中断相关能力。

## 6. 存储设备差异

QEMU：

```text
FAT32
  -> bio
  -> disk.c
  -> virtio_disk.c
  -> virtio block
```

K210：

```text
FAT32
  -> bio
  -> disk.c
  -> sdcard.c
  -> spi.c / dmac.c
  -> SD card
```

这是两个平台最重要的差异之一。QEMU 下很多文件系统问题可能是纯软件问题；K210 下还可能涉及 SPI、SD 卡初始化、DMA、中断和硬件时序。

## 7. 外设差异

QEMU 主要使用：

- 16550A UART。
- virtio disk。
- QEMU virt 平台的 PLIC。

K210 主要使用：

- UARTHS。
- SPI 控制器。
- I2C 控制器。
- SD 卡。
- DMAC。
- FPIOA。
- GPIOHS。
- SYSCTL。

因此 K210 平台编译时会额外加入：

```text
spi.o
i2c.o
spidev.o
i2cdev.o
sdcarddev.o
uartdev.o
gpiohs.o
fpioa.o
utils.o
sdcard.o
dmac.o
sysctl.o
```

## 8. 控制台与 UART 差异

QEMU 使用 16550A UART，K210 使用 UARTHS。二者寄存器和中断行为不同。

K210 上还遇到过一个典型输入差异：

```text
Enter 会发送 '\n' 后跟 '\r'
```

如果内核 console 不过滤 `\r`，shell 可能出现异常输入行为。项目中在 K210 分支对 `\r` 做了过滤。

UART 烧写时，普通 console 路径不适合二进制流，因此 K210 上实现了 raw UART 模式，把 RX 中断数据写入独立 ring buffer。

## 9. 中断差异

QEMU 和 K210 都有 PLIC，但外设中断来源不同。

K210 上需要重点关注：

- RustSBI 是否允许外部中断进入。
- PLIC 是否正确 enable 对应 IRQ。
- hart 上是否正确打开中断。
- UART、DMA、SPI 等外设中断是否清除。

真板中断问题经常表现为：

- 驱动初始化成功，但没有后续中断。
- 第一次中断后卡死。
- QEMU 正常，K210 不触发。

## 10. 调试方法差异

QEMU 适合：

- 快速构建启动。
- 验证系统调用和文件系统逻辑。
- 使用可重复的虚拟磁盘环境。

K210 适合：

- 验证外设寄存器配置。
- 用串口日志观察启动和中断。
- 用逻辑分析仪观察 I2C/SPI 波形。
- 用 OLED 显示关键阶段，辅助排查串口被协议占用时的问题。

建议定位问题时先问：

```text
这个问题在 QEMU 也复现吗？
```

如果 QEMU 复现，多半是通用内核逻辑问题。如果只在 K210 复现，优先怀疑平台适配、外设、中断、时序或缓存一致性。

## 11. 面试讲述重点

可以这样讲：

1. QEMU 和 K210 共用大部分内核代码，但启动地址、入口符号、链接脚本、外设驱动不同。
2. QEMU 使用 virtio disk，K210 使用 SPI SD 卡，因此我在 `disk.c` 做了统一块设备抽象。
3. K210 有真实硬件问题，例如 UART 输入差异、SD 卡初始化、I2C 时序、外部中断配置。
4. 我通常先在 QEMU 验证通用逻辑，再在真板验证驱动和时序问题。
5. 双平台支持让我能区分“内核逻辑 bug”和“硬件适配 bug”。

这部分适合回答：“QEMU 能跑为什么真板不一定能跑？”、“你做过哪些真实硬件适配？”

