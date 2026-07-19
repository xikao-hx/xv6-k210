# SIGNAL 系统调用与终端信号设计方案

日期：2026-07-12  
范围：进程信号、用户态信号处理、Console 控制字符、Shell 前台进程组  
目标平台：QEMU 与 K210

---

## 1. 设计目标

为当前 xv6_k210 增加一套小型、职责清晰的信号机制，满足以下需求：

1. 用户程序可以注册、忽略或恢复信号的默认处理方式。
2. 内核和用户程序可以向指定进程或进程组发送信号。
3. TTY 模式下按下 `Ctrl+C`，能够中断当前前台任务，而不会退出 Shell 或影响后台任务。
4. 信号能够唤醒阻塞在 `sleep()`、pipe、Console 读取等可中断睡眠中的进程。
5. QEMU 和 K210 使用同一套上层语义；RAW 模式不解释任何控制字符。
6. 保持实现规模适合 xv6，不追求完整复制 POSIX/Linux 信号子系统。

原有 `kill(pid)` 系统调用及用户态 `_kill` 命令已经移除。新设计不恢复旧接口，而是使用语义明确的 `sigsend()` 发送信号，避免 `kill` 同时表示“发送任意信号”和“杀死进程”的歧义。

---

## 2. 非目标

第一阶段不实现以下能力：

- 实时信号及同一信号的多次排队。
- `sigqueue()` 携带用户数据。
- `SA_SIGINFO`、alternate signal stack。
- 多线程级信号投递。
- `Ctrl+Z`、停止/继续、完整 Shell job control。
- POSIX 的全部 `sigaction` flag 和复杂重启语义。
- 信号 handler 嵌套执行。

待基础信号稳定后，可以增加 `SIGTSTP`、`SIGSTOP`、`SIGCONT` 和 Shell `jobs/fg/bg`。

---

## 3. 当前代码基础与问题

### 3.1 当前终止机制

`struct proc` 中已有：

```c
int killed;
```

`usertrap()`、`sys_sleep()`、pipe 和 UART 等路径会检查该字段。它只能表示“进程应退出”，无法表达信号编号、忽略、用户 handler、屏蔽或进程组投递。

新信号机制落地后，`killed` 暂时保留为内核最终终止标志：默认动作为终止或收到 `SIGKILL` 时，由信号层设置 `killed = 1`。这样可以复用现有安全退出检查，避免一次性改写所有阻塞路径。

### 3.2 Console 控制字符处理过晚

当前 TTY 控制字符主要在 `console_tty_char()` 中处理，而该函数由 `consoleread()` 调用。CPU 密集型前台程序如果不执行 `read()`，UART RX ring 中的 `Ctrl+C` 不会及时被解释。

因此，`Ctrl+C` 必须在字节进入 RX ring 时被 Console 观察，但 UART 驱动仍不能直接依赖 SIGNAL 或进程模块。

### 3.3 Shell 没有进程组

Shell 可以创建普通命令、管道和后台命令，但进程只有 PID，没有 PGID。只记录一个前台 PID 无法正确处理中间包含多个进程的管道：

```sh
producer | consumer
```

因此 Ctrl+C 的正确目标必须是“前台进程组”，而不是某一个 PID。

---

## 4. 总体架构

```text
UART RX 中断
    │
    │ 收到字节，调用通用非阻塞 RX observer
    ▼
Console TTY observer
    │
    ├── RAW 模式：不处理，字节原样进入 RX ring
    ├── 普通字节：进入 RX ring
    └── Ctrl+C：消费该字节，记录待处理 TTY 事件
                         │
                         ▼
              trap 安全上下文处理事件
                         │
                         ▼
              向 foreground_pgid 发送 SIGINT
                         │
                         ▼
              proc.sig_pending 位图
                         │
                         ▼
             返回用户态前 signal_deliver()
                ├── 默认动作
                ├── 忽略
                └── 用户 handler + sigreturn
```

依赖方向保持为：

```text
shell -> syscall -> proc/signal
console -> signal_process_group
console -> uart
uart -> 通用 observer 回调，不包含 TTY 或信号语义
```

---

## 5. 信号集合与默认行为

公共定义放在 `kernel/include/signal.h`，用户态通过对应公共头文件或 `user/include/user.h` 使用。

```c
#define NSIG       32

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGKILL     9
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGCHLD    17

#define SIG_DFL ((uint64)0)
#define SIG_IGN ((uint64)1)
```

第一阶段行为：

