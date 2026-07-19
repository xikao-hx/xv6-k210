# mmap Linux 风格核心子集重构方案

日期：2026-07-12  
范围：进程虚拟地址空间、页故障处理、FAT32 文件映射、匿名内存和设备共享缓冲区  
目标平台：QEMU 与 K210

---

## 1. 目标定位

本次重构以“Linux 风格核心子集”为目标，不追求完整复制 Linux 的 mmap ABI 和内存管理子系统。重构后应满足以下主要需求：

1. 文件的私有映射和共享映射。
2. 用于大块内存分配的按需分页匿名映射。
3. fork 后正确的 COW 或共享语义。
4. 通过设备 fd 映射专用内核缓冲区，支持用户态与驱动零拷贝交互。
5. 正确的部分 munmap、进程退出和资源引用计数。

暂不将 `mprotect`、`msync`、`madvise`、swap、huge page、`MAP_FIXED` 和 Linux 完整 `SIGBUS` 语义纳入首轮目标，避免为暂无调用方的边缘功能显著提高 VM 和 FAT32 复杂度。

---

## 2. 现有实现的问题

### 2.1 mmap 与进程堆共用 `p->sz`

现有 `sys_mmap()` 将新 VMA 放在 `p->sz` 处，并将映射长度累加到 `p->sz`。这会导致 heap、lazy allocation、fork 的 `uvmcopy()` 和 mmap 映射区域互相混淆，也无法防止后续 `sbrk()` 与 VMA 重叠。

### 2.2 VMA 只能表示文件映射

当前 VMA 默认保存 `struct file *`，fork 和 exit 也无条件执行 `filedup()` 和 `fileclose()`。引入匿名映射或内核缓冲区后，这种数据结构无法表达不同的 backing object 及其生命周期。

### 2.3 权限与缺页判定不完整

现有缺页处理只覆盖 load/store fault，未完整处理 instruction fault，且使用文件的 readable/writable 属性判断访问权限，没有严格按 `PROT_READ`、`PROT_WRITE` 和 `PROT_EXEC` 判定。

`MAP_PRIVATE | PROT_WRITE` 不要求底层文件可写；只有 `MAP_SHARED | PROT_WRITE` 必须要求文件可写。

### 2.4 文件末页和写回逻辑不正确

文件读取返回 0 时会直接将缺页视为失败，不能正确处理文件末尾不足一页的区域。正确行为应为先清零整页，再仅读取页面与文件内容相交的部分。

现有 `munmap()` 通过 `filewrite()` 整段写回，会依赖文件当前 offset，也会访问尚未驻留的用户页。写回应逐个已驻留页执行，并使用 VMA offset 而不是 `file->off`。

### 2.5 munmap 及双页表同步不完整

现有页数按 `length / PGSIZE` 计算，不足整页时可能少解除映射。同时只修改用户 `pagetable`，没有同步解除 `kpagetable` 中的映射，可能保留指向已释放物理页的旧 PTE。

当前也不支持从 VMA 中间解除映射并拆分成两个 VMA。

### 2.6 内核主动访问未驻留 VMA 会失败

用户将尚未缺页分配的 mmap 地址传给 `read()`、`write()` 或 ioctl 时，`copyin/copyout` 可能因 PTE 不存在而直接失败。内核拷贝路径必须能够对合法 VMA 调用统一的 fault-in 逻辑。

---

## 3. 支持的功能边界

### 3.1 文件映射

- `MAP_PRIVATE`：按需读入，fork 后 COW，修改不写回文件。
- `MAP_SHARED`：按需读入，对可写共享映射执行写回。
- 支持页对齐的非零文件 offset。
- 支持文件末尾不足一页的映射。
- 关闭 fd 后，已建立映射仍然有效。

如果要达到近似 Linux 的 `MAP_SHARED` 多进程即时可见性，需要增加按“文件身份 + 页 offset”索引的 mmap page cache。首轮可先保证单进程写回正确，共享 page cache 作为后续阶段。

