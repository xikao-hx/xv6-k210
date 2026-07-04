# xv6-k210 项目整体架构

## 1. 项目定位

本项目是在 xv6-riscv 基础上进行的嵌入式移植与外设驱动开发实践，目标是让一个类 Unix 教学内核同时支持 QEMU virt 平台和 K210 开发板。

对秋招嵌入式 Linux 驱动开发岗位来说，本项目的重点不只是“能运行 xv6”，而是体现以下能力：

- 理解从 Bootloader 到 Kernel，再到用户态程序的完整启动链路。
- 能够在 QEMU 和真实开发板之间处理平台差异。
- 能够编写 UART、SPI、I2C、SD 卡、DMA、中断等底层驱动。
- 能够通过系统调用、文件对象、`devsw[]` 和 `ioctl` 向用户态暴露设备能力。
- 能够进行真板调试、协议设计、错误恢复和稳定性验证。

## 2. 总体分层

项目可以按以下层次理解：

```text
用户态程序
  init / sh / ls / cat / burn / mpu6050 / w25q64
        |
        v
用户态 libc 与系统调用封装
  user/libc / user/usys.pl / user/include/user.h
        |
        v
系统调用层
  kernel/syscall/syscall.c
  kernel/syscall/sysfile.c
  kernel/syscall/sysproc.c
        |
        v
内核对象与抽象层
  proc / file / pipe / fat32 / bio / devsw
        |
        v
平台无关驱动接口
  disk.c / console.c / uartdev.c / spidev.c / i2cdev.c / sdcarddev.c
        |
        v
平台相关底层驱动
  QEMU: virtio_disk
  K210: uarths / spi / i2c / sdcard / dmac / fpioa / gpiohs / sysctl
        |
        v
硬件或模拟平台
  QEMU virt machine / K210 board
```

这条链路是面试中讲项目的主线：用户程序不是直接操作硬件，而是通过系统调用进入内核，再由文件对象和设备表分发到对应驱动。

## 3. 目录结构

主要目录如下：

| 目录 | 作用 |
|------|------|
| `kernel/` | 内核代码主体 |
| `kernel/driver/` | 平台相关底层驱动，如 UART、SPI、I2C、SD 卡、DMA |
| `kernel/devsw/` | 面向文件接口的设备抽象层 |
| `kernel/fs/` | FAT32 文件系统、buffer cache、磁盘抽象、文件对象 |
| `kernel/proc/` | 进程、调度、exec |
| `kernel/syscall/` | 系统调用分发与具体实现 |
| `kernel/trap/` | trap、中断、trampoline |
| `kernel/vm/` | 页表、物理内存分配、用户/内核拷贝 |
| `user/` | 用户程序、shell、用户态 libc |
| `tools/` | 文件系统镜像生成、烧写、下载工具 |
| `linker/` | QEMU 与 K210 对应链接脚本 |
| `bootloader/` | RustSBI 相关二进制 |

## 4. 双平台支持

工程通过 `platform` 参数区分 QEMU 和 K210：

```sh
make build platform=qemu
make build platform=k210
```

`Makefile` 中的主要差异：

- QEMU 使用 `kernel/entry_qemu.S`，链接脚本为 `linker/qemu.ld`，编译时定义 `QEMU` 宏。
- K210 使用 `kernel/entry_k210.S`，链接脚本为 `linker/k210.ld`，并编译 SPI、I2C、SD 卡、DMAC、FPIOA、GPIOHS、SYSCTL 等驱动。
- QEMU 的块设备来自 `virtio_disk.c`。
- K210 的块设备来自 `sdcard.c`，底层依赖 SPI 和 DMAC。

`kernel/fs/disk.c` 提供统一磁盘接口：

```text
disk_init()
disk_read()
disk_write()
disk_intr()
```

在 QEMU 下转发到 virtio，在 K210 下转发到 SD 卡驱动。这样 FAT32 和 buffer cache 不需要感知底层设备差异。

## 5. 启动与初始化流程

内核入口在汇编文件中完成最小初始化：

- 根据 hart id 设置启动栈。
- 跳转到 `main(hartid, dtb_pa)`。

`kernel/main.c` 中 hart 0 完成主要初始化：

```text
consoleinit()
printfinit()
kinit()
kvminit()
kvminithart()
procinit()
trapinit()
trapinithart()
plicinit()
plicinithart()
binit()
fileinit()
K210 外设设备注册
disk_init()
userinit()
scheduler()
```

K210 平台还会初始化 FPIOA、DMAC、SPI/I2C/SD/UART 设备抽象，并通过 SBI 打开 M-mode 外部中断。

## 6. 文件系统与存储路径

项目使用 FAT32 文件系统。用户程序通过 `open/read/write/exec` 等系统调用访问文件，内核路径大致如下：

```text
sys_read / sys_write
  -> fileread / filewrite
  -> FAT32 entry 操作
  -> buffer cache
  -> disk_read / disk_write
  -> QEMU virtio 或 K210 SD card
```

K210 上 SD 卡同时承担两种角色：

- 作为 FAT32 文件系统的底层块设备。
- 作为 UART 烧写 `fs.img` 的目标设备。

UART 烧写会绕过 FAT32 和 buffer cache，直接通过 `DEV_SDCARD` 按扇区写 SD 卡。因此烧写完成后需要调用缓存失效接口，避免内核继续使用旧的 FAT32 缓存。

## 7. 设备抽象层

内核用 `devsw[]` 维护设备主设备号到设备操作函数的映射：

```c
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
  int (*ioctl)(int, uint64, uint64);
};
```

当前设备号包括：

| 设备 | 主设备号 | 说明 |
|------|----------|------|
| `DEV_CONSOLE` | 1 | 控制台 |
| `DEV_STATS` | 2 | 统计信息 |
| `DEV_SPI` | 3 | SPI 用户态访问 |
| `DEV_I2C` | 4 | I2C 用户态访问 |
| `DEV_SDCARD` | 5 | SD 卡 raw 扇区访问 |
| `DEV_UART` | 6 | raw UART 二进制收发 |

这部分可以类比 Linux 字符设备：

```text
Linux:
  app -> syscall -> VFS -> file_operations -> driver

xv6-k210:
  app -> syscall -> file/devsw -> read/write/ioctl -> driver
```

## 8. 当前项目亮点

适合在简历和面试中重点讲的内容：

- 支持 QEMU 和 K210 双平台运行。
- 完成 RustSBI 到 xv6 kernel 的启动适配。
- 支持 FAT32 文件系统和用户态 shell。
- K210 上实现 SD 卡 SPI 驱动，并接入统一块设备接口。
- 实现 SPI、I2C、UART、SD 卡等设备的用户态访问接口。
- 实现 raw UART 模式和 ring buffer，用于二进制传输。
- 实现 UART 下载 `fs.img` 并写入 SD 卡的烧写闭环。
- 通过 `burn`、`mpu6050`、`w25q64` 等用户程序验证外设驱动。

## 9. 后续完善方向

后续可以继续补强：

- 外设测试命令体系：`sdtest`、`uarttest`、`spitest`、`i2ctest`、`dmactest`。
- 统一设备号、`ioctl` 编号、错误码和接口语义。
- 补充 UART 烧写不同 baud 下的成功率、吞吐和失败统计。
- 补充 SD 卡读写速度、SPI 频率稳定性、I2C 设备访问成功率。
- 整理调试复盘，重点记录真板上出现而 QEMU 不出现的问题。

