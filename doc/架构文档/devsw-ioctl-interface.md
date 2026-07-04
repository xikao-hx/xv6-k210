# devsw/ioctl 设备抽象层

## 目标

本项目通过 `dev()`、`read()`、`write()`、`ioctl()` 向用户态暴露板端设备能力，避免用户程序直接访问硬件寄存器。

调用链可以类比 Linux 字符设备：

```text
Linux:
  app -> syscall -> VFS -> file_operations -> driver

xv6-k210:
  app -> syscall -> struct file -> devsw[major] -> devsw device -> hardware driver
```

## 公共 ABI

公共设备 ABI 放在 `kernel/include/dev.h`，同时供内核和用户程序包含。

主设备号：

| 宏 | 主设备号 | 设备 |
| --- | ---: | --- |
| `DEV_CONSOLE` | 1 | console |
| `DEV_STATS` | 2 | stats |
| `DEV_SPI` | 3 | SPI |
| `DEV_I2C` | 4 | I2C |
| `DEV_SDCARD` | 5 | SD card raw block device |
| `DEV_UART` | 6 | raw UART |

minor 编码：

| 设备 | 编码宏 | 说明 |
| --- | --- | --- |
| SPI | `SPI_MINOR(bus, chip_select)` | `minor = (bus << 2) | chip_select` |
| I2C | `I2C_MINOR(bus)` | `minor = bus & 3` |

## 用户态头文件

| 头文件 | 内容 |
| --- | --- |
| `dev.h` | 主设备号、minor 编码宏 |
| `uartdev.h` | UART ioctl 命令和统计结构 |
| `sdcarddev.h` | SD card ioctl 命令 |
| `spidev.h` | SPI ioctl 命令和 `struct spidev_transfer` |
| `i2cdev.h` | I2C ioctl 命令和 message/transfer 结构 |

用户程序入口示例：

```c
int fd = dev(O_RDWR, DEV_UART, 0);
ioctl(fd, UART_IOCTL_GET_BAUD_INFO, (uint64)&info);
```

```c
int fd = dev(0, DEV_SPI, SPI_MINOR(1, 0));
ioctl(fd, SPI_IOCTL_TRANSFER, (uint64)&xfer);
```

## 设备语义

| 设备 | read | write | ioctl |
| --- | --- | --- | --- |
| console | 读 console 输入 | 写 console 输出 | 无 |
| stats | 读内核统计信息 | 不支持 | 无 |
| UART | 从 raw ring 读数据 | 同步发送数据 | raw 模式、baud 设置、baud/raw stats 查询 |
| SD card | 读当前 512B sector 并自增 | 写当前 512B sector 并自增 | seek/tell/nsectors/invalidate cache |
| SPI | 不支持 | 不支持 | init/transfer |
| I2C | 不支持 | 不支持 | init/transfer |

## ioctl 命令

UART：

| 命令 | 参数 | 语义 |
| --- | --- | --- |
| `UART_IOCTL_RAW_START` | 0 | RX 从 console 切到 raw ring |
| `UART_IOCTL_RAW_END` | 0 | 退出 raw 模式并恢复 console |
| `UART_IOCTL_SET_BAUD` | baud 值 | 设置 UART baud |
| `UART_IOCTL_GET_BAUD_INFO` | `struct uart_baud_info *` | 查询 requested/actual/div/clock |
| `UART_IOCTL_GET_RAW_STATS` | `struct uart_raw_stats *` | 查询 dropped/buffered/capacity/mode |

SD card：

| 命令 | 参数 | 语义 |
| --- | --- | --- |
| `SDCARD_IOCTL_SEEK` | sector number | 设置当前 raw sector |
| `SDCARD_IOCTL_TELL` | `uint32 *` | 返回当前 raw sector |
| `SDCARD_IOCTL_NSECTORS` | `uint32 *` | 返回总 sector 数 |
| `SDCARD_IOCTL_INVALIDATE_CACHE` | 0 | raw 写盘后失效 FAT32 和 buffer cache |

SPI：

| 命令 | 参数 | 语义 |
| --- | --- | --- |
| `SPI_IOCTL_INIT` | `uint32 *clk_rate` | 初始化 SPI 控制器 |
| `SPI_IOCTL_TRANSFER` | `struct spidev_transfer *` | 发起一次 send/receive/command+receive 事务 |

I2C：

| 命令 | 参数 | 语义 |
| --- | --- | --- |
| `I2C_IOCTL_INIT` | `struct i2cdev_init *` | 初始化 I2C bus 和默认 slave |
| `I2C_IOCTL_TRANSFER` | `struct i2c_transfer *` | 发起 1 到 2 个 I2C message |

## 结构约束

- `kernel/devsw/*.c` 只做设备文件语义转换，不把用户协议下沉到底层驱动。
- `kernel/driver/*.c` 只处理硬件寄存器和传输细节，不依赖用户态协议。
- 用户态只通过 `dev/read/write/ioctl` 访问设备，不直接访问驱动全局变量。
- 新增设备时必须先分配主设备号，再新增设备专用头文件和 `devsw[]` 注册函数。
- 新增 ioctl 时优先保持设备内编号连续，并在本文件记录参数和语义。
