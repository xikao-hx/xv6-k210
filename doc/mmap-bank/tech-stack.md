# mmap 技术栈与工程约束

## 选择结果

- 语言：现有 freestanding C 与 RISC-V Sv39 页表机制。
- 构建：GNU Make，`riscv64-unknown-elf-` 工具链。
- 平台：QEMU 与 K210，共享 VM/FAT32 核心实现。
- 内存：现有 `kalloc_page/kfree_page` 页面分配及引用计数。
- 并发：短状态临界区使用现有 `spinlock`。
- 文件系统：现有 FAT32 `eread/ewrite`，增加不改变 `file->off` 的窄接口。
- 资源表：固定 `NVMA` VMA 数组；后续 object 内页槽按阶段设计。

## 选择理由

- 复用现有 COW、页分配、双页表和 FAT32 能力，不引入新运行时。
- 固定 VMA 表符合当前 xv6 资源上限，失败路径易于验证。
- object 分层将虚拟区间与 backing 生命周期分开，能承载三类映射。

## 备选方案与取舍

- 继续用 `p->sz` 分配 mmap：会与 heap/lazy allocation 冲突，不采用。
- mmap 继续放在 `sysfile.c`：会让 syscall 层承担 VM 生命周期，不采用。
- 直接映射 `struct buf::data`：暴露锁和内核元数据，不采用。
- 阶段一直接实现共享 file page cache：扩大单步风险，留到阶段五。
- 为 mmap 单独复制一套 fault 逻辑：会与 copy/trap 分叉，不采用。

## 工程约束

- 内核禁止浮点、标准库和动态外部依赖。
- 缩进 2 空格，函数定义左大括号另起一行。
- 平台差异只在确有硬件原因时使用 `QEMU` 宏。
- 每步只加入该步有调用方的数据结构和行为。
- 页面引用、PTE 安装和 object 引用的失败回滚必须成对。
- 每步至少构建 QEMU；影响公共 VM 时同时构建 K210。