### 3.2 匿名映射

用户接口：

```c
mmap(0, length,
     PROT_READ | PROT_WRITE,
     MAP_PRIVATE | MAP_ANONYMOUS,
     -1, 0);
```

支持：

- 只保留虚拟地址，mmap 时不立即分配物理页。
- 首次访问时逐页分配并清零。
- 大块虚拟内存不要求物理内存连续。
- `MAP_PRIVATE` 在 fork 后使用现有 COW 机制。
- `MAP_SHARED` 在 fork 后共享同一 anonymous object 和物理页。

匿名 mmap 可以提供较大虚拟地址空间，但无 swap 时实际驻留容量仍受 `PHYSTOP` 限制。

### 3.3 内核共享缓冲区

支持通过设备 fd 映射专用、页对齐的 page-backed kbuf：

```text
用户虚拟地址 ──┐
                 ├── 同一组物理页
内核 kbuf ─────┘
```

应用场景包括设备共享环形队列、SPI/SD 批量传输、数据采集和 DMA 缓冲区。

禁止直接映射现有 buffer cache 的 `struct buf::data`。该数据区只有 512 字节且内嵌在包含锁、内核指针和相邻 buf 数据的内核页中，直接映射会暴露内核元数据并破坏生命周期管理。

---

## 4. 虚拟地址空间布局

mmap 区域与 `p->sz` 分离，建议从高地址向下分配：

```text
低地址
+--------------------------+
| ELF text/data            |
| heap / sbrk              |  向上增长
+--------------------------+
| 未分配空洞                 |
+--------------------------+
| mmap VMA                 |  从 MMAP_TOP 向下分配
+--------------------------+
| trapframe / trampoline   |
+--------------------------+
高地址
```

`growproc()` 必须检查新 heap end 是否与最低 VMA 重叠；VMA 插入必须检查地址溢出、页对齐和区间重叠。

---

## 5. 核心数据结构

VMA 只描述进程虚拟地址区间，具体资源由 mmap object 管理：

```c
enum vma_type {
  VMA_FILE,
  VMA_ANON,
  VMA_KBUF,
};

struct vma_area {
  int used;
  enum vma_type type;
  uint64 start;
  uint64 end;          // exclusive，页对齐
  uint64 valid_end;    // 用户请求的实际末尾
  uint64 offset;
  int prot;
  int flags;
  struct mmap_object *object;
};
```

```c
struct mmap_object {
  struct spinlock lock;
  int refcnt;
  enum vma_type type;
  union {
    struct file *file;
    struct anon_object *anon;
    struct kbuf_object *kbuf;
  };
};
```

VMA 不保存 fd。fd 可以被用户关闭，已建立的映射通过 `mmap_object` 引用继续持有底层资源。

---

## 6. 统一缺页处理

将 trap 和内核拷贝路径统一到：

```c
int vm_fault(struct proc *p, uint64 va, int access);
```

处理流程：

```text
访问地址
    |
    v
查找 VMA 并校验 PROT 权限
    |
    +-- 无 VMA 或权限不符 --> 失败/终止进程
    |
    v
按 VMA 类型取得物理页
    |
    +-- FILE: 分配页并按 offset 读取
    +-- ANON: 分配零页或取共享匿名页
    +-- KBUF: 取已有 kbuf 物理页
    |
    v
安装用户 PTE
    |
    v
同步 kpagetable 并刷新 TLB
```

fault 类型与权限必须匹配：

- instruction fault 需要 `PROT_EXEC`。
- load fault 需要 `PROT_READ`。
- store fault 需要 `PROT_WRITE`。

`copyin`、`copyout` 和 `copyinstr` 遇到未驻留 PTE 时，应在确认地址属于合法 VMA 后调用同一 `vm_fault()`。

---

## 7. munmap 和页面所有权

`munmap()` 应支持：

