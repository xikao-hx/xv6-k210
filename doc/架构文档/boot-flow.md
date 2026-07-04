# K210 启动流程：从 RustSBI 到 xv6 kernel

## 1. 启动链路概览

K210 上的启动链路可以概括为：

```text
K210 Boot ROM
  -> RustSBI
  -> xv6 kernel entry
  -> main()
  -> 内核各子系统初始化
  -> userinit()
  -> init
  -> shell
```

本项目中，RustSBI 承担机器模式下的基础服务，xv6 kernel 主要运行在 S-mode。内核通过 SBI 调用完成部分底层能力，例如字符输出、IPI、外部中断相关控制等。

## 2. 镜像入口与链接地址

K210 平台使用：

- 入口文件：`kernel/entry_k210.S`
- 链接脚本：`linker/k210.ld`
- 入口符号：`_start`
- 内核链接地址：`0x80020000`

QEMU 平台使用：

- 入口文件：`kernel/entry_qemu.S`
- 链接脚本：`linker/qemu.ld`
- 入口符号：`_entry`
- 内核链接地址：`0x80200000`

这两个链接地址不同，是平台适配时必须注意的点。入口符号也不同，因此 `Makefile` 会根据 `platform` 选择不同的 entry object 和 linker script。

## 3. 汇编入口做了什么

K210 的入口代码位于 `kernel/entry_k210.S`，核心逻辑是：

```text
_start:
  根据 a0 中的 hartid 计算当前 hart 的栈位置
  设置 sp
  call main
  如果 main 返回，则进入死循环
```

入口处使用：

```text
sp = boot_stack + ((hartid + 1) << 14)
```

也就是每个 hart 使用 16 KiB 栈空间。`boot_stack` 在 `.bss.stack` 中分配。

QEMU 的 `entry_qemu.S` 逻辑类似，只是入口符号为 `_entry`。

## 4. main() 参数

`main()` 定义在 `kernel/main.c`：

```c
void main(unsigned long hartid, unsigned long dtb_pa)
```

其中：

- `hartid` 来自启动时传入的 hart id。
- `dtb_pa` 是设备树物理地址，目前项目中没有作为主要配置来源使用。

`main()` 开始时会调用 `inithartid(hartid)`，把 hart id 写入 `tp`：

```text
tp = hartid & 0x1
```

后续 xv6 通过 `tp` 区分当前 CPU。

## 5. hart 0 初始化流程

hart 0 负责完成系统级初始化，顺序大致如下：

```text
consoleinit()
printfinit()
print_logo()

kinit()
kvminit()
kvminithart()

procinit()
trapinit()
trapinithart()

plicinit()
plicinithart()

K210: sbi_set_mie()

binit()
fileinit()

K210:
  fpioa_pin_init()
  dmac_init()
  spidev_init()
  i2cdev_init()
  sdcarddev_init()
  uartdev_init()

disk_init()
userinit()
唤醒其他 hart
scheduler()
```

这些初始化可以按职责分为几类：

| 类别 | 相关函数 | 说明 |
|------|----------|------|
| 输出与调试 | `consoleinit`、`printfinit` | 建立早期输出能力 |
| 内存 | `kinit`、`kvminit`、`kvminithart` | 物理页分配和页表 |
| 进程 | `procinit`、`userinit` | 初始化进程表并创建第一个用户进程 |
| 异常中断 | `trapinit`、`trapinithart`、`plicinit` | trap 向量和外部中断 |
| 文件系统 | `binit`、`fileinit`、`disk_init` | buffer cache、文件表、磁盘 |
| K210 外设 | `fpioa_pin_init`、`dmac_init` 等 | 注册和初始化真实硬件 |

## 6. K210 平台特有初始化

K210 平台下，`main()` 会额外执行：

```text
fpioa_pin_init()
dmac_init()
spidev_init()
i2cdev_init()
sdcarddev_init()
uartdev_init()
```

其中：

- `fpioa_pin_init()` 负责把 K210 的外设功能映射到实际引脚。
- `dmac_init()` 初始化 DMA 控制器。
- `spidev_init()`、`i2cdev_init()`、`sdcarddev_init()`、`uartdev_init()` 注册用户态可访问的设备接口。
- `disk_init()` 在 K210 下会调用 `sdcard_init()`，让 FAT32 能通过 SD 卡读写块设备。

K210 上还需要 `sbi_set_mie()` 打开 M-mode 外部中断，因为 RustSBI 默认可能没有为内核打开对应中断入口。

## 7. 多核启动

hart 0 初始化完成后，会通过 SBI 发送 IPI 给其他 hart：

```text
for each hart:
  sbi_send_ipi()
```

其他 hart 在 `started` 变量变为 1 前自旋等待。被唤醒后执行：

```text
kvminithart()
trapinithart()
plicinithart()
scheduler()
```

也就是说，系统级资源由 hart 0 初始化，其他 hart 只完成本 hart 必需的页表、中断向量和 PLIC 配置，然后进入调度器。

## 8. 第一个用户进程

`userinit()` 会创建第一个用户态进程，加载 `initcode`。随后用户态 `init` 启动 shell。此后用户可以运行：

```text
ls
cat
burn
mpu6050
w25q64
```

这些程序通过系统调用进入内核，访问 FAT32 文件系统或 K210 外设设备。

## 9. 面试讲述重点

可以按这条线讲：

1. K210 上电后先进入 Boot ROM，再进入 RustSBI。
2. RustSBI 准备好基本运行环境后跳转到 xv6 kernel 的 `_start`。
3. `_start` 设置每个 hart 的启动栈，然后调用 `main()`。
4. hart 0 初始化内存、页表、trap、PLIC、buffer cache、文件表和外设。
5. K210 平台额外初始化 FPIOA、DMAC、SPI/I2C/SD/UART 设备。
6. `disk_init()` 把 FAT32 底层块设备接到 SD 卡。
7. `userinit()` 创建第一个用户进程，最终进入 shell。

这部分适合回答：“开发板上电后你的系统是怎么跑起来的？”

