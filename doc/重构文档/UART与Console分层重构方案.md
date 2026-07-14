# UART 与 Console 分层重构方案

日期：2026-07-11  
范围：`kernel/driver/uarths.c`、`kernel/devsw/console.c`、`kernel/devsw/uartdev.c` 及相关用户态接口  
目标平台：QEMU 16550A 与 K210 UARTHS

---

## 1. 重构目标

将当前 UART 驱动中的“硬件操作、字节缓冲、console 语义、raw 设备路由”拆分为两层：

1. `uarths.c` 作为纯 UART 字节流驱动，只负责硬件和统一 RX/TX 软件环形缓冲区。
2. `console.c` 作为简化的 TTY 层，通过 ioctl 在 TTY 模式和 RAW 模式之间切换。

重构后只保留 `/dev/console` 作为用户态串口入口，删除独立的 `/dev/uart` 抽象。TTY 和 RAW 模式消费同一个 UART RX ring，避免当前 `cons.buf[]` 与 `uart_raw_buf[]` 两套接收缓冲及中断路由逻辑。

---

## 2. 当前设计的问题

### 2.1 UART 驱动依赖 console

`uartintr()` 根据 `uart_raw_mode` 直接选择 `uart_raw_input()` 或 `consoleintr()`，导致底层 UART 驱动知道上层 console 语义。这使 UART 很难被其他字节流消费者复用，也使模式切换、缓冲和中断控制混在同一文件中。

### 2.2 RX 存在两套用途互斥的缓冲区

- `console.c` 使用 128 字节 `cons.buf[]`。
- `uarths.c` 使用 32768 字节 `uart_raw_buf[]`。
- 中断根据 `uart_raw_mode` 选择其中一个。

两个缓冲区并不同时提供服务，却需要两套锁、索引、睡眠唤醒和清空逻辑。

### 2.3 TX ring 存在但未进入主要写路径

`uarths.c` 已有 32 字节 `uart_tx_buf[]`，但 `consolewrite()` 和 `uartdev_write()` 都逐字节调用 `uartputc_sync()`。K210 初始化又关闭了 TX watermark 中断，因此现有 buffered TX 不是完整可用的发送链路。

### 2.4 `/dev/console` 和 `/dev/uart` 暴露同一硬件的两套语义

`DEV_CONSOLE` 和 `DEV_UART` 最终操作同一 UART0，但用户程序需要切换设备节点才能获得 raw 能力。更合理的接口是对同一 console fd 设置终端模式，类似 Linux 通过 termios ioctl 修改同一 TTY 设备的工作方式。

---

## 3. 目标分层

```text
用户程序（shell / burn / tests）
              │
              │ read / write / ioctl
              ▼
     /dev/console（console.c）
       ├── TTY 模式：终端语义
       └── RAW 模式：原始字节流
              │
              │ uart_read / uart_write
              ▼
        uarths.c（纯驱动）
       ├── 统一 RX ring
       ├── 统一 TX ring
       ├── RX/TX 中断
       └── 波特率与硬件状态
              │
              ▼
       QEMU 16550A / K210 UARTHS
```

依赖方向必须保持为 `console.c -> uarths.c`。`uarths.c` 不再 `#include "console.h"`，也不再调用 `consoleintr()`。

---

## 4. `uarths.c` 设计

### 4.1 驱动状态

建议使用独立 RX/TX 锁，保证收发双向不互相阻塞：

```c
#define UART_RX_BUF_SIZE 32768
#define UART_TX_BUF_SIZE 4096

struct uart_ring {
  char *data;
  uint size;
  uint r;
  uint w;
};

struct uart_state {
  struct spinlock rx_lock;
  struct spinlock tx_lock;
  struct uart_ring rx;
  struct uart_ring tx;
  char rx_data[UART_RX_BUF_SIZE];
  char tx_data[UART_TX_BUF_SIZE];
  uint rx_dropped;
  uint requested_baud;
};
```

缓冲区大小使用 2 的幂，索引回绕可使用位掩码。第一版也可继续使用 `% size`，优先保证正确性。