| 信号 | 来源/用途 | 默认动作 | 可捕获 | 可忽略 |
|---|---|---:|---:|---:|
| `SIGHUP` | 终端断开，预留 | 终止 | 是 | 是 |
| `SIGINT` | `Ctrl+C` | 终止 | 是 | 是 |
| `SIGQUIT` | `Ctrl+\`，可选 | 终止 | 是 | 是 |
| `SIGKILL` | 强制终止 | 终止 | 否 | 否 |
| `SIGPIPE` | 向无读端 pipe 写入 | 终止 | 是 | 是 |
| `SIGALRM` | 定时信号，预留 | 终止 | 是 | 是 |
| `SIGTERM` | 请求进程退出 | 终止 | 是 | 是 |
| `SIGCHLD` | 子进程状态变化 | 忽略 | 是 | 是 |

第一阶段重点完成 `SIGINT`、`SIGKILL`、`SIGTERM` 和 `SIGCHLD`。预留编号不代表必须同时实现全部信号来源。

信号编号必须满足：

```c
0 < signum && signum < NSIG
```

信号 0 不作为实际信号。后续若需要“仅检查目标是否存在”的能力，可以单独定义 `sigsend(pid, 0)` 语义，不在第一阶段实现。

---

## 6. 用户态接口

### 6.1 系统调用

新增三个核心系统调用：

```c
typedef void (*sighandler_t)(int);

