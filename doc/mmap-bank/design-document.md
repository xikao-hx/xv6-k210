# mmap 核心子集设计文档

## 需求理解

- 目标用户是需要文件映射、大块懒分配内存和设备零拷贝的 xv6 用户程序及驱动。
- 用户提供的重构方案是功能边界和分阶段顺序的事实来源。
- 首轮只实现 Linux 核心语义，不追求完整 ABI。
- 内核为 freestanding C，禁止浮点和标准库依赖。

## 已确认事项

- mmap 地址由内核选择，首轮不接受固定地址。
- mmap 区从高地址向下分配，与 `p->sz` 分离。
- 文件 offset 必须非负且页对齐。
- `MAP_PRIVATE | PROT_WRITE` 不要求文件可写；`MAP_SHARED | PROT_WRITE` 要求可写。
- fd 关闭不终止已有映射。
- 用户页表和进程内核页表必须同步。
- 共享文件 page cache 放在最后阶段，不混入文件基础修复。

## 功能设计

### VMA 与 mmap object

- VMA 只记录虚拟区间、用户有效末尾、offset、权限、flags 和 object。
- mmap object 统一持有 file、anonymous object 或 kbuf object，并独立引用计数。
- VMA 不保存 fd，避免映射生命周期依赖 descriptor 槽位。

### 地址分配

- `p->sz` 只表示 ELF、stack、heap 的低地址空间。
- mmap 从 `MMAP_TOP` 向下寻找不与现有 VMA 重叠的页对齐区间。
- `growproc` 在扩展 heap 前检查最低 VMA，禁止相交。

### 统一缺页

- trap 将 instruction/load/store fault 转换为 EXEC/READ/WRITE。
- `vm_fault(p, va, access)` 查找 VMA、校验 `PROT_*`，再委托 backing 类型取得页面。
- fault-in 后同时建立 `pagetable` 和 `kpagetable` 映射并刷新 TLB。
- `copyin/copyout/copyinstr` 在 PTE 缺失时对合法 VMA调用同一入口。

### 文件映射

- 页面先整体清零，再从 `vma.offset + page_delta` 读取与文件相交的内容。
- 文件读写使用显式 offset，不修改 `file->off`。
- PRIVATE 修改页不写回；SHARED 仅写回已驻留且需要覆盖的文件范围。
- 最终阶段以“文件身份 + 页 offset”建立共享 mmap page cache。

### 匿名映射

- PRIVATE 首次访问分配零页，fork 后使用现有物理页引用计数和 COW。
- SHARED 由 anonymous object 保存页槽，fork 后映射同一物理页。
- 无 swap，实际驻留上限仍由 `PHYSTOP` 决定。

### kbuf 映射

- 设备 file operations 提供 mmap 回调，并返回专用 kbuf object。
- kbuf 页由 object/驱动管理，通用 `uvmunmap` 不直接释放。
- 连续 DMA 内存使用独立分配接口，不改变匿名 mmap 分配策略。

### munmap 与生命周期

- 先完整校验区间及所需 VMA 槽，再修改页表/VMA，避免半完成。
- 支持删除、头尾裁剪、中间拆分和跨多个 VMA。
- 页面释放按 backing 类型处理，同时删除双页表 PTE。
- fork、exec、exit 只调用 mmap VM 层生命周期接口。

## 关键流程

1. syscall 层解析参数并创建 backing object。
2. mmap VM 层分配地址并插入 VMA。
3. trap/copy 路径调用统一 `vm_fault`。
4. backing 层取得、共享或分配物理页。
5. mmap VM 层安装双页表 PTE。
6. unmap/exec/exit 通过统一接口写回和释放。

## 数据与状态

- `vma_area`：单进程虚拟地址区间。
- `mmap_object`：跨 VMA/进程共享的 backing 生命周期。
- FILE page cache：最终阶段共享文件驻留页和 dirty 状态。
- anonymous object：共享匿名页槽。
- kbuf object：驱动引用、fd 引用、VMA 引用和物理页集合。

## 验收标准

- 参数、权限、文件、匿名、munmap、生命周期、kbuf 和双页表矩阵全部覆盖。
- mmap VM 不反向依赖 syscall、trap 或 proc 的流程细节。
- FAT32 和设备层不直接操作进程 VMA。