RX 容量暂时保持 32768 字节，避免高波特率烧录回归。TX 建议从 4096 字节起步，与用户态当前单次 4096 字节传输上限匹配。若 K210 内存压力需要更小，可在量化后调整为 1024 字节。

### 4.2 RX 中断

RX 中断只负责将硬件 FIFO 搬入统一 RX ring：

```c
static void
uart_rx_intr(void)
{
  int c;
  int received = 0;

  acquire(&uart.rx_lock);
  while ((c = uartgetc_hw()) != -1) {
    if (uart_rx_ring_full())
      uart.rx_dropped++;
    else {
      uart_rx_ring_put((char)c);
      received = 1;
    }
  }
  if (received)
    wakeup_reason(&uart.rx.r, WAKEUP_DEVICE);
  release(&uart.rx_lock);
}
```

中断中不判断 TTY/RAW 模式，不处理回显、控制字符或换行转换。

### 4.3 RX 读接口

```c
int uart_read(char *dst, int n);
int uart_try_read(char *dst, int n);
void uart_flush_rx(void);
void uart_get_rx_stats(struct uart_rx_stats *stats);
```

`uart_read()` 的语义建议为：

- RX ring 为空时睡眠，并响应当前进程的 `killed` 状态。
- 至少有 1 字节后，一次返回当前可用的最多 `n` 字节，不强制等待填满 `n`。
- 只在进程上下文中调用，不在中断中调用。
- `uart_try_read()` 在无数据时立即返回 0。

这个语义既适用交互式 console，也适用 `burn.c` 的上层 `read_bytes()` 循环。

### 4.4 TX 发送链路

新增批量写接口：

```c
int uart_write(const char *src, int n);
void uart_flush_tx(void);
void uartputc_sync(int c);
```

`uart_write()` 将数据批量放入 TX ring。ring 满时允许进程睡眠，TX 中断取走数据后唤醒写者。`uartputc_sync()` 仅保留给以下场景：

- panic 输出。
- 内核早期启动。
- 不允许睡眠且必须立即输出的特殊上下文。

TX 中断必须按需管理：

- TX ring 从空变为非空时开启 TX watermark/THRE 中断。
- 中断处理中尽可能填满硬件 TX FIFO。
- TX ring 清空后立即关闭 TX 中断，防止空中断风暴。
- 数据出队后唤醒等待 TX ring 空位的进程。

QEMU 需按位更新 16550A IER 的 TX/RX enable；K210 需更新 `uarths->ie.txwm`。两个平台都不应始终开启 TX 中断。

### 4.5 波特率切换

`uart_set_baud()` 是硬件能力，继续保留在 UART 层，但由 console ioctl 对用户态暴露。切换前必须：

1. 等待软件 TX ring 清空。
2. 等待硬件 TX FIFO/移位寄存器完成。
3. 暂停 RX 中断并处理边界处的残留字节。
4. 更新分频寄存器后恢复 RX 中断。

---

## 5. `console.c` 设计

### 5.1 简化 TTY 模式

第一阶段不实现完整 Linux termios，仅实现两种模式：

```c
enum console_mode {
  CONSOLE_MODE_TTY = 0,
  CONSOLE_MODE_RAW = 1,
};
```

TTY 模式保持当前项目约定：

- console 为非规范模式，收到字符后即可返回。
- 退格、Tab 补全和行编辑继续由 shell 处理。
- 回显可打印字符和 `\n`，并保持 ANSI 转义序列不回显的现有行为。
- K210 的 `\r` 过滤或 `\r`/`\n` 策略应从硬件中断移到 console TTY 处理。
- `Ctrl-P` 等 console 控制字符在 TTY 读取路径处理。

RAW 模式保证：

- `0x00` 到 `0xff` 全部原样返回。
- 不回显，不处理控制字符。
- 不做 `\r`/`\n` 或输出换行转换。
- 读写均使用 UART 层的批量接口。

### 5.2 单 RX ring 读取

`console.c` 不再保留 `cons.buf[]`。`consoleread()` 根据 console 模式处理从 `uart_read()` 取得的字节：

