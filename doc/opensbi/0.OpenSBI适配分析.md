# xv6-k210 改为 OpenSBI 引导启动适配分析

本文档基于 `doc/` 目录下的移植记录，总结将 xv6-k210 从 RustSBI 迁移到 OpenSBI 引导需要做的适配工作。

## 原有架构回顾

xv6-k210 项目（2020 年）使用 RustSBI 作为 M 态固件，内核运行在 S 态。K210 芯片使用 RISC-V 特权规范 v1.9.1，与当前主流 v1.11 存在多处差异。RustSBI 通过自定义扩展弥补了这些差异。

RustSBI 为 xv6-k210 提供的关键支持：
- 多核启动（IPI 唤醒）
- 虚拟内存管理（`sfence.vma` 指令模拟、`mstatus.VM` 设置）
- 时钟中断（SBI 标准接口）
- S 态外部中断（RustSBI 自定义扩展，K210 硬件不支持 S 态外部中断）
- 时钟初始化（PLL 配置等）

## 需要适配的内容

### 1. S 态外部中断——最大难点

**问题**：K210 使用 RISC-V 特权规范 v1.9.1，硬件上不存在 S 态外部中断。原 RustSBI 通过自定义 SBI 扩展实现了 S 态外部中断的软件代理。

**原 RustSBI 做法**：
- 内核通过自定义 SBI 调用（extension 0x0A000004, function 0x210）将 S 态中断处理函数指针注册给 RustSBI
- 当 M 态收到外部中断时，RustSBI 设置 `mstatus.mprv`，然后调用注册的 S 态处理函数
- 处理完成后通过另一个自定义 SBI 调用（extension 0x0A000005）重新使能 M 态外部中断