- 删除完整 VMA。
- 裁剪 VMA 头部或尾部。
- 从 VMA 中间打洞并拆分为两个 VMA。
- 一次解除覆盖多个 VMA 的区间。
- 跳过尚未驻留的页，不将其视为错误。

不能继续用单一 `do_free` 布尔值处理所有页：

- 私有匿名页和 COW 页：减少物理页引用。
- 共享文件页：减少 page cache 页引用，必要时写回。
- 共享匿名页：减少 anonymous object 页引用。
- kbuf 页：仅减少映射引用，不能由 `uvmunmap()` 直接释放。

每个已驻留页都必须同时从 `pagetable` 和 `kpagetable` 解除。中间拆分 VMA 前应先预留空 VMA 槽，如果没有空槽应在修改页表前整体失败。

---

## 8. fork、exec 和 exit

fork 对不同映射的处理如下：

| 映射类型 | fork 后行为 |
|---|---|
| 文件 `MAP_PRIVATE` | 已驻留页 COW |
| 文件 `MAP_SHARED` | 共享同一文件映射页 |
| 匿名 `MAP_PRIVATE` | 已驻留页 COW |
| 匿名 `MAP_SHARED` | 共享同一 anonymous object |
| `VMA_KBUF` | 默认共享同一 kbuf object |

建议将普通用户内存与 VMA 复制分开：

```c
uvmcopy_heap(parent, child, p->sz);
vma_fork(parent, child);
```

exec 成功替换地址空间时释放旧 VMA；exec 失败回滚时不影响旧映射。exit 通过统一 `vma_destroy_all()` 完成写回、解除映射和 object 引用释放，不在 `proc.c` 重复实现各类 VMA 逻辑。

---

## 9. 设备 kbuf 设计

建议在设备抽象层增加 mmap 回调：

```c
struct devsw {
  int (*read)(int minor, uint64 addr, int len);
  int (*write)(int minor, uint64 addr, int len);
  int (*ioctl)(int minor, uint64 cmd, uint64 arg);
  int (*mmap)(int minor, struct mmap_request *req,
              struct mmap_object **object);
};
```

驱动负责决定：

- 设备是否允许 mmap。
- 最大映射长度和合法 offset。
- 允许的 `PROT_*` 权限。
- 映射对应哪个 kbuf object。
- DMA 是否要求物理连续。
- DMA 前后是否需要 cache 同步。

kbuf 需要同时管理驱动引用、fd 引用和 VMA 引用。关闭 fd 后，已有映射应继续有效；只有所有引用归零后才能释放物理页。

CPU 共享缓冲区可使用离散物理页。如 K210 DMAC 要求物理连续，应建立独立的连续 DMA 分配接口，不将其混入通用匿名 mmap 分配器。

---

## 10. 建议的代码分层

- `kernel/include/mmap.h`：VMA、mmap object、fault 访问类型及函数声明。
- `kernel/vm/mmap.c`：VMA 查找、地址分配、fault-in、munmap、fork 和销毁。
- `kernel/vm/kbuf.c`：page-backed kbuf 分配、引用计数和物理页查询。
- `kernel/syscall/sysmmap.c`：系统调用参数解析、ABI 校验和 mmap object 创建。
- `kernel/fs/file.c`：提供不修改 `file->off` 的指定 offset 读写接口。
- `kernel/trap/trap.c`：仅将 fault 原因转换为 read/write/exec 并调用 `vm_fault()`。
- `kernel/proc/proc.c`：仅调用 `vma_fork()` 和 `vma_destroy_all()`。

依赖方向保持为：

```text
syscall / trap / proc
          |
          v
       mmap VM
       /     \
      v       v
 file backing  anon/kbuf backing
```

文件系统和设备驱动不应反向依赖进程缺页处理细节。

---

## 11. 分阶段实施

### 阶段一：修复文件 mmap 基础

实施内容：