```c
int
consoleread(int user_dst, uint64 dst, int n)
{
  if (console_mode_get() == CONSOLE_MODE_RAW)
    return console_raw_read(user_dst, dst, n);

  return console_tty_read(user_dst, dst, n);
}
```

TTY 处理改为进程上下文后，控制字符不再于 UART IRQ 到达的瞬间执行，而在 console 消费该字符时执行。对当前 shell 持续读取的使用方式，延迟通常可忽略。如果未来要求无读者时 `Ctrl-P` 也立即响应，再引入 console 内核 worker，不在本次重构中提前增加该复杂度。

### 5.3 写路径

`consolewrite()` 应分块 `copyin` 后调用 `uart_write()`，不得在持有 `cons.lock` 时调用可睡眠的 UART 写接口。`cons.lock` 只保护 console 模式和 TTY 处理状态，不保护 UART TX。

`consputc()` 作为内核 console 字符输出接口，普通进程上下文可进入 TX ring；panic 路径必须继续走 `uartputc_sync()`。实施时应明确区分“普通 console 输出”和“紧急同步输出”，避免 printf/中断上下文误入睡眠路径。

---

## 6. Console ioctl 接口

建议将当前 UART ioctl 迁移到 `kernel/include/console.h`：

```c
#define CONSOLE_IOCTL_FLUSH_INPUT   0x01
#define CONSOLE_IOCTL_SET_MODE      0x02
#define CONSOLE_IOCTL_GET_MODE      0x03
#define CONSOLE_IOCTL_SET_BAUD      0x04
#define CONSOLE_IOCTL_GET_BAUD_INFO 0x05
#define CONSOLE_IOCTL_GET_RX_STATS  0x06

#define CONSOLE_MODE_TTY 0
#define CONSOLE_MODE_RAW 1
```

现有 `uart_baud_info` 可改名为 `console_baud_info`，`uart_raw_stats` 改为不带模式语义的 `console_rx_stats`：

```c
struct console_baud_info {
  uint32 requested;
  uint32 actual;
  uint32 div;
  uint32 clock;
};

struct console_rx_stats {
  uint32 dropped;
  uint32 buffered;
  uint32 capacity;
  uint32 mode;
};
```

`SET_MODE` 建议而不是继续暴露 `RAW_START/RAW_END`，因为“模式”属于 console/TTY，不是 UART 硬件的启停状态。用户态可封装成：

```c
ioctl(fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_RAW);
/* binary transfer */
ioctl(fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_TTY);
```

### 6.1 模式的作用域

当前 `devsw` 与 `struct file` 没有类似 Linux `struct tty_struct` 的每打开实例 termios 状态，因此第一阶段的 console mode 是全局状态。

必须明确以下限制：

- 某进程设置 RAW 后，shell 和其他 console 读者都处于 RAW 模式。
- 同一时间只允许一个二进制传输会话。
- `burn.c` 所有正常和失败出口都必须恢复 TTY 模式及 console 默认波特率。
- 如需从内核强制处理程序异常退出，后续应扩展设备 `open/close` 回调或为 `struct file` 增加设备私有状态。该扩展不与本次基础分层捆绑。

### 6.2 模式切换边界

为防止 shell 残留输入污染二进制协议，建议 `TTY -> RAW` 时默认清空 RX；`RAW -> TTY` 时也丢弃协议残留数据。切换操作必须与 RX IRQ 串行化：

```text
1. 关闭 RX 中断
2. 获取 RX lock
3. 清空软件 RX ring 和硬件 RX FIFO
4. 更新 console mode
5. 唤醒相关阻塞读者
6. 释放 RX lock
7. 恢复 RX 中断
```

不应在持有 RX lock 时调用内部会重新获取该锁的 `uart_flush_rx()`。实现时可提供 `uart_set_mode_boundary()` 或“锁内 helper + 对外封装”，保证原子性与锁层次清晰。

---

## 7. 需要删除和迁移的内容

### 7.1 内核

删除：