sighandler_t signal(int signum, sighandler_t handler);
int sigsend(int target, int signum);
int sigreturn(void);
```

接口约定：

- `signal()` 注册 handler，并返回旧 handler；失败返回 `(sighandler_t)-1`。
- `sigsend(target, signum)` 发送信号。
- `target > 0` 表示 PID。
- `target < 0` 表示进程组 `-target`。
- 暂不接受 `target == 0`，避免隐式依赖调用者进程组语义。
- `sigreturn()` 仅由用户态 signal trampoline 调用，不作为普通应用接口使用。
- `SIGKILL` 不允许注册 handler，也不允许设置为 `SIG_IGN`。

系统调用号从当前末尾继续分配，不复用已删除的 `SYS_kill = 6`：

```c
#define SYS_signal      32
#define SYS_sigsend     33
#define SYS_sigreturn   34
#define SYS_setpgid     35
```

保留 6 号空洞可以避免后续系统调用编号变化，也使旧二进制调用 6 号时明确失败。

### 6.2 进程组接口

增加：

```c
int setpgid(int pid, int pgid);
int getpgrp(void);
```

为了控制 Console 前台进程组，优先复用现有 `ioctl()` 分层，而不是增加 TTY 专用系统调用：

```c
#define CONSOLE_IOCTL_SET_FG_PGRP  ...
#define CONSOLE_IOCTL_GET_FG_PGRP  ...
```

Shell 使用标准输入 fd 0 调用：

```c
ioctl(0, CONSOLE_IOCTL_SET_FG_PGRP, pgid);
```

内核必须确认该 fd 对应 Console 设备，并限制普通后台任务任意抢占终端。第一阶段可采用简化权限规则：调用者必须是当前前台进程组成员或 Console 尚未设置前台组；Shell 初始化时负责建立第一个前台组。

---

## 7. 进程数据结构

在 `kernel/include/proc.h` 中扩展：

```c
struct proc {
  ...
  int pgid;
  uint32 sig_pending;
  uint32 sig_mask;
  uint64 sig_handlers[NSIG];
  int sig_handling;
  int sig_current;
  struct trapframe sig_saved_trapframe;
};
```

字段职责：

- `pgid`：所属进程组 ID。
- `sig_pending`：待投递信号位图，同一信号最多保留一次。
- `sig_mask`：暂时屏蔽的信号位图；第一阶段只供内部防重入使用，也可以暂不暴露用户接口。
- `sig_handlers[]`：每个信号的用户处理地址、`SIG_DFL` 或 `SIG_IGN`。
- `sig_handling`：是否正在执行用户 handler；第一阶段禁止嵌套。
- `sig_current`：当前正在处理的信号编号，用于调试和恢复。
- `sig_saved_trapframe`：handler 执行前的用户寄存器快照。

内存开销约为每进程 32 个 64 位 handler 加一个 trapframe。若后续需要降低开销，可以把 handler 表缩减为 `NSIG_SUPPORTED`，但第一版优先保持索引直观。

### 7.1 初始化、fork 和 exec

`allocproc()`：

- 清空 pending、mask 和 handling 状态。
- 所有 handler 初始化为 `SIG_DFL`。
- 新建进程默认 `pgid = pid`，或在 fork 后继承父进程组；推荐 fork 子进程继承父进程 `pgid`。

`fork()`：

- 继承 `pgid`、mask 和 handler 表。
- 不继承 pending 信号。
- 不继承正在执行 handler 的临时状态。

`exec()`：

- `SIG_IGN` 保持忽略。
- 用户注册的 handler 恢复为 `SIG_DFL`，因为旧地址空间中的函数地址在新程序中无效。
- 清空 `sig_handling`、`sig_current` 和 pending。
- 保持 `pgid` 不变。

---

## 8. 内核信号核心

建议新增：

```text
kernel/proc/signal.c
kernel/include/signal.h
```

并将 `kernel/proc/signal.o` 加入 `Makefile` 的内核对象列表，避免把信号逻辑继续堆入 `proc.c` 或 `trap.c`。

核心接口：

```c
int signal_send_pid(int pid, int signum);
int signal_send_pgrp(int pgid, int signum);
int signal_pending_unmasked(struct proc *p);
void signal_deliver(struct proc *p);
void signal_reset_on_exec(struct proc *p);
```

### 8.1 发送信号

发送流程：

1. 校验信号编号。
2. 在进程表中查找 PID 或 PGID。
3. 持有目标 `p->lock` 设置 pending 位。
4. 如果目标处于可中断 `SLEEPING`，将其唤醒。
5. `SIGKILL` 可以同时置 `p->killed = 1`，保证沿用现有强制退出路径。

伪代码：

```c
static void
signal_mark_locked(struct proc *p, int signum)
{
  p->sig_pending |= 1U << signum;
  if (signum == SIGKILL)
    p->killed = 1;
  if (p->state == SLEEPING)
    p->state = RUNNABLE;
}
```

不能在持有一个进程锁时再获取另一个进程锁。进程组发送应逐个获取和释放，避免形成锁顺序依赖。

### 8.2 可中断睡眠

简单地唤醒任意 `SLEEPING` 进程后，原等待循环必须重新检查条件和 pending signal。需要审计：

- `sys_sleep()`
- `pipewrite()`、`piperead()`
- `uart_read()`、`uart_write()`
- `wait()`
- buffer cache、磁盘与设备等待

第一阶段只允许用户可见、可安全提前返回的睡眠被普通信号中断。磁盘 I/O、锁等待等不可中断睡眠不应直接强制改成 `RUNNABLE`。因此建议在 `struct proc` 中增加睡眠可中断属性，或封装：

```c
void sleep_interruptible(void *chan, struct spinlock *lk);
```

普通信号只唤醒 interruptible sleep；`SIGKILL` 可设置终止标志，但仍等待不可中断内核操作自然完成。

---

## 9. 用户 handler 投递与恢复

### 9.1 投递时机

在 `kernel/trap/trap.c` 中，所有准备返回用户态的路径都应执行：

```c
signal_deliver(p);
```

位置应在最终 `p->killed` 判断之前，使默认终止动作可以设置 `killed`；用户 handler 则修改 trapframe 后通过 `usertrapret()` 返回用户态。

定时器中断确保 CPU 密集型进程也会周期性进入内核，因此无需等待该进程主动发起系统调用。

### 9.2 选择信号

```c
pending = p->sig_pending & ~p->sig_mask;
```

优先级规则：

1. `SIGKILL` 永远优先且不可屏蔽。
2. 其余信号按编号从小到大选择。
3. 第一阶段正在执行 handler 时不再投递用户 handler；`SIGKILL` 仍可立即生效。

### 9.3 默认与忽略动作

- handler 为 `SIG_IGN`：清除 pending，继续运行。
- handler 为 `SIG_DFL`：按照默认动作表处理。
- 默认终止：设置 `p->killed = 1`，由现有 trap 退出路径调用 `exit(-signum)`。
- `SIGCHLD` 默认忽略。

建议用负信号编号作为退出状态，便于 Shell 区分普通退出和信号终止。

### 9.4 用户 signal trampoline

用户 handler 返回时不能直接回到被中断程序，必须调用 `sigreturn()` 恢复完整寄存器。

新增用户态 trampoline 汇编，例如 `user/sigtramp.S`：

```asm
.global sigtramp
sigtramp:
  li a7, SYS_sigreturn
  ecall
