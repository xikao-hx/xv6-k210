# mmap 架构记录

## 当前目录与文件作用

- `kernel/syscall/sysfile.c`：只解析 mmap/munmap ABI 参数并调用 mmap VM。
- `kernel/proc/proc.c`：调用 `vma_fork/vma_destroy_all/vma_heap_limit`，不解释 backing。
- `kernel/proc/exec.c`：在替换地址空间的不可失败提交点销毁旧 VMA。
- `kernel/trap/trap.c`：将 instruction/load/store fault 转换为 access 并调用 `vm_fault`。
- `kernel/vm/vm.c`：通用页表、lazy allocation 和 COW。
- `kernel/vm/vmcopyin.c`：逐页执行受权限保护的 copyin/copyinstr，并对 VMA fault-in。
- `kernel/fs/file.c`：普通顺序 I/O及不改变 `file->off` 的 mmap 指定 offset I/O。

## 目标目录与文件作用

- `kernel/include/mmap.h`：file/anon/kbuf VMA/object 类型、fault access 和公共
  mmap VM 接口。
- `kernel/vm/mmap.c`：集中管理地址分配、backing fault、unmap、fork、destroy。
- `kernel/vm/kbuf.c`：管理 page-backed kbuf 的页面集合和引用。
- `kernel/vm/mmap_file.c`：按文件身份与页 offset 管理 SHARED mmap 文件页缓存。
- `kernel/devsw/kbufdev.c`：提供跨平台 kbuf mmap 测试设备，不操作用户页表。
- `kernel/syscall/sysmmap.c`：若 syscall 入口继续增长时再独立；Step 1 暂保留在 `sysfile.c` 以避免无必要搬迁。
- `kernel/fs/file.c`：提供显式 offset 的 mmap 文件 I/O。

## 依赖方向

```text
syscall / trap / proc / copy paths
                |
                v
             mmap VM
             /   |   \
          file  anon  kbuf
```

- 禁止 mmap backing 层反向依赖 trap、syscall 或 proc 流程。
- 禁止 FAT32 直接查找或修改进程 VMA。
- 禁止设备驱动自行安装用户 PTE。

## 当前结构约束

- `p->sz` 只覆盖普通低地址用户内存。
- `MMAP_TOP` 必须避开 trapframe、trampoline 和进程内核页表中的 kernel stack。
- 每个驻留 VMA 页必须在用户页表和进程内核页表中同步出现或消失。
- VMA 只描述虚拟区间，底层资源通过 object 持有。
- 解除映射先验证完整操作可执行，再修改 PTE 和 VMA 元数据。

## 新增洞察

- 当前 `uvmcopy` 只遍历 `[0, p->sz)`，高地址 VMA 必须由独立 `vma_fork` 处理。
- 当前进程内核页表在固定高地址映射 kernel stack，因此 mmap 上界不能简单使用 `TRAPFRAME`。
- `filewrite` 使用用户地址并推进 `file->off`，不能用于 mmap 页写回。
- RISC-V 不支持有效的 write-only 叶 PTE；当前 ABI 拒绝
  `PROT_WRITE` 但没有 `PROT_READ` 的组合，以保持权限判定一致。
- Step 1 只共享 fork 时已经驻留的 SHARED 文件页；两个进程之后分别 fault
  的同一文件页仍是不同物理页，必须由 Step 5 page cache 解决。

## Step 1 后确认

- VMA 元数据只由 `kernel/vm/mmap.c` 修改。
- mmap object 引用以 VMA 为单位；拆分和 fork 增加引用，删除 VMA 减少引用。
- `MMAP_TOP` 取 `KSTACK(0)`，VMA end 为 exclusive，不覆盖进程内核 stack。
- unmap 先完成所有写回检查，再修改 PTE/VMA；中间拆分先预留槽。
- copy 路径不再用 `p->sz` 排除高地址 VMA，并在访问物理页前校验 PTE 权限。

## Step 2 后确认

- `VMA_ANON | MAP_PRIVATE` 的 object 只管理 VMA 生命周期，不保存物理页数组。
- 匿名 PRIVATE 页的唯一所有权信息仍是 PTE 与现有页面引用计数。
- `vm_fault` 先统一分配清零页，再仅对 `VMA_FILE` 执行 backing read。
- syscall 以 `MAP_ANONYMOUS` 决定 fd 是否必须解析；匿名 ABI 固定
  `fd == -1`、`offset == 0`。
- mmap object 析构按 type 释放 backing，匿名 object 不触碰文件层。

## Step 3 后确认

- `VMA_ANON | MAP_SHARED` 的 object 持有独立 anonymous object；多个
  fork/split VMA 通过 mmap object 引用共享它。
- anonymous object 使用按驻留页创建的稀疏页槽，VMA 长度不会转化为预分配
  元数据开销。
- anonymous object 为页持有基础物理引用，实际安装的每个 PTE 再持有一个映射
  引用；删除单个映射不会使页槽悬空。
- shared fault 的页索引由 `vma->offset + page - vma->start` 计算，头部裁剪和
  VMA 拆分后仍定位到同一 backing 页。
- 首次 fault 的候选页在 object 锁外分配，并在锁内二次查找后发布；锁内不调用
  页面或堆分配器。
- PRIVATE anonymous 仍以进程 PTE/COW 管理页面，不依赖 anonymous object 页槽。

## Step 4 后确认

- 设备 mmap 回调只接收 offset/length/prot/flags 并返回借用的 kbuf；设备层
  不依赖 proc、VMA、fault 或 PTE。
- kbuf 在创建时分配 page-backed 页面并持有基础引用；fd 和 mmap object 分别
  持有 kbuf 引用，每个驻留 PTE 再持有页面映射引用。
- mmap object 在 syscall 返回前取得独立 kbuf 引用，因此 close fd 后 fault、
  fork 和既有映射访问仍安全。
- `VMA_KBUF` 使用 VMA offset 计算页索引，split/头部裁剪后与其他 backing
  保持同一索引规则。
- kbuf 页面仍由通用双页表路径安装/删除；设备驱动无法绕过权限校验或
  `kpagetable` 同步。
- 当前 kbuf 是非连续 page-backed 内存；连续 DMA/CMA 和 MMIO 映射未纳入本轮。

## Step 5 后确认

- SHARED 文件页缓存独立于 FAT32 buffer cache，键为 VMA object 持有期间稳定的
  `file->ep` 与页对齐文件 offset。
- LOADING/WRITING/EVICTING 状态在 cache spinlock 下发布，但页面分配、文件读取
  和写回都在锁外完成；竞争者以缓存项地址 sleep 并在唤醒后重新查找键。
- cache entry 持有一个页面基础引用，`mappings` 与实际 SHARED FILE PTE 一一
  对应；fault、fork、unmap 和失败回滚同时维护 mappings 与物理引用。
- 最后 mappings 进入 EVICTING 后仍保留缓存键，dirty 写回完成才删除，防止新
  fault 在写回窗口读取旧文件内容。
- `dirty_length` 保守记录所有可写映射允许触及的最大有效前缀；显式 unmap/exit
  写回和最后引用写回均不越过 VMA valid_end。
- PRIVATE 文件映射不进入缓存，仍由独立物理页、COW 和“不写回”规则管理。
- mmap page cache 只协调 mmap 路径；普通 read/write/truncate 的并发一致性不在
  当前核心子集内。