- mmap 地址区域与 `p->sz` 分离。
- 引入统一 VMA 查找、插入和权限校验。
- 正确处理文件末页和 read/write/exec fault。
- 修复页数对齐、部分 munmap 和双页表同步。
- 拆分不依赖 `file->off` 的 mmap 文件读写接口。

成功标准：现有文件 mmap 测试通过，新增文件末页、非零 offset、权限错误和 VMA 中间 munmap 测试。

### 阶段二：私有匿名 mmap

实施 `MAP_PRIVATE | MAP_ANONYMOUS`，完成零页按需分配、fork COW、部分 unmap 和 `copyin/copyout` fault-in。

成功标准：建立大 VMA 时物理页不增长，逐页访问后按需增长，fork 后父子写入互不影响。

### 阶段三：共享匿名 mmap

引入 anonymous object 及其共享页管理，实现 `MAP_SHARED | MAP_ANONYMOUS`。

成功标准：fork 后父子进程对共享页的修改立即互相可见，任一进程退出不会破坏另一方映射。

### 阶段四：kbuf 和设备 mmap

引入 page-backed kbuf、设备 mmap 回调和完整引用计数。

成功标准：用户与驱动可访问同一组物理页，关闭 fd 后映射仍有效，最后一个引用释放后无页面泄漏。

### 阶段五：完善文件 MAP_SHARED

引入共享文件 mmap page cache，实现同一文件 offset 的多映射页共享和 dirty 写回。

成功标准：不同进程映射同一文件区域后可立即看到对方修改，解除映射或回收页时正确写回 FAT32。

---

## 12. 验证矩阵

| 测试类别 | 关键用例 |
|---|---|
| 参数校验 | 零长度、溢出、错误 flags、未对齐 offset、非法 fd |
| 权限 | `PROT_NONE`、只读页写入、不可执行页取指、只读文件共享写映射 |
| 文件 | 末尾短页、非零 offset、PRIVATE 不写回、SHARED 写回、先 close fd 后继续访问 |
| 匿名 | 初始全零、懒分配、大块 VMA、fork COW、fork 共享 |
| munmap | 全部、头部、尾部、中间打洞、跨 VMA、未驻留页 |
| 生命周期 | fork、exit、exec、fd 提前关闭、多进程解除共享页 |
| kbuf | 越界拒绝、权限限制、关闭顺序、fork 继承、驱动与用户数据一致 |
| 双页表 | fault-in 后两份 PTE 一致，unmap 后两份 PTE 同时消失 |

每个阶段至少执行：

```sh
make build platform=qemu
make fs
```

涉及缺页、fork、文件写回或设备行为时，应运行 QEMU 功能测试。涉及 kbuf、DMA 或 K210 设备后，还必须执行：

```sh
make build platform=k210
```

---

## 13. 明确不支持的 Linux 功能

首轮重构不保证以下 Linux 行为：

- `MAP_FIXED` 和 `MAP_FIXED_NOREPLACE`。
- `mprotect()`、`msync()`、`madvise()` 和 `mlock()`。
- swap、内存压缩和页面换出。
- huge page、NUMA 和 ASLR。
- 映射期间文件截断导致的完整 `SIGBUS` 语义。
- 将任意内核虚拟地址或现有 `struct buf` 直接暴露给用户。
- 不借助共享 fd 或其他命名对象的无亲缘进程匿名共享。

---

## 14. 结论

重构后的 mmap 定位为 Linux 风格的核心子集，而不是 Linux 完整兼容实现。它应稳定支持：

- 文件 `MAP_PRIVATE` 和 `MAP_SHARED`。
- 匿名 `MAP_PRIVATE` 和 `MAP_SHARED`。
- 按需分页、fork COW 和共享页生命周期。
- 完整的部分 munmap。
- 通过设备 fd 映射安全的 page-backed 内核 kbuf。

该边界能够满足大块匿名内存、文件映射和驱动零拷贝需求，同时使 VM、FAT32 和设备驱动的职责边界保持清晰，为后续增加共享 page cache、`mprotect` 或 `msync` 留出可扩展结构。
