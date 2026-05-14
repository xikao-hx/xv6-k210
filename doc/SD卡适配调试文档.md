
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

# 6. 遗留事项

1. **K210 硬件测试** — 当前 K210 构建仅验证了编译链接，未在真实 K210 板或 QEMU 模拟上运行测试
2. **用户程序移植** — K210 平台需要额外的用户程序（如 FAT32 感知的 ls、文件操作等）
3. **FAT32 兼容性** — stat 结构体缺少 ino/nlink 字段，部分依赖这些字段的用户程序可能需要调整