```

投递 handler 时：

```c
p->sig_saved_trapframe = *p->trapframe;
p->sig_handling = 1;
p->sig_current = signum;
p->trapframe->a0 = signum;
p->trapframe->ra = user_sigtramp_address;
p->trapframe->epc = handler;
```

必须校验 handler 地址位于用户地址空间且可执行。若项目页表没有独立执行权限检查，至少验证：

```c
handler < p->sz
```

并通过页表查询确认映射有效。

`sigtramp` 可以作为固定用户映射页，也可以链接进每个用户程序。推荐固定映射页，方式类似 `TRAMPOLINE`，避免依赖每个 ELF 都包含同名符号。

### 9.5 sigreturn

`sys_sigreturn()`：

1. 校验 `sig_handling == 1`。
2. 恢复 `sig_saved_trapframe`。
3. 清除 handling/current 状态。
4. 返回恢复后的原始 `a0`。

最后一点不可遗漏，因为当前系统调用分发器会执行：

```c
p->trapframe->a0 = syscalls[num]();
```

如果 `sys_sigreturn()` 返回普通的 0，会覆盖被中断现场原有的 `a0`。

---

## 10. Ctrl+C 与 Console

### 10.1 模式语义

TTY 模式：

- `Ctrl+C`（`0x03`）转换为 `SIGINT`。
- 控制字节不进入用户可读 RX ring。
- 输出 `^C\n`，清理当前 Shell 编辑行所需状态。

RAW 模式：

- `0x03` 必须原样进入 RX ring。
- 不产生信号，不回显，不清理输入。
- 保证烧录、串口下载和二进制协议不受影响。

### 10.2 RX observer

为保持 UART 驱动纯净，UART 层只提供通用 observer：

```c
typedef int (*uart_rx_observer_t)(int c);
void uart_set_rx_observer(uart_rx_observer_t observer);
```

observer 返回 1 表示字节已被上层消费，不再写入 RX ring；返回 0 表示正常入 ring。

Console 注册的 observer 必须满足：

- 在中断上下文执行。
- 不睡眠。
- 不打印阻塞输出。
- 不扫描进程表。
- 不获取可能已被中断代码持有的进程锁。
- 只读取 TTY/RAW 模式并设置一个待处理事件位。

示意：

```c
static volatile uint tty_events;

static int
console_rx_observer(int c)
{
  if (console_mode_is_tty_irq_safe() && c == C('C')) {
    __sync_fetch_and_or(&tty_events, TTY_EVENT_SIGINT);
    return 1;
  }
  return 0;
}
```

如果工具链不使用原子 builtin，可以在关闭本 hart 中断的极短临界区内更新事件位，或使用专用自旋锁，但必须审查同核中断重入。

### 10.3 延迟处理 TTY 事件

在安全的 trap 上下文调用：

```c
console_dispatch_events();
```

它原子取走 `tty_events`，读取 `foreground_pgid`，调用：

```c
signal_send_pgrp(foreground_pgid, SIGINT);
```

建议在 `devintr()` 返回后的 trap 路径和时钟 trap 路径处理事件。UART 中断本身已经触发 trap，通常会立即完成分发；时钟路径作为兜底。

`^C\n` 的输出也放在延迟处理阶段，避免在 UART RX 中断 observer 内执行同步串口输出。

---

## 11. 前台进程组与 Shell

### 11.1 进程组规则

- Shell 启动时创建自己的进程组，通常 `pgid == shell_pid`。
- 普通前台任务的第一个子进程 PID 作为新 PGID。
- 管道所有子进程加入同一个 PGID。
- 后台任务创建独立 PGID，但不设置为 Console 前台组。
- `fork()` 默认继承父 PGID，Shell 随后通过 `setpgid()` 调整任务组。

### 11.2 前台命令执行

Shell 执行前台任务：

```text
fork 第一个子进程
  ↓
setpgid(child, child)
  ↓
管道其余子进程 setpgid(child_n, first_child)
  ↓
ioctl(console, SET_FG_PGRP, first_child)
  ↓
wait 前台进程组
  ↓
