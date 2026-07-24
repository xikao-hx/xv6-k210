# AGENTS.md

本文件为在本仓库中工作的代码代理提供项目约定和操作指南。

## 交流语言

- 与用户沟通时使用中文。

## 项目概览

这是一个将 xv6-riscv 移植到 K210/QEMU 的工程。仓库主要内容：

- `kernel/`：内核代码。
  - `driver/`：底层硬件驱动。
  - `devsw/`：设备抽象层，通过 `devsw[]` 提供 `read`/`write`/`ioctl`。
  - `fs/`：FAT32 文件系统、buffer cache、文件对象等。
  - `proc/`：进程、调度、exec。
  - `syscall/`：系统调用分发和实现。
  - `trap/`：异常、中断、trampoline。
  - `vm/`：页表、内存分配、用户/内核拷贝。
- `user/`：用户态程序、shell、用户态 libc。
- `tools/`：文件系统镜像生成、K210 烧录和串口下载工具。
- `testcase/`：xv6 相关测试用例。
- `linker/`：QEMU 和 K210 对应链接脚本。
- `bootloader/`：RustSBI 相关内容。

## 常用命令

- `make build platform=qemu`：构建 QEMU 版本。`platform` 默认为 `qemu`，可省略。
- `make build platform=k210`：构建 K210 版本。
- `make fs`：生成 FAT32 文件系统镜像 `target/fs.img`，并填充用户程序。
- `make run platform=qemu`：构建、生成文件系统并启动 QEMU。
- `make run platform=k210`：构建、生成文件系统并通过 `kflash.py` 烧录 K210。
- `make qemu`：`make run platform=qemu` 的别名。
- `make clean`：清理 `build/` 以及 `target/` 中的构建产物。
- `make boot`：打开 K210 串口终端。
- `make download`：通过 UART 下载 `target/fs.img` 到板端。

工具链默认使用 `riscv64-unknown-elf-`，在 `Makefile` 的 `TOOLPREFIX` 中配置。

## 平台差异

代码中通过 `QEMU` 宏区分平台：

- QEMU：
  - 使用 `kernel/entry_qemu.S`。
  - 使用 `kernel/driver/virtio_disk.c`。
  - 链接脚本为 `linker/qemu.ld`。
  - 编译时添加 `-D QEMU`。
- K210：
  - 使用 `kernel/entry_k210.S`。
  - 使用 `kernel/driver/` 下 SPI、I2C、DMAC、GPIOHS、FPIOA、SYSCTL、SD card 等驱动。
  - 链接脚本为 `linker/k210.ld`。
  - UART 输出走 SBI ecall，不是直接 MMIO。

修改平台相关代码时，注意 `#ifdef QEMU` / `#ifndef QEMU` 分支是否都需要同步处理。

## 关键注意事项

- 内核代码禁止使用 `float` 或 `double` 运算。K210 有硬件 FPU，但 xv6 不保存/恢复浮点上下文，内核浮点指令会触发 illegal instruction。
- 控制台是非规范模式：字符立即传递，退格、Tab 补全、行编辑由 shell 处理。
- Console 只回显可打印字符和 `\n`。
- K210 上 Enter 会发送 `\n` 后跟 `\r`，内核在 `consoleintr()` 中通过 `#ifndef QEMU` 过滤 `\r`。
- 修改内核时避免引入标准库依赖，保持 freestanding 环境假设。
- 仓库工作区可能已有用户改动；不要回退、删除或重写与当前任务无关的变更。

## 代码风格

- 缩进使用 2 个空格，不使用 Tab。
- 函数定义的左大括号单独一行。
- `if`、`for`、`while` 等控制流的左大括号放在同一行。
- 函数名和变量名使用 `lowercase_with_underscores`。
- 宏使用 `UPPERCASE`。
- 头文件放在 `kernel/include/` 或 `user/include/`，内核头文件按 `#include "header.h"` 形式包含。
- 注释应解释不直观的设计原因或硬件约束，避免重复代码本身。

## 构建与验证建议

- 普通内核/用户态改动后，优先运行：
  - `make build platform=qemu`
  - 必要时再运行 `make fs`
- 涉及 QEMU 启动、磁盘或文件系统行为时，运行：
  - `make run platform=qemu`
- 涉及 K210 外设、SD 卡、UART、I2C、SPI、DMAC 等代码时，至少确保：
  - `make build platform=k210`
  - 检查对应 `#ifndef QEMU` 路径没有影响 QEMU 构建。
- 涉及用户程序列表时，同步检查 `Makefile` 中的 `UPROGS`。
- 涉及系统调用时，通常需要同步检查：
  - `kernel/include/syscall.h`
  - `kernel/syscall/syscall.c`
  - `kernel/syscall/sysproc.c` 或 `kernel/syscall/sysfile.c`
  - `user/usys.pl`
  - `user/include/user.h`

## 文件系统与用户程序

- 文件系统镜像由 `make fs` 生成。
- `tools/mkfs.py` 会把 `build/user` 中的用户程序写入 FAT32 镜像。
- 新增用户程序时，应确认其目标被加入 `Makefile` 的 `UPROGS`，并且链接规则覆盖对应目录。

## 设备驱动约定

- 底层寄存器访问和硬件初始化放在 `kernel/driver/`。
- 面向文件接口的设备行为放在 `kernel/devsw/`。
- 新增设备相关系统调用或 ioctl 时，优先保持现有 `devsw[]` 分层方式。

## Git 与工作区

- 修改前先查看相关文件当前内容和 `git status --short`。
- 不要使用 `git reset --hard`、`git checkout -- <file>` 等破坏性命令，除非用户明确要求。
- 不要清理或覆盖 `target/`、`build/` 之外的用户变更。
- 如果构建生成产物导致工作区变化，只在最终回复中说明，不要擅自提交或回退。

### 代码提交约束

- `doc/*-bank/`、memory-bank、progress、code-design、implementation-plan 等代理工作过程文档默认只在本地维护，不纳入代码提交；只有用户明确要求提交这些文档时才可暂存。
- 功能提交只暂存本步骤直接涉及的源码、头文件、正式测试和必要构建配置，不夹带过程记录或其他无关文件。
- 禁止使用 `git add .`、`git add -A` 等扩大暂存范围的命令；必须显式列出本次提交的文件路径。
- 提交前必须执行 `git diff --cached --name-only` 和 `git diff --cached --check`，确认暂存清单和格式检查均符合本步骤范围。
- 工作区中已有的未跟踪文件和用户改动默认不提交、不删除、不清理。
- 分步开发时，每个步骤的代码与对应正式测试独立提交；验证记录保留在本地过程文档中，不混入功能提交。
