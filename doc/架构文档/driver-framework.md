# 驱动框架与用户态访问路径

## 1. 设计目标

本项目希望用户态程序可以像访问文件一样访问设备，而不是直接在用户态操作硬件寄存器。

整体访问路径是：

```text
用户程序
  -> open/dev/read/write/ioctl
  -> syscall
  -> struct file
  -> devsw[major]
  -> 设备抽象层
  -> 底层硬件驱动
  -> 硬件
```

这条路径可以和 Linux 驱动模型类比：

```text
Linux:
  app -> syscall -> VFS -> file_operations -> driver

xv6-k210:
  app -> syscall -> file/devsw -> read/write/ioctl -> driver
```

面试时可以用这个类比说明：项目虽然不是 Linux 内核，但驱动分层思想和用户态访问模型与嵌入式 Linux 驱动开发高度相关。

## 2. 文件对象

内核中文件对象定义在 `kernel/include/file.h`：

```c
struct file {
  enum { FD_NONE, FD_PIPE, FD_ENTRY, FD_DEVICE } type;
  int ref;
  char readable;
  char writable;
  struct pipe *pipe;
  struct dirent *ep;
  uint off;
  short major;
  short minor;
};
```

其中：

- `FD_ENTRY` 表示 FAT32 文件或目录。
- `FD_PIPE` 表示管道。
- `FD_DEVICE` 表示设备文件。
- `major` 决定访问哪个设备驱动。
- `minor` 用于区分同类设备下的具体实例，例如 SPI bus 和 chip select。

## 3. devsw 设备表

设备表定义如下：

```c
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
  int (*ioctl)(int, uint64, uint64);
};
```

公共设备 ABI 已整理到 `kernel/include/dev.h`。当前设备号：

| 设备 | 主设备号 | 文件 |
|------|----------|------|
| `DEV_CONSOLE` | 1 | `kernel/devsw/console.c` |
| `DEV_STATS` | 2 | `kernel/devsw/stats.c` |
| `DEV_SPI` | 3 | `kernel/devsw/spidev.c` |
| `DEV_I2C` | 4 | `kernel/devsw/i2cdev.c` |
| `DEV_SDCARD` | 5 | `kernel/devsw/sdcarddev.c` |
| `DEV_UART` | 6 | `kernel/devsw/uartdev.c` |

更完整的设备号、minor 编码、read/write/ioctl 语义见 `doc/架构文档/devsw-ioctl-interface.md`。

设备初始化时会把自己的 `read/write/ioctl` 注册到 `devsw[]` 中。例如 `uartdev_init()`：

```text
devsw[DEV_UART].read  = uartdev_read
devsw[DEV_UART].write = uartdev_write
devsw[DEV_UART].ioctl = uartdev_ioctl
```

## 4. read/write 分发路径

以用户程序调用 `read(fd, buf, n)` 为例：

```text
user read()
  -> ecall
  -> sys_read()
  -> argfd() 找到当前进程的 struct file
  -> fileread()
  -> 根据 f->type 分发
```

`fileread()` 中的逻辑：

```text
FD_PIPE:
  piperead()

FD_DEVICE:
  devsw[f->major].read()

FD_ENTRY:
  FAT32 eread()
```

`write()` 路径类似：

```text
user write()
  -> sys_write()
  -> filewrite()
  -> FD_DEVICE 时调用 devsw[f->major].write()
```

这说明设备和普通文件在用户态入口上是统一的，差异在内核 `struct file` 的类型和分发函数中体现。

## 5. ioctl 的作用

`read/write` 适合传输连续数据，但很多设备需要配置、控制和状态查询，因此项目中为 `devsw` 增加了 `ioctl` 函数指针。

典型用途：

- UART：进入 raw 模式、退出 raw 模式、设置 baud、查询实际 baud。
- SPI：初始化控制器、发起一次 transfer。
- I2C：初始化 bus/slave、发起多个 message 的 transfer。
- SD 卡：seek 到指定扇区、查询当前扇区、查询容量、失效缓存。

这种设计可以类比 Linux 中字符设备的 `unlocked_ioctl`。

## 6. UART 设备

相关文件：

- `kernel/devsw/uartdev.c`
- `kernel/driver/uarths.c`
- `kernel/include/uartdev.h`

主要接口：

| ioctl | 作用 |
|------|------|
| `UART_IOCTL_RAW_START` | 将 UART RX 从 console 路径切换到 raw ring buffer |
| `UART_IOCTL_RAW_END` | 退出 raw 模式，恢复 console 输入 |
| `UART_IOCTL_SET_BAUD` | 设置 UART baud |
| `UART_IOCTL_GET_BAUD_INFO` | 查询 requested、actual、div、clock |

在 raw 模式下：

```text
UART RX interrupt
  -> uartintr()
  -> uart_raw_input()
  -> raw ring buffer
  -> uartdev_read()
  -> 用户态 read()
```

这个设备支撑了 `burn` 用户程序的二进制协议传输。

## 7. SD 卡设备

相关文件：

- `kernel/devsw/sdcarddev.c`
- `kernel/driver/sdcard.c`
- `kernel/include/sdcarddev.h`

接口语义：

- `read(fd, buf, 512)`：从当前扇区读取 512 字节，然后扇区号自增。
- `write(fd, buf, 512)`：向当前扇区写 512 字节，然后扇区号自增。
- `SDCARD_IOCTL_SEEK`：设置当前扇区。
- `SDCARD_IOCTL_TELL`：查询当前扇区。
- `SDCARD_IOCTL_NSECTORS`：查询 SD 卡总扇区数。
- `SDCARD_IOCTL_INVALIDATE_CACHE`：raw 写盘后失效 FAT32 和 buffer cache。

该设备用于 `burn` 直接写入 `fs.img`，绕过 FAT32 层。

## 8. SPI 设备

相关文件：

- `kernel/devsw/spidev.c`
- `kernel/driver/spi.c`
- `kernel/include/spidev.h`

minor 编码：

```text
minor = (spi_bus << 2) | chip_select
```

主要 ioctl：

- `SPI_IOCTL_INIT`：初始化 SPI 控制器。
- `SPI_IOCTL_TRANSFER`：发起一次 SPI 传输。

`struct spidev_transfer` 使用用户态地址传入 TX/RX buffer，支持 command + receive 形式的事务，适合 W25Q64 这类 SPI Flash 访问。

## 9. I2C 设备

相关文件：

- `kernel/devsw/i2cdev.c`
- `kernel/driver/i2c.c`
- `kernel/include/i2cdev.h`

主要 ioctl：

- `I2C_IOCTL_INIT`：设置 I2C clock 和默认 slave 地址。
- `I2C_IOCTL_TRANSFER`：发起 I2C message 传输。

`struct i2c_transfer` 最多支持 2 个 message，适合常见寄存器读写模式：

```text
写寄存器地址
读寄存器数据
```

这部分可用于 MPU6050、OLED 等 I2C 外设。

## 10. 面试讲述重点

可以这样讲：

1. 我没有让用户程序直接访问寄存器，而是通过系统调用进入内核。
2. 内核用 `struct file` 表示文件、管道和设备。
3. 对设备文件，`read/write/ioctl` 会根据主设备号分发到 `devsw[]`。
4. `devsw[]` 类似 Linux 的 `file_operations`。
5. SPI/I2C/UART/SD 卡各自实现设备抽象层，底下再调用具体 K210 驱动。
6. `burn` 程序就是这个框架的综合使用：UART 收包，SD 卡写扇区，最后失效文件系统缓存。

这部分适合回答：“你的驱动怎么给用户态使用？”、“这个项目和 Linux 驱动有什么关系？”