- `kernel/devsw/uartdev.c`
- `kernel/include/uartdev.h`
- `kernel/include/dev.h` 中的 `DEV_UART`
- `kernel/main.c` 中的 `uartdev_init()`
- `uarths.c` 中的 `uart_raw_lock`、`uart_raw_buf[]`、`uart_raw_mode` 及全部 `uart_raw_*()`
- `uarths.c` 对 `consoleintr()` 的调用
- `uarths.h` 中的 raw 模式接口
- `console.c` 中的 `cons.buf[]` 及 `r/w/e` 数据队列索引

迁移：

- 模式、波特率、flush 和 RX 统计 ioctl 移入 `console.c`/`console.h`。
- 硬件波特率和统计实现保留在 `uarths.c`，console ioctl 只做参数验证与 `copyout`。
- 字符转换、回显和控制字符处理集中到 `console.c`。

### 7.2 用户态

`user/app/burn.c` 由：

```c
uart_fd = dev(O_RDWR, DEV_UART, 0);
ioctl(uart_fd, UART_IOCTL_RAW_START, 0);
```

改为使用 console fd：

```c
uart_fd = dev(O_RDWR, DEV_CONSOLE, 0);
ioctl(uart_fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_RAW);
```

同步更新：

- `user/test/uarttest.c`：改为 console mode/baud/RX stats 测试，建议改名为 `consoletest.c`。
- `user/test/devtest.c`：删除 `DEV_UART` 打开测试，将相关 ioctl 验证迁到 `DEV_CONSOLE`。
- 用户态公共头文件对 `console.h` 的暴露方式。
- `Makefile` 中 `uartdev.c` 对象和重命名测试的目标列表。

---

## 8. 锁、睡眠与中断规则

### 8.1 锁职责

| 锁 | 保护内容 | 可否在持有时睡眠 |
|---|---|---|
| `uart.rx_lock` | RX ring 索引、数据和 dropped 统计 | 只能通过 `sleep(chan, &rx_lock)` 原子释放 |
| `uart.tx_lock` | TX ring 索引和 TX 中断启停 | 只能通过 `sleep(chan, &tx_lock)` 原子释放 |
| `cons.lock` | console mode、回显及转义序列状态 | 不得持有它调用可睡眠的 `uart_read/write` |

### 8.2 锁顺序

尽量不在同一路径同时持有 console 锁和 UART 锁。必须组合时，先快照 console mode，释放 `cons.lock`，再进入 UART 读写。这可避免下列环路：

```text
console writer 持有 cons.lock 等待 TX 空位
    ↑                                      ↓
RX IRQ 进入 console 等待 cons.lock    TX IRQ 无法继续执行
```

### 8.3 中断约束

- UART IRQ 不得睡眠。
- UART IRQ 不得执行 `copyin/copyout`。
- 唤醒应在一次 FIFO 批量搬运后进行，避免每字节重复唤醒。
- 中断中只使用 spinlock 保护的短临界区。
- QEMU/K210 的 RX/TX interrupt enable 必须由平台 helper 封装，避免通用 ring 逻辑到处出现 `#ifdef QEMU`。

---

## 9. 建议的实施步骤

### 步骤 1：整理 UART 内部边界

- 封装 `uart_hw_getc()`、`uart_hw_putc()`、`uart_hw_tx_ready()`。
- 封装 RX/TX 中断启停。
- 保持现有对外行为不变并完成 QEMU/K210 双平台构建。

### 步骤 2：完成 TX ring

- 扩大 TX ring 并实现批量 `uart_write()`。
- 实现按需 TX 中断、睡眠唤醒和 `uart_flush_tx()`。
- `consolewrite()` 改用批量发送，panic 仍用同步路径。

### 步骤 3：建立统一 RX ring

- 将 `uart_raw_buf[]` 改为无模式语义的 `uart_rx_buf[]`。
- `uartintr()` 总是将硬件 RX FIFO 写入统一 ring。
- 实现批量阻塞/非阻塞读、flush 和统计接口。

### 步骤 4：console 消费统一 RX ring

- 在 `console.c` 增加 TTY/RAW 模式及 ioctl。
- 将原 `consoleintr()` 的必要 TTY 处理迁入 `console_tty_read()`。
- 删除 `cons.buf[]`，验证 shell 的非规范读取、回显、退格和 ANSI 序列。