ioctl(console, SET_FG_PGRP, shell_pgid)
```

设置 PGID 应同时在父子两边尝试，以避免 child 很快 `exec()` 引发竞态；内核 `setpgid()` 需允许父进程调整尚未脱离控制关系的子进程。

### 11.3 Shell 信号策略

Shell 本身应：

```c
signal(SIGINT, SIG_IGN);
```

子进程执行用户命令前恢复：

```c
signal(SIGINT, SIG_DFL);
```

或者依赖 `exec()` 将捕获 handler 恢复默认，但 `SIG_IGN` 按设计跨 exec 保留，因此 Shell 子进程必须显式恢复 `SIGINT`，否则所有外部命令都会继承忽略行为。

Shell 正在读取命令行且自己是前台组时，Ctrl+C 不退出 Shell，只清空当前编辑行并显示新提示符。可由 Shell 自定义 handler 设置一个用户态标志，也可让 Console 的 `^C\n` 配合 `read()` 返回中断错误；推荐 handler 方案，职责更清楚。

### 11.4 后台命令

后台命令不属于 Console 前台 PGID，因此 Ctrl+C 不会影响它。第一阶段不实现 `SIGTTIN/SIGTTOU`，后台任务仍可能读写 Console；后续完整 job control 再限制后台终端访问。

---

## 12. 系统调用接入点

需要同步修改：

| 文件 | 修改内容 |
|---|---|
| `kernel/include/syscall.h` | 添加系统调用号 |
| `kernel/syscall/syscall.c` | extern、名称、参数个数、分发表 |
| `kernel/syscall/sysproc.c` | `sys_signal`、`sys_sigsend`、`sys_sigreturn`、进程组调用 |
| `user/usys.pl` | 生成用户态 ecall 桩 |
| `user/include/user.h` | 用户 API 声明 |
| `kernel/include/proc.h` | 信号状态和 PGID |
| `kernel/proc/proc.c` | 初始化、fork、exit/wait 相关行为 |
| `kernel/proc/exec.c` | exec 时重置信号状态 |
| `kernel/proc/signal.c` | 信号核心逻辑 |
| `kernel/trap/trap.c` | 返回用户态前投递、处理 TTY 延迟事件 |
| `kernel/devsw/console.c` | RX observer、前台 PGID、TTY ioctl |
| `kernel/driver/uarths.c` | 通用 RX observer 接口 |
| `user/sh/sh.c` | 进程组、前台终端、Shell 信号策略 |
| `Makefile` | 新对象、signal 测试程序 |

系统调用 trace 表必须同时填写名称和参数个数，避免数组索引不完整导致 trace 输出异常。

---

## 13. 锁与并发约束

信号实现涉及 UART 中断、Console、进程表和 trap，必须明确锁规则：

1. UART RX observer 不得获取 `p->lock`。
2. `console_dispatch_events()` 不持有 UART 锁时才允许扫描进程表。
3. `signal_send_pgrp()` 每次只持有一个 `p->lock`。
4. 修改 `sig_pending`、进程 state 和 `killed` 时持有目标 `p->lock`。
5. 当前进程在 trap 中处理自己的信号时，应明确哪些字段受 `p->lock` 保护；不要依赖“当前进程不会并发”这一假设，因为其他 hart 可以发送信号。
6. `foreground_pgid` 和 `tty_events` 使用 Console 专用锁或原子访问。
7. 不能在持有 Console 锁时调用可能获取进程锁并睡眠的代码。

推荐顺序：先从 Console 状态中复制 `foreground_pgid` 并释放 Console 锁，再调用 `signal_send_pgrp()`。

---

## 14. 错误处理与安全校验

- 无效 signum 返回 `-1`。
- 不存在的 PID/PGID 返回 `-1`。
- 进程组发送只要至少命中一个进程即可返回 0。
- 尝试捕获或忽略 `SIGKILL` 返回 `-1`。
- handler 地址必须属于有效用户映射，不能跳入内核地址。
- 非 handler 上下文调用 `sigreturn()` 返回 `-1`，不得恢复未初始化 trapframe。
- 防止用户通过伪造 `sigreturn` 修改内核字段；只恢复用户寄存器，不接受用户提供的内核 trapframe 指针。
- `sstatus` 恢复必须过滤特权位，保证返回 U-mode 且用户态中断状态合法。
- 进程退出时清理 pending/handler 临时状态，防止 proc slot 复用泄漏。

---

## 15. 分阶段实施计划

### 阶段一：内核信号基础

1. 新增 `signal.h` 和 `signal.c`。
2. 扩展 `struct proc` 并完成 alloc/fork/exec 生命周期。
3. 实现 `signal()`、`sigsend()` 和 `sigreturn()`。
4. 实现默认终止、忽略和用户 handler。
5. 先通过 PID 手工发送信号验证，不接入 Ctrl+C。

验收：用户程序可以向指定 PID 发送 `SIGTERM`，默认退出、忽略和 handler 返回均正确。

### 阶段二：可中断睡眠

1. 增加 `sleep_interruptible()` 或等价状态标记。
2. 改造 `sys_sleep()`、pipe 和 UART 用户等待路径。
3. 保持磁盘与锁等待不可中断。

验收：阻塞在 sleep、pipe、Console read 中的进程收到信号后能够安全处理或退出。

### 阶段三：进程组与 Console

1. 增加 PGID 和 `setpgid/getpgrp`。
2. Console 增加前台 PGID ioctl。
3. UART 增加通用 RX observer。
4. Console 识别 Ctrl+C 并延迟发送 SIGINT。

验收：CPU 死循环和阻塞程序都能被 Ctrl+C 中断；RAW 模式中的 `0x03` 保持原样。

### 阶段四：Shell 集成

1. 普通任务、管道和后台任务建立独立进程组。
2. 前台任务执行期间切换 Console foreground PGID。
3. Shell 处理 SIGINT，并在子进程中恢复默认行为。
4. wait 完成或错误退出时必须恢复 Shell 前台组。

验收：Ctrl+C 中断整个管道、不退出 Shell、不影响后台任务。

### 阶段五：扩展信号来源

1. pipe 无读端时产生 `SIGPIPE`。
2. 子进程退出时产生 `SIGCHLD`。
3. 视需要增加 alarm/timer。
4. 最后考虑 `SIGTSTP/SIGCONT` 和 job control。

---

## 16. 测试方案

新增 `user/test/signaltest.c`，覆盖：

### 16.1 系统调用与 handler

- 注册 handler 后能收到正确 signum。
- handler 返回后继续执行被中断代码。
- `SIG_IGN` 生效。
- `SIG_DFL` 恢复默认动作。
- `SIGKILL` 无法捕获或忽略。
- 非法编号、PID 和 handler 地址返回错误。
- handler 前后通用寄存器和用户栈保持正确。
- `fork()` 继承 handler，`exec()` 正确重置。

### 16.2 阻塞与并发

- 信号中断 `sleep()`。
- 信号中断 pipe read/write。
- 信号中断 Console read。
- 多 hart 同时向同一进程发送信号不会丢失位图状态或死锁。
- 进程退出与信号发送并发时不会操作已复用 proc slot。

### 16.3 Ctrl+C

- 前台 CPU 死循环收到 SIGINT 并退出。
- 前台阻塞程序收到 SIGINT 并退出。
- `a | b` 两端都收到 SIGINT。
- `cmd &` 不收到前台 SIGINT。
- Shell 保持运行并重新显示提示符。
- Shell 输入到一半按 Ctrl+C 能清空编辑行。
- RAW 模式下发送 `0x03`，用户程序读取到原字节且不产生 SIGINT。

### 16.4 平台验证

```sh
make build platform=qemu
make fs
make run platform=qemu
make build platform=k210
```

K210 还需验证：

- `\n`/`\r` 过滤不影响 Ctrl+C。
- UART 高速传输和烧录进入 RAW 模式后不误触发信号。
- RX observer 不显著增加 UART 中断延迟或丢字节率。

---

## 17. 兼容性与迁移

旧 `kill(pid)` 系统调用不恢复。历史 testcase 中的调用按意图迁移：

```c
kill(pid);
```

替换为：

```c
sigsend(pid, SIGKILL);
```

用户态原 `_kill` 命令可在信号功能完成后以新语义重新提供，建议命名为 `signal` 或 `sigsend`：

```sh
sigsend PID SIGNAL
```

如果希望兼容常见 Unix 使用习惯，也可以保留命令名 `kill`，但它只是用户程序名称，内部调用 `sigsend()`，不代表恢复 `kill` 系统调用。

---

## 18. 关键设计结论

1. 原 `kill` 系统调用保持删除，统一由 `sigsend()` 承担信号发送职责。
2. `killed` 暂时保留为信号默认终止动作的最终落点，不再作为外部发送接口。
3. Ctrl+C 的目标是 Console 前台进程组，而不是当前进程或单一 PID。
4. UART 驱动只提供通用 RX observer，不包含 Ctrl+C、TTY 或 SIGNAL 语义。
5. UART 中断只记录 TTY 事件，进程组扫描和信号发送延迟到安全 trap 上下文。
6. RAW 模式绝不解释 `0x03`，保证串口二进制传输安全。
7. 第一阶段禁止 handler 嵌套，以单份 trapframe 快照降低实现复杂度；后续需要嵌套时再改为用户栈 signal frame。
8. 先完成 PID 信号与 handler，再改可中断睡眠，最后接入进程组、Console 和 Shell，便于逐步验证和定位问题。