**当前代码位置**：[kernel/include/sbi.h:93-100](../kernel/include/sbi.h#L93-L100)、[kernel/trap.c:186-219](../kernel/trap.c#L186-L219)

**适配方案**：
- 方案 A：在 OpenSBI 中实现 K210 平台补丁，通过 `mideleg` 委托 + `mstatus.mprv` 机制代理外部中断到 S 态
- 方案 B：修改 OpenSBI 的 M 态陷阱处理，在 K210 平台上捕获外部中断后调用内核注册的回调
- 方案 C：如果 K210 硬件可以通过设置 `mideleg` 直接委托，则在 OpenSBI 启动时完成委托配置即可

### 2. `sfence.vma` 指令兼容

**问题**：K210 只有 v1.9.1 的 `sfence.vm` 指令，不存在 `sfence.vma`。同时分页使能需要写 `mstatus.VM` 位（v1.9.1 特有），S 态无法操作。

**原 RustSBI 做法**：
1. 内核执行 `sfence.vma` 触发非法指令异常
2. RustSBI 捕获后读 `satp` 获取根页表 PPN
3. 写 `sptbr`（v1.9.1 的页表基址寄存器）
4. 设置 `mstatus.VM` 开启分页
5. 执行 `sfence.vm` 清空 TLB
6. 返回 `mepc + 4`

**当前代码位置**：内核直接使用 `sfence.vma`（[vm.c:96](../kernel/vm.c#L96)、[exec.c:162](../kernel/exec.c#L162)、[proc.c:556-559](../kernel/proc.c#L556-L559)），定义在 [kernel/include/riscv.h:329-334](../kernel/include/riscv.h#L329-L334)

**适配方案**：
- 在 OpenSBI 的非法指令异常处理中，为 K210 平台添加对 `sfence.vma` 指令的模拟，逻辑与 RustSBI 一致
- 或内核条件编译，K210 上发 `sfence.vm` 并通过 SBI 调用请求 OpenSBI 设置 `mstatus.VM`

### 3. 时钟初始化

**问题**：K210 的 PLL 时钟树初始化原在 RustSBI 中完成。SD 卡驱动（SPI 通信）依赖正确的时钟配置。

**原文档说明**（[构建调试-SD卡驱动.md](../doc/构建调试-SD卡驱动.md)）：
> SDK 中调节时钟的代码直接跳过，因为这个在 RustSBI 里面已经调好了

**适配方案**：
- 确认 OpenSBI 的 K210 平台支持是否包含 PLL/PLL2 及外设时钟初始化
- 如未包含，需在 OpenSBI 的 `platform/k210/` 目录添加时钟初始化代码
- 涉及寄存器：SYSCTL 的 PLL 控制寄存器、CLK_SEL 等

### 4. SD 卡驱动相关外设

SD 卡驱动通过 SPI 协议与 SD 卡通信，涉及以下外设（已在内核页表中映射）：

| 外设 | 物理地址 | 用途 |
|------|----------|------|
| SPI0 | 0x52000000 | SPI 通信 |
| SPI1 | 0x53000000 | 备用 SPI |
| SPI2 | 0x54000000 | 备用 SPI |
| GPIOHS | 0x38001000 | 片选信号控制 |
| FPIOA | 0x502B0000 | 引脚功能复用 |
| DMAC | 0x50000000 | DMA 传输 |
| SYSCTL | 0x50440000 | 时钟控制 |

**适配要点**：
- 外设 MMIO 页表映射已在 [vm.c](../kernel/vm.c) 中完成，不依赖 SBI
- 需确保 SPI 时钟源在 OpenSBI 中已正确初始化（见第 3 点）
- 当前 SD 卡读写稳定性问题与 SBI 无直接关系，属于驱动本身的问题

### 5. 清理 RustSBI 专有 SBI 调用

以下自定义 SBI 扩展是 RustSBI 特有的，OpenSBI 不支持：

| 调用 | 用途 | 替代方案 |
|------|------|----------|
| `SBI_CALL_1(0x0A000004, func)` | 注册 S 态外部中断处理函数 | 由 OpenSBI 通过标准异常委托机制替代 |
| `SBI_CALL_0(0x0A000005)` | 重新使能 M 态外部中断 | 由 OpenSBI 平台代码内部处理 |

**当前代码位置**：[kernel/include/sbi.h:93-100](../kernel/include/sbi.h#L93-L100)

### 6. 链接地址与启动入口对齐

**QEMU**：
- OpenSBI fw_jump 加载地址：`0x80000000`
- 内核链接地址：`0x80200000`（见 [linker/qemu.ld](../linker/qemu.ld)）
- 入口符号：`_entry`（见 [kernel/entry_qemu.S](../kernel/entry_qemu.S)）

**K210**：
- 内核链接地址：`0x80020000`
- 需确保 OpenSBI 的 `FW_JUMP_ADDR` 配置与此一致
- 设备树文件位于 `dts/` 目录，OpenSBI 通过 `a1` 寄存器传递给内核

### 7. 标准 SBI 接口（无需改动）

以下功能使用标准 SBI Legacy 扩展，RustSBI 和 OpenSBI 均支持：

| 功能 | SBI 调用号 | 说明 |
|------|------------|------|
| 设置时钟中断 | SBI_SET_TIMER (0) | `sbi_set_timer()` |
| 控制台输出 | SBI_CONSOLE_PUTCHAR (1) | `sbi_console_putchar()` |
| 控制台输入 | SBI_CONSOLE_GETCHAR (2) | `sbi_console_getchar()` |
| 发送 IPI | SBI_SEND_IPI (4) | 多核唤醒 |
| 关机 | SBI_SHUTDOWN (8) | `sbi_shutdown()` |

## 适配优先级

| 优先级 | 任务 | 说明 |
|--------|------|------|
| 1 | 编译 OpenSBI for K210 | 配置 fw_jump，确认基础启动链路 |
| 2 | `sfence.vma` 指令模拟 | 阻塞分页开启，必须先解决 |
| 3 | S 态外部中断 | 最复杂的部分，阻塞设备交互 |
| 4 | 时钟初始化 | 阻塞 SD 卡和文件系统 |
| 5 | 清理 RustSBI 专有调用 | 代码清理 |
| 6 | 文件系统 FAT32 适配 | 独立于 SBI，但阻塞整体可用性 |

## 参考资料

- [构建调试-开机启动.md](构建调试-开机启动.md)
- [构建调试-时钟中断.md](构建调试-时钟中断.md)
- [构建调试-外部中断.md](构建调试-外部中断.md)
- [构建调试-SD卡驱动.md](构建调试-SD卡驱动.md)
- [构建调试-SD卡驱动v2.md](构建调试-SD卡驱动v2.md)
- [构建调试-进程管理.md](构建调试-进程管理.md)
- [rustsbi.md](rustsbi.md)
- [xv6-k210-report-车春池.md](xv6-k210-report-车春池.md)
- [report_2020_12_26.md](report_2020_12_26.md)
