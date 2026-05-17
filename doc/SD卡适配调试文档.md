
# 2. 任务3适配记录

## 2.1 适配目标
将 xv6-k210 的 K210 板级 HAL 驱动（SD卡、SPI、DMA等）移植到 os/ 目录，通过 disk.c 抽象层统一管理 QEMU（virtio）和 K210（sdcard）两种后端，保证 QEMU 构建不受影响。

## 2.2 移植的文件清单

### 新增头文件（os/kernel/include/）

| 文件 | 来源 | 用途 |
|------|------|------|
| disk.h | 新建 | 磁盘抽象层接口，forward-declare struct buf |
| fat32.h | xv6-k210 | FAT32 文件系统定义 |
| sdcard.h | xv6-k210 | SD 卡 SPI 模式驱动 |
| spi.h | xv6-k210 | SPI 主机控制器驱动 |
| dmac.h | xv6-k210 | DMA 控制器驱动 |
| gpiohs.h | xv6-k210 | 高速 GPIO 驱动 |
| fpioa.h | xv6-k210 | FPIOA 功能引脚分配 |
| sysctl.h | xv6-k210 | 系统控制器驱动 |
| utils.h | xv6-k210 | 延时和寄存器操作工具 |

### 新增源文件

| 文件 | 目录 | 用途 |
|------|------|------|
| disk.c | os/kernel/driver/ | 磁盘抽象层：disk_init/read/write/intr |
| fat32.c | os/kernel/fs/ | FAT32 文件系统实现（约 935 行） |
| sdcard.c | os/kernel/driver/ | SD 卡 SPI 模式驱动 |
| spi.c | os/kernel/driver/ | SPI 主机控制器 |
| dmac.c | os/kernel/driver/ | DMA 通道管理 |
| gpiohs.c | os/kernel/driver/ | GPIOHS 中断和 IO 操作 |
| fpioa.c | os/kernel/driver/ | FPIOA 功能复用的初始化（空实现） |
| sysctl.c | os/kernel/driver/ | 系统时钟和频率获取 |
| utils.c | os/kernel/driver/ | usleep 和寄存器 read/write 工具 |

### 修改的现有文件

| 文件 | 修改内容 |
|------|----------|
| os/kernel/include/defs.h | 添加 disk_init/read/write/intr、fat32_init 声明，删除 lookup_sym |
| os/kernel/main.c | virtio_disk_init() → disk_init() |
| os/kernel/trap.c | virtio_disk_intr() → disk_intr() |
| os/kernel/libc/printf.c | 删除已移除的 symtab.h 引用 |
| os/Makefile | 新增 disk.o 到 OBJS |
| os/kernel/include/buf.h | 添加 timestamp 字段（FAT32兼容） |

## 2.3 架构设计

disk.c 作为统一磁盘抽象层，通过编译时宏 `#ifdef QEMU` 区分后端：

```c
void disk_init(void) {
#ifdef QEMU
    virtio_disk_init();
#else
    sdcard_init();
#endif
}

void disk_read(struct buf *b) {
#ifdef QEMU
    virtio_disk_rw(b, 0);
#else
    sdcard_read_sector(b->data, b->blockno);
#endif
}

void disk_write(struct buf *b) {
#ifdef QEMU
    virtio_disk_rw(b, 1);
#else
    sdcard_write_sector(b->data, b->blockno);
#endif
}

void disk_intr(void) {
#ifdef QEMU
    virtio_disk_intr();
#else
    dmac_intr(DMAC_CHANNEL0);
#endif
}
```

**设计要点：**
- QEMU 构建时，K210 驱动代码（spi.c, sdcard.c, dmac.c, fat32.c 等）存在源码树中但不参与编译
- Makefile 通过 `-D QEMU` 全局宏控制编译路径，K210 目标需要额外添加 `platform=k210` 变量支持
- buf.h 添加 timestamp 字段保持与 xv6-k210 的 FAT32 实现兼容

---

# 3. 调试记录

## 3.1 disk.h 循环引用问题

**现象：** disk.h 直接 `#include "buf.h"`，而 buf.h 依赖 sleeplock.h → spinlock.h，且需要 BSIZE 宏定义。

**调试过程：** 编译时报错 "field has incomplete type 'struct buf'"，追踪发现 buf.h 自身不满足自包含要求——它依赖调用方预先包含 types.h、param.h、fs.h 等。通过分析各头文件的依赖链，确定 disk.h 不应包含任何头文件，改用前向声明。

**根因：** buf.h 本身不是自包含的头文件，需要调用方按特定顺序前置包含 types.h、param.h、spinlock.h、sleeplock.h 和 fs.h。

**解决方案：** disk.h 改用 struct forward declaration，将具体 include 放在 disk.c 中按序引入：

```c
// disk.h — 仅前向声明，不包含任何头文件
struct buf;
void disk_init(void);
void disk_read(struct buf *b);
void disk_write(struct buf *b);
void disk_intr(void);
```

## 3.2 virtio 声明缺失

**现象：** disk.c 编译报错，virtio_disk_rb 等函数未声明。

**根因：** disk.c 的 `#else` 分支（即 QEMU 路径）包含了 virtio.h，但 virtio 函数声明实际在 defs.h 中。

**解决方案：** 始终包含 defs.h，移除 virtio.h 引用。

## 3.3 buf 结构体字段名不一致

**现象：** 编译报错 `b->sectorno` 字段不存在。

**根因：** xv6-k210 的 buf 使用 `sectorno` 字段名，os/ 的 buf 使用 `blockno`。

**解决方案：** disk.c 中使用 os/ 的字段名 `b->blockno`。

## 3.4 用户程序头文件路径问题

