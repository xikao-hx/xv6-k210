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

- `kernel/include/mmap.h`：当前 file VMA/object 类型、fault access 和公共 mmap VM 接口。
- `kernel/vm/mmap.c`：当前集中管理地址分配、文件 fault-in、unmap、fork、destroy。
- `kernel/vm/kbuf.c`：Step 4 后管理 page-backed kbuf。
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