### 步骤 5：迁移 raw 用户

- `burn.c` 改用 `DEV_CONSOLE` 及 console ioctl。
- 更新 `uarttest` 和 `devtest`。
- 确保正常、超时、协议错误和 killed 路径都恢复 TTY 模式与默认波特率。

### 步骤 6：删除旧设备层

- 删除 `uartdev.c`/`uartdev.h`。
- 删除 `DEV_UART` 和 `uartdev_init()`。
- 全局搜索并清理 `UART_IOCTL_*`、`uart_raw_*` 和 `consoleintr()` 引用。

每一步都应保持可构建，不建议一次性同时重写 RX、TX、console 和 `burn.c`。

---

## 10. 验证方案

### 10.1 构建

```sh
make build platform=qemu
make build platform=k210
make fs
```

### 10.2 QEMU 功能验证

- shell 启动和基本命令输入。
- 连续快速输入，检查无丢字符和死锁。
- 长文本输出，检查 TX ring 睡眠/唤醒。
- TTY/RAW/TTY 往返切换，检查输入残留是否清理。
- RAW 模式传输包含 `0x00`、`0x0a`、`0x0d`、`0x7f` 和 `0xff` 的数据。
- 模式切换时正在阻塞的 read/write 能正确唤醒或返回。

### 10.3 K210 功能验证

- 115200 波特率 shell 交互。
- K210 Enter 序列与 `\r`/`\n` 处理。
- 烧录协议全流程：TTY → RAW → 高波特率 → 默认波特率 → TTY。
- 烧录过程中检查 RX dropped 统计。
- 在不同 TX 负载下验证 `txwm` 中断不会停止发送或形成空中断。
- 人为中断烧录和注入 CRC/超时错误，确认恢复 console。

### 10.4 并发与压力验证

- 多进程同时写 console，数据不应造成 ring 索引破坏。
- RX 高速输入与 TX 长输出同时发生时不死锁。
- 长时间空闲时不应持续产生 TX 中断。
- RX ring 故意溢出时 dropped 计数正确，内核不越界。

---

## 11. 风险与取舍

### 11.1 全局 TTY/RAW 模式

这是当前 xv6 设备模型下的主要取舍。它能以较小改动完成清晰分层，但不具备 Linux 每 TTY 打开实例的独立 termios 语义。在单 console、单前台 shell 和单烧录程序的当前系统中可接受。

### 11.2 只保留一个 RX ring

优点是内存和状态机更简单，UART 与 console 边界清晰。代价是 TTY 层没有独立的“已处理输入”队列。当前为非规范模式且行编辑在 shell，因此不需要为模仿 Linux 而引入第二个 TTY buffer。若未来实现 canonical mode，应将“TTY 已处理队列”作为上层功能新增，而不回填到 UART 驱动。

### 11.3 console 实时控制字符

删除 `consoleintr()` 的 IRQ 直接调用后，特殊字符的处理时机变为 console read 消费时。本方案接受该变化，以换取底层 IRQ 与 TTY 语义的完全解耦。

### 11.4 panic 输出

panic 时不能依赖睡眠、TX 中断或其他 CPU 继续运行。因此不能删除同步轮询输出，也不能将所有 `consputc()` 无条件改成可睡眠的 TX ring 写入。

---

## 12. 完成标准

同时满足以下条件时，本次重构才算完成：

- `uarths.c` 不依赖 `console.h`，不包含 TTY/RAW 模式判断。
- 内核只有一个 UART RX 软件 ring。
- `console.c` 通过 ioctl 提供 TTY/RAW 模式。
- `/dev/uart`、`uartdev.c`、`DEV_UART` 和 `UART_IOCTL_*` 被完整移除。
- `consolewrite()` 与二进制传输使用统一 TX ring，panic 输出保留同步通道。
- QEMU 和 K210 均能构建。
- shell 交互、RAW 二进制数据、波特率切换和烧录流程全部通过。
- 无 TX 空中断风暴，无 RX/TX/console 锁顺序死锁，RX 溢出可通过统计观测。