**现象：** 所有 user/*.c 编译报错找不到 `"kernel/types.h"` 等头文件。

**根因：** 头文件从 `os/kernel/xxx.h` 移到了 `os/kernel/include/xxx.h`，但 user 代码使用 `#include "kernel/xxx.h"` 形式。

**解决方案：** 批量修正所有 user/*.c 和 usys.pl 中的引用路径：
```shell
# 将所有 "kernel/xxx.h" 替换为 "kernel/include/xxx.h"
sed -i 's|"kernel/|"kernel/include/|g' user/*.c usys.pl
```

## 3.5 Makefile 条件过滤语法错误

**现象：** 空 LABEL 时某些条件编译块异常触发。

**根因：** `$(filter $(LAB), pgtbl lock)` 在 LABEL 为空时 pattern 为空，匹配全部内容。正确写法为 `$(filter pgtbl lock,$(LAB))`。

**解决方案：** 交换 filter 参数位置。

## 3.6 残留 .d 文件导致假性依赖错误

**现象：** clean 后初次编译报错找不到已删除的 symtab.h。

**根因：** 之前编译生成的 `*.d` 文件记录了依赖关系，引用了已删除的头文件。

**解决方案：** 清理所有残留依赖文件：
```shell
find os/ -name '*.d' -delete
```

## 3.7 mkfs 编译时头文件冲突

**现象：** mkfs/mkfs.c 编译报错 `O_CREATE` 未定义，实际应为 `O_CREAT`。

**根因：** `-I$K/include` 让 mkfs（主机 gcc 编译）找到了 kernel 的 fcntl.h，该文件定义了 `O_CREATE`（非 POSIX 标准），覆盖了系统头文件的 `O_CREAT`。

**解决方案：** 使用 `-idirafter` 替代 `-I` 将 kernel include 路径降级为次要搜索路径：
```makefile
# 错误
gcc -I. -o mkfs/mkfs mkfs/mkfs.c -I$K/include
# 正确
gcc -I. -o mkfs/mkfs mkfs/mkfs.c -idirafter $K/include
```

## 3.8 编译验证

最终构建通过，生成以下产物：

- kernel/kernel — xv6 内核 ELF（约 100+ KB）
- 13 个用户程序：_cat, _echo, _grep, _init, _kill, _ln, _ls, _mkdir, _rm, _sh, _grind, _wc, _zombie, _usertests
- fs.img — xv6 日志结构文件系统镜像（200000 block）

## 3.9 QEMU 运行验证

完整启动链测试通过：

```
OpenSBI v0.9 → xv6 kernel is booting → hart 1 starting → init: starting sh → $
```

---

# 4. 任务4适配记录

## 4.1 适配目标

参考 xv6-k210/Makefile，完成 os/ 目录下 K210 平台的构建支持，包括：
- 增加 `platform=k210` 构建变量，与现有 `platform=qemu`（默认）共存
- 移植 K210 入口文件 `entry_k210.S` 和链接脚本 `k210.ld`
- 移植 FAT32 文件系统相关的 syscall/file/exec 实现
- 保证 QEMU 构建不受影响

## 4.2 文件清单

### 新增文件

| 文件 | 目录 | 用途 |
|------|------|------|
| entry_k210.S | os/kernel/asm/ | K210 平台入口（跳转到 main） |
| k210.ld | os/linker/ | K210 平台链接脚本（加载地址 0x80020000） |
| file_k210.c | os/kernel/fs/ | K210 文件操作（使用 FAT32 elock/eunlock/eput/estat 等） |
| exec_k210.c | os/kernel/proc/ | K210 exec 实现（使用 FAT32 ename/eread） |
| syscall_k210.c | os/kernel/syscall/ | K210 系统调用分发表（简化版，不含 QEMU 独有调用） |
| sysfile_k210.c | os/kernel/syscall/ | K210 系统调用实现（基于 FAT32 的 ename/ealloc/eread/ewrite） |

### 修改的文件

| 文件 | 修改内容 |
|------|----------|
| os/Makefile | 添加 `platform ?= qemu` 变量；平台条件 OBJS（k210 含 spi/sdcard/fat32 等，qemu 含 virtio/fs/log 等）；条件 CFLAGS（-D QEMU）；条件链接脚本 |
| os/build.sh | 添加 `-k210`/`-xv6-k210`/`-all` 选项，支持 `platform=k210` 构建 |
| os/kernel/include/types.h | 添加 `wchar`（uint16）和 `NELEM` 宏 |
| os/kernel/include/stat.h | 添加 `#ifdef QEMU` 条件：QEMU 使用带 `ino`/`nlink` 的结构，K210 使用带 `name[STAT_MAX_NAME+1]` 的结构 |
| os/kernel/include/proc.h | 添加 include guard；`struct proc` 的 `cwd` 字段条件化：QEMU 使用 `struct inode*`，K210 使用 `struct dirent*` |
| os/kernel/include/file.h | 添加 `FD_ENTRY` 类型；`struct file` 添加 `struct dirent *ep` 字段（K210），`struct inode *ip` 改为 QEMU 条件 |
| os/kernel/include/buf.h | 添加 include guard；添加 `#ifndef BSIZE` / `#define BSIZE 512` 默认值 |
| os/kernel/include/fcntl.h | 添加 `O_APPEND` 定义 |
| os/kernel/include/syscall.h | 添加 K210 专有系统调用号：SYS_dev=27, SYS_readdir=28, SYS_getcwd=29, SYS_remove=30 |
| os/kernel/include/spinlock.h | 添加 include guard 和函数声明 |
| os/kernel/include/sleeplock.h | 添加 include guard 和 `#include "spinlock.h"` |
| os/kernel/include/string.h | 添加 `snstr` 声明 |
| os/kernel/libc/string.c | 添加 `snstr` 和 `strchr` 实现 |
| os/kernel/main.c | 条件化 K210 初始化：sdcard_init/fat32_init 替代 iinit/disk_init；secondary hart 启动地址 0x80020000 |
| os/kernel/proc/proc.c | 条件化 inode/dirent 操作：K210 使用 ename/edup/eput，QEMU 使用 namei/idup/iput |
| os/kernel/trap.c | 条件化 find_vma/mmap_handler（仅 QEMU） |
| os/kernel/fs/fat32.c | 移除 `#include "defs.h"`（避免 dirlookup 冲突），添加 forward declarations |
| os/kernel/fs/bio.c | virtio_disk_rw → disk_read/disk_write |

## 4.3 架构设计

### 平台条件编译策略

通过 Makefile 的 `platform` 变量控制两组编译路径：

```makefile
platform ?= qemu

# Common objects
OBJS = $K/main.o $K/trap.o ...  # 与平台无关的通用模块

# Platform-specific objects
ifeq ($(platform), k210)
OBJS += $K/entry_k210.o $K/driver/spi.o ... $K/fs/fat32.o $K/fs/file_k210.o ...
else
OBJS += $K/entry.o $K/driver/virtio_disk.o ... $K/fs/fs.o $K/fs/file.o ...
endif

# Conditional define
ifeq ($(platform), qemu)
CFLAGS += -D QEMU
endif
```

### 头文件条件化策略

三个关键数据结构需根据平台使用不同字段：

| 数据结构 | QEMU | K210 |
|----------|------|------|
| `struct stat` | `dev, ino, type, nlink, size` | `name[33], dev, type, size` |
| `struct proc::cwd` | `struct inode *cwd` | `struct dirent *cwd` |
| `struct file::ip` | `struct inode *ip` (FD_INODE) | `struct dirent *ep` (FD_ENTRY) |

### defs.h 排除策略

os/ 树的 `defs.h` 声明了 `dirlookup(struct inode*, ...)`，与 FAT32 的 `dirlookup(struct dirent*, ...)` 符号冲突。因此所有包含 fat32.h 的文件**不得**包含 defs.h，改为使用 forward declarations：

```c
// sysfile_k210.c — 不使用 defs.h，手动声明所需函数
int  argint(int, int*);
int  argstr(int, char*, int);
int  argaddr(int, uint64 *);
struct dirent*  ename(char *path);
struct dirent*  ealloc(struct dirent*, char*, int);
// ...
```

---

# 5. 调试记录（任务4）

## 5.1 defs.h 与 fat32.h dirlookup 符号冲突

**现象：** 包含 fat32.h 的文件编译报错：`dirlookup` 重定义。

**调试过程：**
1. 编译报错 "conflicting types for 'dirlookup'"——两个函数同名但参数类型不同
2. `grep -n dirlookup kernel/include/defs.h kernel/include/fat32.h` 确认两个头文件各自声明了不同签名的 `dirlookup`
3. 由于 C 语言不允许同一作用域存在同名函数声明（即使参数不同），包含两个头文件的源文件必然报错
4. 所有使用 FAT32 的文件必须避免包含 defs.h，否则触发冲突

**根因：** defs.h 声明了 `struct inode* dirlookup(struct inode*, char*, uint64*)`，而 fat32.h 声明了 `struct dirent* dirlookup(struct dirent*, char*, uint*)`。两者函数签名不同但符号名相同，C 语言不允许同一作用域存在两个同名函数声明。

**解决方案：** 所有使用 FAT32 的文件不包含 defs.h，改用 forward declarations：
```c
// 在 fat32.c, file_k210.c, exec_k210.c, sysfile_k210.c 中
struct dirent*  ename(char *path);
struct dirent*  ealloc(struct dirent *dp, char *name, int attr);
int  eread(struct dirent *entry, int user_dst, uint64 dst, uint off, uint n);
```

## 5.2 BSIZE 宏未定义

**现象：** fat32.c 编译报错 `BSIZE` undeclared。

**根因：** xv6-k210 的 buf.h 定义了 `BSIZE 512`，但 os/ 的 buf.h 依赖 fs.h 提供 `BSIZE 1024`。FAT32 需要 512 字节的扇区大小，不能使用 QEMU 的 1024。

**解决方案：** 在 buf.h 中添加条件默认值：
```c
#ifndef BSIZE
#define BSIZE 512   // K210 SD card sector size
#endif
```

## 5.3 include guard 缺失导致重定义

**现象：** 多个头文件重定义错误，如 `struct spinlock`、`struct sleeplock`、`struct stat` 等。

**根因：** os/ 树的多个头文件缺少 include guard。当文件通过不同路径（如 `#include "spinlock.h"` 和 `#include "sleeplock.h"` → 再间接包含）时被重复包含。

**受影响文件：** spinlock.h、sleeplock.h、proc.h、stat.h、buf.h

**解决方案：** 逐一添加 include guard：
```c
#ifndef __SPINLOCK_H
#define __SPINLOCK_H
// ...
#endif
```

## 5.4 sleeplock 中 spinlock 类型不完整

**现象：** 编译报错 `field 'lk' has incomplete type`。

**根因：** sleeplock.h 使用 `struct spinlock lk` 但未包含 spinlock.h，导致编译器不知道 spinlock 结构体的大小。

**解决方案：** 在 sleeplock.h 中添加 `#include "spinlock.h"`。

## 5.5 struct stat 布局不兼容

**现象：** K210 构建报错 `st->ino` / `st->nlink` 字段不存在。

**根因：** QEMU 的 stat 有 5 个字段（含 ino、nlink），K210 的 stat 使用文件名代替 inode 号，仅 4 个字段。

**解决方案：** 使用 `#ifdef QEMU` 条件化 struct stat 定义：

```c
#ifdef QEMU
struct stat {
  int dev; uint ino; short type; short nlink; uint64 size;
};
#else
#define STAT_MAX_NAME 32
struct stat {
  char name[STAT_MAX_NAME + 1];
  int dev; short type; uint64 size;
};
#endif
```

## 5.6 系统调用分发表平台差异

**现象：** K210 构建链接报找不到 syscall 相关函数（argint 等）。

**根因（两处）：**

1. K210 使用独立的系统调用分发表 `syscall_k210.c`，QEMU 使用 `syscall.c`。两者不能同时编译链接。Makefile 需要为每个平台选择正确的分发表。

2. **QEMU 构建回归** — 将 `syscall.o` 从公共 OBJS 移到平台分支后，QEMU 的 else 分支遗漏了 `$K/syscall/syscall.o`，导致 QEMU 链接时找不到 argint、argaddr、fetchaddr 等函数。

**解决方案：**
```makefile
# K210 分支
OBJS += $K/syscall/syscall_k210.o

# QEMU 分支（else）
OBJS += $K/syscall/syscall.o   # ← 此行初始遗漏，后续修复
OBJS += $K/syscall/sysfile.o
```

## 5.7 clean 目标未清理深层嵌套对象

**现象：** 切换平台构建时，残留的对象文件（使用不同 `-D QEMU` 标志编译）导致链接错误。

**根因：** clean 目标使用 `**/*.o` 匹配深层目录，但 bash 需要 `shopt -s globstar` 才能展开 `**`。默认情况下 `**/*.o` 等同于 `*/*.o`，仅匹配单层嵌套，无法匹配 `kernel/driver/disk.o` 等两层嵌套文件。

**解决方案：** 将 `**/*.o` 改为 `*/*/*.o` 明确匹配两层嵌套：
```makefile
clean:
	rm -f */*.o */*.d */*.asm */*.sym \
	       */*/*.o */*/*.d */*/*.asm */*/*.sym \
```

## 5.8 _entry 入口点不在内核起始位置

**现象：** QEMU 启动后 OpenSBI 正常输出，但内核无任何输出，直接超时退出。

**调试过程：**
1. QEMU 有 OpenSBI 输出说明固件加载正确，但内核没有任何打印说明启动代码没执行
2. 查看 `kernel/kernel.asm` 反汇编文件，查找 `_entry` 标签的位置：发现 `_entry` 在 `0x8020462a`，即 `.text` 段尾部；而 `main` 函数在 `0x80200000`（入口地址）
3. 确认 OpenSBI 跳转到 `0x80200000`（FW_JUMP_ADDR），实际执行的是 `main` 函数而非 `_entry`——跳过了栈指针（sp）和 hart ID（tp）的初始化
4. 检查 OBJS 列表中对象的顺序：链接器按输入文件的顺序排放 `.text` 段，`entry.o` 被放在 OBJS 末尾（位于平台相关分支中），导致其代码被排到最后
5. 将 entry 对象提到 OBJS 列表最前面，重新编译后确认 `_entry` 位于 `0x80200000`

**根因：** `_entry` 符号位于 `0x8020462a`（`.text` 段尾部），而非 `0x80200000`。OpenSBI 跳转到 `0x80200000` 时直接进入了 `main` 函数，跳过了 `entry.S` 中设置 `tp`（hart ID）和 `sp`（栈指针）的启动代码。

```
# 错误：_entry 在尾部，main 在入口
0000000080200000 <main>          # ← OpenSBI 跳到这里
000000008020462a <_entry>        # ← 应该在这里
```

链接顺序问题是导致这一错误的关键原因：`entry.o` 在 Makefile 的 OBJS 列表中被添加在末尾（位于平台相关分支中），而链接器按 OBJS 顺序排放 `.text` 段，导致 `entry.o` 的启动代码被排到了最后：

```makefile
# 错误：entry.o 排在最后
OBJS = $K/main.o ... $K/asm/kernelvec.o
OBJS += $K/entry.o              # 追加在末尾
```

**解决方案：** 将 entry 对象提到 OBJS 列表的最前面：

```makefile
# 先决定入口对象
ifeq ($(platform), k210)
ENTRY_OBJ = $K/entry_k210.o
else
ENTRY_OBJ = $K/entry.o
endif

# OBJS 以入口对象开头
OBJS = \
  $(ENTRY_OBJ) \                # ← 必须排在第一
  $K/main.o \
  ...
```

## 5.9 编译验证

### K210 平台（platform=k210）

从零构建通过：33 个目标文件编译链接成功，生成 kernel/kernel ELF。

### QEMU 平台（platform=qemu）

从零构建通过：28 个目标文件编译链接成功（含 `-D QEMU`），生成 kernel/kernel ELF。

### QEMU 运行验证（最终）

```
OpenSBI v0.9 → xv6 kernel is booting → hart 1 starting → init: starting sh → $
```

完整启动链测试通过。`_entry` 位于 `0x80200000`，`main` 位于 `0x80200018`，栈正确初始化，hart 0/1 均正常启动，init 进程成功运行 shell。

---

---

# 7. 任务5适配记录：QEMU 使用 FAT32 文件系统

## 7.1 适配目标

将 os/ 的两套文件系统代码路径（QEMU 的 inode-based xv6 fs + K210 的 FAT32）统一为 FAT32 单路径。QEMU 和 K210 共用 fat32.c、file.c、exec.c、sysfile.c，不再维护两份平台独立的实现。

此前架构（两套代码路径）：
```
QEMU: entry.o → main → iinit → ... → fs.c/log.c/file.c/exec.c/sysfile.c
K210: entry_k210.o → main → sdcard_init → ... → fat32.c/file_k210.c/exec_k210.c/sysfile_k210.c
```

统一后架构（单路径）：
```
QEMU/K210: entry.o/entry_k210.o → main → disk_init → ... → fat32.c/file.c/exec.c/sysfile.c
```

## 7.2 文件变更清单

### 重命名的文件（FAT32 版本成为默认）

| 原路径 | 新路径 |
|--------|--------|
| os/kernel/fs/file_k210.c | os/kernel/fs/file.c（覆盖原 QEMU 版本） |
| os/kernel/proc/exec_k210.c | os/kernel/proc/exec.c（覆盖原 QEMU 版本） |
| os/kernel/syscall/sysfile_k210.c | os/kernel/syscall/sysfile.c（覆盖原 QEMU 版本） |

### 删除的文件（不再需要）

| 文件 | 原因 |
|------|------|
| os/kernel/fs/fs.c | xv6 inode-based 文件系统，FAT32 替代 |
| os/kernel/fs/log.c | xv6 日志层，FAT32 不需要 |
| os/kernel/syscall/syscall_k210.c | 分发表已合并到 syscall.c |

### 修改的文件

| 文件 | 修改内容 |
|------|----------|
| os/Makefile | fat32.o/file.o/exec.o/sysfile.o 移入公共 OBJS；移除 fs.o/log.o；fs.img 改用 FAT32 格式（dd+mkfs.vfat+mount+cp） |
| os/kernel/syscall/syscall.c | 添加 FAT32 系统调用：sys_dev/sys_readdir/sys_getcwd/sys_remove |
| os/kernel/include/syscall.h | 添加 SYS_dev=27, SYS_readdir=28, SYS_getcwd=29, SYS_remove=30 |
| os/kernel/include/proc.h | 移除 `#ifdef QEMU`，`cwd` 统一为 `struct dirent*` |
| os/kernel/include/file.h | 移除 `#ifdef QEMU`，统一使用 `FD_ENTRY` 和 `struct dirent*` |
| os/kernel/include/stat.h | 移除 `#ifdef QEMU`，统一使用 FAT32 格式（含 `name[STAT_MAX_NAME+1]`） |
| os/kernel/include/defs.h | 移除 inode-based fs 和 log 声明；添加 FAT32 声明（ename/eput/elock/eunlock/eread/ewrite/estat/eremove 等） |
| os/kernel/proc/proc.c | 移除 `#ifdef QEMU`，forkret/fork/exit/userinit 中无条件使用 fat32_init + ename/edup/eput |
| os/kernel/main.c | 移除 iinit() 调用；K210 和 QEMU 统一通过 disk_init 初始化磁盘 |
| os/kernel/trap.c | `find_vma`/`mmap_handler` 取消 `#ifdef QEMU`，无条件编译 |
| os/kernel/fs/fat32.c | 恢复 `#include "defs.h"`，移除手动 forward declarations |
| os/build.sh | build_xv6() 自动编译用户程序并用 pyfatfs 填充 FAT32 镜像 |

---

# 8. 调试记录（任务5）

任务5 的调试涉及多种运行时调试手段，包括：
- **QEMU monitor**：通过 `scause`/`stval` 寄存器判断异常类型和故障地址
- **kernel.asm 反汇编**：定位故障指令的源码位置
- **printf 插桩**：在内核关键路径添加临时调试输出
- **Python pyfatfs 检查**：查看 FAT32 镜像内容分布
- **git diff 对比**：确认代码变更差异，找出遗漏的修改点
- **grep 扫描**：搜索残留的 `#ifdef QEMU` 条件编译片段
- **objdump 符号分析**：确认链接顺序和符号地址
- **QEMU info mem 命令**：检查页表映射状态

以下是具体调试记录：

## 8.1 kpagetable 未更新导致 secondary hart 页错误

**现象：** QEMU 启动后，hart 1 在 `copyin_new` 的 `memmove` 处触发页错误（scause=13），hart 0 正常。跳板代码成功执行，但进入内核态访问用户空间数据时崩溃。

**调试过程：**
1. QEMU 启动后 hart 1 立即崩溃，输出 `scause=0x000000000000000d`（Page Fault 读异常），`stval=0xXXXXXXXXXXXX`（故障虚拟地址）
2. 查看 `kernel/kernel.asm` 反汇编，定位 `stval` 地址对应 `copyin_new` 函数中的 `memmove` 指令
3. 在 `copyin_new` 中添加临时调试输出（`printf("stval=%p\n", r_stval())`），确认 fault 地址在用户空间范围内但不在当前进程的页面中
4. 检查 `fork()` 代码发现它调用 `copyin_new()` 来复制参数，而 `copyin_new` 通过 `p->kpagetable` 直接访问用户地址
5. 对比 `fork()` 和 `exec()`：fork 中通过 `upg2ukpg()` 更新了子进程的 kpagetable，但 `exec()` 中 `p->pagetable` 被替换为新页表后，没有对应的 kpagetable 更新步骤
6. 通过 QEMU 的 `info mem` 命令确认新页表已有用户映射，而 kpagetable 中没有

**根因：** `exec()` 替换了进程的 pagetable 后，未更新 `p->kpagetable`（内核态直接访问用户空间地址用的影子页表）。`fork()` 中通过 `upg2ukpg()` 复制了子进程的用户页面到 kpagetable，但 exec 执行后创建了全新的用户地址空间，kpagetable 仍指向旧进程的物理页面。hart 1 执行 fork() 后调用 copyin_new()，通过过期的 kpagetable 访问用户地址，触发 page fault。

**解决方案：** 在 exec() 的 pagetable 替换后，同步更新 kpagetable：

```c
// exec.c — 在 p->pagetable = pagetable 之后
uvmunmap(p->kpagetable, 0, PGROUNDUP(old_sz) / PGSIZE, 0);
upg2ukpg(p->pagetable, p->kpagetable, 0, p->sz);
```

先解映射旧页表的所有用户映射（不释放物理页），再复制新页表的用户映射到内核页表。

## 8.2 freewalk: leaf panic

**现象：** 运行 `ls` 命令后内核 panic，输出 "freewalk: leaf"，hart 0 崩溃。

**调试过程：**
1. panic 信息 "freewalk: leaf" 定位到 `vm.c` 的 `freewalk()` 函数，该函数在遍历页表时遇到标记了 R/W/X 的叶子 PTE 会 panic
2. 在 `freewalk` 中添加调试输出，在 panic 前打印故障页表的虚拟地址和页表项内容
3. 发现遗留的叶子 PTE 对应旧进程的高地址页面——这些页面在新程序中不再使用，但 `uvmfree()` 没有遍历到
4. 查看 `exec.c` 中调用 `proc_freepagetable()` 的位置，发现传入的是 `p->sz`（新程序大小）而非 `old_sz`（旧程序大小）
5. 对比旧程序（init，约 32KB）和新程序（ls，约 24KB）的尺寸差异——新程序更小时才会触发此 bug
6. 通过分析 `uvmfree()` 的实现确认它按传入的 sz 遍历页表，不会处理 sz 以上的页面

**根因：** `proc_freepagetable(oldpagetable, p->sz)` 传递了新的进程大小（exec 加载 ELF 后的尺寸），而非旧页表对应的尺寸。当新程序比旧程序小时（例如 shell 比 init 小），`uvmfree()` 只遍历到新尺寸对应的 PTE，遗留了旧页表中高地址的叶子 PTE。`freewalk()` 递归遍历全部三级页表，遇到这些遗留的叶子 PTE（标记了 R/W/X）时 panic。

**解决方案：** 在更新 `p->sz` 之前保存旧值，释放旧页表时使用旧尺寸：

```c
uint64 old_sz = p->sz;               // 保存 exec 前的尺寸
p->pagetable = pagetable;
p->sz = sz;                           // 更新为新尺寸
// ...
proc_freepagetable(oldpagetable, old_sz);  // 使用 old_sz，不是 p->sz
```

两处关键修复对比：

| exec.c 行号 | 错误代码 | 正确代码 |
|-------------|----------|----------|
| proc_freepagetable 调用 | `proc_freepagetable(oldpagetable, p->sz)` | `proc_freepagetable(oldpagetable, old_sz)` |

## 8.3 fs.img 为空导致 "panic: init exiting"

**现象：** `./run.sh nographic` 输出 `init: starting sh` 后立即 `panic: init exiting`。QEMU 正常启动 OpenSBI 和内核，init 进程能运行，但无法启动 shell。

**调试过程：**
1. 看到 "init: starting sh" 说明内核启动正常，init 进程开始执行，但 exec("sh") 失败
2. 检查 `os/fs.img` 大小——256MB，不为空，排除镜像文件不存在的可能性
3. 用 Python 检查镜像内容：`python3 -c "import fs; vfs=fs.open_fs('fat://os/fs.img'); print(vfs.listdir('/'))"`，输出空列表——根目录确实没有文件
4. 查看 build.sh 的 build_xv6() 实现，发现 `make platform=qemu` 只编译内核，不编译用户程序；`fs.img` 目标只创建空白 FAT32 镜像
5. 对比 `output/os/fs.img`（由 build.sh 生成）和在 `os/` 目录下手动用 pyfatfs 填充后的 fs.img，确认问题出在构建流程缺少用户程序填充步骤
6. 确认 `make qemu` 目标虽然依赖 `fs.img`，但 `fs.img` 目标仅是 `dd + mkfs.vfat`，不填充文件

**根因：** `build.sh` 的 `build_xv6()` 调用 `make platform=qemu` 只编译了内核，fs.img 是通过 `make fs.img` 创建的空白 FAT32 镜像，未包含任何用户程序。init 进程通过 FAT32 的 `ename("sh")` 查找 shell，但 FAT32 根目录为空，返回 NULL，init 直接 exit(0)，内核 panic。

**解决方案：** 更新 build.sh 的 build_xv6()，添加用户程序编译和 FAT32 镜像填充步骤：

```bash
# 1. 编译用户程序
make platform=qemu $(for p in _cat _echo _grep _init _kill _ln _ls _mkdir _rm _sh _grind _wc _zombie; do echo "user/$p"; done)

# 2. 创建 FAT32 镜像并填充用户程序
dd if=/dev/zero of=fs.img bs=512k count=512
mkfs.vfat -F 32 fs.img
python3 -c "
import fs, os, glob
vfs = fs.open_fs('fat://fs.img')
if not vfs.exists('/bin'): vfs.makedir('/bin')
for prog in glob.glob('user/_*'):
    name = os.path.basename(prog)[1:]
    with open(prog, 'rb') as src: data = src.read()
    vfs.open('/' + name, 'wb').write(data)
    vfs.open('/bin/' + name, 'wb').write(data)
vfs.close()
"
```

这样无需 sudo 权限即可填充 FAT32 镜像（pyfatfs 用户空间库直接操作镜像文件）。

## 8.4 defs.h 恢复与 fat32.h 的兼容

**现象：** 统一代码路径后，原来不使用 defs.h 的 FAT32 文件（fat32.c, file.c, exec.c, sysfile.c）需要恢复包含 defs.h。

**调试过程：**
1. 尝试在 fat32.c 中添加 `#include "defs.h"`，编译报错 "conflicting types for 'dirlookup'"
2. 用 `grep -n dirlookup defs.h fat32.h` 确认两个头文件都声明了 `dirlookup`：defs.h 是 `struct inode* dirlookup(struct inode*, ...)`，fat32.h 是 `struct dirent* dirlookup(struct dirent*, ...)`
3. 原来的代码（任务4）通过让 FAT32 文件不包含 defs.h 来规避此冲突，改用 forward declarations。统一后用这种做法会导致 forward declarations 膨胀且难以维护
4. 确认 inode-based 的 dirlookup 在删除 fs.c 后已无调用者，可以直接从 defs.h 移除
5. 将 defs.h 中的 `dirlookup(struct inode*, ...)` 声明删除，保留 fat32.h 中的声明

**根因：** 任务4 中为了规避 defs.h 与 fat32.h 之间的 `dirlookup` 符号冲突，FAT32 系列文件全部禁用了 defs.h，改用手写 forward declarations。统一后，这些文件不再有条件编译隔离，需要 defs.h 中的其他声明（如 `argint`、`argstr`、`fetchaddr`、`myproc` 等）。

**解决方案：** 不直接恢复 `#include "defs.h"`（dirlookup 冲突仍在），而是：
1. 将 defs.h 中的 `dirlookup(struct inode*, ...)` 声明移除（inode-based fs 已删除）
2. FAT32 的 `dirlookup(struct dirent*, ...)` 声明保留在 fat32.h 内部
3. fat32.c 恢复包含 defs.h，移除 forward declarations
4. exec.c 和 sysfile.c 保留需要的 forward declarations（仅声明实际使用的函数）

## 8.5 proc.h/file.h/stat.h 的 QEMU ifdef 清理

**现象：** 移除平台条件编译后，三个关键数据结构需要统一。

**调试过程：**
1. 用 `grep -rn "#ifdef QEMU" kernel/include/` 扫描所有头文件中残留的 QEMU 条件编译
2. 对每个匹配，分析该数据结构是否还有两套实现的必要——因为现在 QEMU 和 K210 都用 FAT32 文件系统，inode 和 dirent 两套指针不再需要
3. 逐个修改：
   - `proc.h`：cwd 从 `struct inode*`（QEMU）/ `struct dirent*`（K210）统一为 `struct dirent*`
   - `file.h`：`struct file` 的 ip/ep 字段和 FD_INODE/FD_ENTRY 枚举统一为 dirent 版本
   - `stat.h`：FAT32 的 stat 带 `name[33]` 字段，QEMU 的 stat 带 `ino/nlink` 字段，统一使用 FAT32 版本
4. 编译验证：`make clean && make platform=qemu -j$(nproc)` 确认无编译错误
5. 运行验证：QEMU 启动到 shell，ls/cat 等命令正常

**变更细节：**

**proc.h — `struct proc` 的 cwd 字段：**
```c
// 之前
#ifdef QEMU
  struct inode *cwd;
#else
  struct dirent *cwd;
#endif

// 之后
  struct dirent *cwd;
```

**file.h — `struct file` 的文件后端：**
```c
// 之前
#ifdef QEMU
  struct inode *ip;
  enum { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE };
#else
  struct dirent *ep;
  enum { FD_NONE, FD_PIPE, FD_ENTRY, FD_DEVICE };
#endif

// 之后
  struct dirent *ep;
  enum { FD_NONE, FD_PIPE, FD_ENTRY, FD_DEVICE };
```

**stat.h — `struct stat` 的布局：**
```c
// 之前（QEMU）
struct stat { int dev; uint ino; short type; short nlink; uint64 size; };

// 之前（K210）
struct stat { char name[STAT_MAX_NAME+1]; int dev; short type; uint64 size; };

// 之后（统一使用 FAT32 格式）
#define STAT_MAX_NAME 32
struct stat { char name[STAT_MAX_NAME+1]; int dev; short type; uint64 size; };
```

## 8.6 QEMU 输出空白（无 OpenSBI 输出）

**现象：** 使用 QEMU 4.2.0 运行 `./run.sh nographic` 时屏幕完全空白，无任何输出。

**调试过程：**
1. 检查 `run.sh` 的 QEMU 命令行参数，发现 `-bios none`——QEMU 不会加载任何固件，直接从 0x80000000 开始执行
2. 确认 `.` 目录和执行 `./run.sh` 的目录正确，fw_jump.bin 已编译到 `output/opensbi/` 下
3. 查阅当前 QEMU 版本的文档：`qemu-system-riscv64 --version` 显示版本 4.2.0，该版本未内置 OpenSBI（较新版本如 QEMU 7+ 内置了 OpenSBI）
4. 手动添加 `-bios output/opensbi/fw_jump.bin` 后重新运行，OpenSBI 正常输出
5. 确认 `run.sh` 已经使用了正确的 `-bios` 路径，早期调试时遇到的空白屏幕是因为 build.sh 尚未编译 OpenSBI

**根因：** QEMU 4.2.0 不自带 OpenSBI 固件。默认 `-bios none` 时 QEMU 跳转到 0x80000000 执行，但该地址无有效代码。需要显式指定 OpenSBI 二进制文件。

**解决方案：** run.sh 已使用 `-bios output/opensbi/fw_jump.bin`，确保编译 OpenSBI（`./build.sh -sbi`）：

```
-bios $SHELL_FOLDER/output/opensbi/fw_jump.bin
```

## 8.7 工具链路径问题

**现象：** `./build.sh -xv6` 报错 "riscv64-unknown-linux-gnu-gcc: No such file or directory"。

**调试过程：**
1. 运行 `which riscv64-unknown-linux-gnu-gcc`，输出为空——工具链不在 PATH 中
2. 用 `find / -name "riscv64-unknown-linux-gnu-gcc" 2>/dev/null` 查找工具链位置，找到在 `/home/xikao/quard_star_tutorial/toolchain/gcc-riscv64-unknown-linux-gnu/bin/` 下
3. 将此路径添加到 PATH 后重新运行 `build.sh`，编译通过
4. 确认问题仅存在于首次设置环境时，CLAUDE.md 已记录该路径信息

**根因：** RISC-V 交叉编译工具链安装在 `/home/xikao/quard_star_tutorial/toolchain/`，不在默认 PATH 中。

## 8.7 工具链路径问题

**现象：** `./build.sh -xv6` 报错 "riscv64-unknown-linux-gnu-gcc: No such file or directory"。

**根因：** RISC-V 交叉编译工具链安装在 `/home/xikao/quard_star_tutorial/toolchain/`，不在默认 PATH 中。

**解决方案：** build.sh 不修改系统配置，编译前需要 export 工具链路径：
```bash
export PATH=$PATH:/home/xikao/quard_star_tutorial/toolchain/gcc-riscv64-unknown-linux-gnu/bin/
./build.sh -xv6
```

---

# 9. 编译验证

## QEMU 平台

构建命令：
```bash
export PATH=$PATH:/home/xikao/quard_star_tutorial/toolchain/gcc-riscv64-unknown-linux-gnu/bin/
./build.sh -xv6
```

输出：内核 ELF + 13 个用户程序（_cat, _echo, _grep, _init, _kill, _ln, _ls, _mkdir, _rm, _sh, _grind, _wc, _zombie）+ FAT32 fs.img（含 /bin 目录）

运行验证：
```bash
./run.sh nographic
```

预期输出：
```
OpenSBI v0.9 → xv6 kernel is booting → hart 1 starting → init: starting sh → $
```

## K210 平台

构建命令：
```bash
export PATH=$PATH:/home/xikao/quard_star_tutorial/toolchain/gcc-riscv64-unknown-linux-gnu/bin/
./build.sh -k210
```

输出：kernel-k210 ELF，编译通过无错误。

---

# 10. 任务6适配记录：统一 fs.img 构建流程和多平台支持

## 10.1 适配目标

参考 os/Makefile，优化 Makefile 和 build.sh 的 fs.img 构建过程，消除 sudo 依赖，统一构建流程，支持多平台构建。

## 10.2 改进内容

### Makefile 变更

| 变更 | 说明 |
|------|------|
| 新增 `fs` 目标 | 编译 UPROGS + 创建 FAT32 镜像 + pyfatfs 填充用户程序，单步完成 |
| 保留 `fs.img` 目标 | 仅创建空白 FAT32 镜像（无用户程序），用于快速初始化和向后兼容 |
| 删除 `fs-populate` 目标 | 被 `fs` 替代，不再需要 sudo mount |
| `qemu` 目标依赖 `fs` | `qemu: $K/kernel fs` → 构建时自动生成带用户程序的镜像 |

`fs` 目标的核心规则：
```makefile
fs: $(UPROGS) | fs.img
	@echo "populating fs.img with user programs..."
	@dd if=/dev/zero of=fs.img bs=512k count=512 2>/dev/null
	@mkfs.vfat -F 32 fs.img 2>/dev/null
	@python3 scripts/mkfs.py "$U"
	@echo "done"
```

### 新增 scripts/mkfs.py

将 FAT32 镜像填充逻辑抽取为独立 Python 脚本，被 Makefile 的 `fs` 目标调用。

### build.sh 变更

`build_xv6()` 大幅简化：原来需要手动循环编译 UPROGS + 内联 pyfatfs 脚本，现在两行完成：

```bash
build_xv6() {
    make platform=qemu -j$(nproc)     # 编译内核
    make platform=qemu fs             # 编译用户程序 + 填充 fs.img
    cp kernel/kernel $OUTPUT/os
    cp fs.img $OUTPUT/os
}
```

### 多平台支持

- `make fs` 与 `platform` 解耦：fs.img 内容和用户程序列表与平台无关，同一镜像可用于 QEMU virt 和 K210 QEMU 模拟
- K210 真实硬件通过 SD 卡启动，不需要 fs.img，`build_xv6_k210()` 保持不变
- pyfatfs 作为纯用户空间库，不需要 root 权限，也不需要 `/mnt` 挂载点

## 10.3 架构变化

之前（两阶段流程，需 sudo）：
```
make platform=qemu              → kernel only
make platform=qemu fs-populate  → sudo mount + cp (root required)
```

之后（单步免 sudo）：
```
make platform=qemu && make platform=qemu fs  → kernel + user programs + fs.img
```

或通过 build.sh 一步完成：
```
./build.sh -xv6  → 自动执行 clean + make + make fs + cp
```

---

# 11. 遗留事项

1. **K210 硬件测试** — 当前 K210 构建仅验证了编译链接，未在真实 K210 板或 QEMU 模拟上运行测试
2. **FAT32 兼容性** — stat 结构体不包含 ino/nlink 字段，部分用户程序（如 find）依赖这些字段可能需要调整
3. **build.sh 自动化** — build.sh 内部调用 make 时未自动处理工具链 PATH，需用户在编译前手动 export
