# Step 4 设备 page-backed kbuf mmap 代码设计

## 当前功能

- 功能名称：设备 kbuf mmap
- 所属实施步骤：Step 4
- 关联需求：用户态与设备驱动访问同一组 page-backed 物理页

## 成功标准

- 设备 `file_operations` 可用窄 mmap 回调返回 kbuf，不接触 proc/VMA/PTE。
- mmap VM 按 kbuf offset 缺页映射，并同步用户页表和进程内核页表。
- fd close、fork、munmap/exit 的任意正常顺序都不会提前释放或泄漏 kbuf 页面。
- 长度、offset、权限和 flags 越界均拒绝。
- QEMU 完整 mmaptest 和 K210 构建通过。

## 现有代码观察

- `file_operations` 已承载 open/read/write/ioctl/close，适合增加设备 mmap 回调。
- mmap object 已处理跨 fork、VMA split 和 fd close 的 backing 生命周期。
- shared anonymous 已验证“object 基础页引用 + 每个 PTE 映射引用”模型。
- 设备表和 `dev()` 在 QEMU/K210 共用，可增加无硬件依赖的测试 kbuf 设备。

## 修改文件

- `kernel/include/kbuf.h`、`kernel/vm/kbuf.c`：kbuf object、页集合和引用接口。
- `kernel/include/file.h`：设备 mmap 回调。
- `kernel/include/mmap.h`、`kernel/vm/mmap.c`：`VMA_KBUF`、设备映射与 fault。
- `kernel/include/dev.h`、`kernel/include/kbufdev.h`、
  `kernel/devsw/kbufdev.c`：跨平台测试设备及 ioctl ABI。
- `kernel/main.c`、`Makefile`：注册和构建。
- `kernel/syscall/sysfile.c`：将非匿名设备 fd 分派到设备 mmap 入口。
- `testcase/mmaptest.c`：参数、共享、一致性和生命周期测试。
- `doc/mmap-bank/*`：同步进度、架构和长期规则。

## 禁止修改范围

- 映射 buffer cache 的 `struct buf::data`。
- 共享文件 mmap page cache：Step 5。
- 连续 DMA/CMA 分配器和设备物理 MMIO 直映。
- 设备驱动直接安装、删除或查询用户 PTE。

## 职责划分

- `kbuf`：拥有固定 page-backed 页面集合及基础物理引用，管理自身引用计数。
- 设备 fd：open 时持有一个 kbuf 引用，close 时释放。
- 设备 mmap 回调：验证设备内部 offset/length/flags，返回仍由 fd 持有的 kbuf。
- mmap object：取得独立 kbuf 引用，使 fd close 后映射继续有效。
- PTE：每个实际映射持有一个页面引用。
- mmap VM：地址选择、VMA、fault、fork、unmap 和双页表同步。

## 依赖方向

```text
sys_mmap -> mmap VM -> file_operations.mmap -> device
                    \-> kbuf page/ref API
```

- 允许：mmap VM 调用设备回调取得 kbuf，并调用 kbuf 页接口。
- 允许：设备通过 kbuf 内核地址接口访问自己持有的页面。
- 禁止：kbuf/设备依赖 proc、trap、syscall 或页表实现。

## 数据与引用模型

```text
device fd ref ----\
                   -> kbuf -> page list [每页 1 个 kbuf 基础引用]
mmap object ref --/                        + 每个 PTE 1 个映射引用
```

1. 测试设备 open 创建两页 kbuf，fd 持有初始引用。
2. mmap object 对回调返回的 kbuf 增加独立引用。
3. kbuf fault 按 `(vma.offset + page_delta) / PGSIZE` 取页并增加 PTE 引用。
4. unmap/exit 删除 PTE，通用页面引用计数减少映射引用。
5. fd 和最后一个 mmap object 都释放后，kbuf 释放每页基础引用。

## 接口设计

- `struct kbuf *kbuf_create(uint64 size)`
- `void kbuf_get(struct kbuf *)` / `void kbuf_put(struct kbuf *)`
- `uint64 kbuf_size(struct kbuf *)`
- `void *kbuf_page_get(struct kbuf *, uint64 index)`：返回带 PTE 引用的页。
- `void *kbuf_page_address(struct kbuf *, uint64 index)`：驱动持有 kbuf 时借用页地址。
- `file_operations.mmap(file, offset, length, prot, flags)`：返回借用的 kbuf。
- `vma_map_device(...)`：校验 fd 权限、调用回调、建立 `VMA_KBUF`。

## 测试设备

- 稳定 major：`DEV_KBUF`，minor 仅接受 0，QEMU/K210 都注册。
- 每次 open 创建独立两页 kbuf，避免不同 fd 意外共享。
- ioctl：
  - `KBUF_IOCTL_FILL`：驱动侧按 offset/length 写入指定字节。
  - `KBUF_IOCTL_CHECK`：驱动侧检查用户映射写入的内容。
- mmap 仅接受 `MAP_SHARED`，offset 页对齐且映射有效范围不超过 kbuf。

## 风险与避免方式

- fd close 后回调私有数据失效：mmap object 在 syscall 返回前取得独立 kbuf 引用。
- 映射失败泄漏引用：object 创建失败或 VMA 插入失败不保留 kbuf 引用。
- fork/unmap 提前释放：沿用每个 PTE 的页面引用，kbuf 始终保留基础引用。
- 非整页有效末尾：VMA `valid_end` 限定 ABI 长度，kbuf backing 校验使用原始 length。
- 驱动/用户并发访问：测试使用进程同步；kbuf 只保证生命周期，不提供数据一致性锁。

## 结构自查清单

- 回调是否完全不接触 proc/VMA/PTE：已确认，回调只验证参数并返回 kbuf。
- fd 引用、object 引用、PTE 引用是否区分：已确认并由 close/fork/unmap 测试覆盖。
- 是否拒绝 MAP_PRIVATE、越界和权限不匹配：已由 QEMU 测试确认。
- 是否未映射 buffer cache、未实现文件 page cache：已确认，留给 Step 5。

## 验证方式

```bash
make build platform=qemu
make run platform=qemu
# shell 中运行 mmaptest
make build platform=k210
git diff --check
```

## 实施结果

- `kernel/vm/kbuf.c` 以不可变页链表保存 page-backed 页面，引用归零时统一释放。
- `file_operations.mmap` 返回设备 fd 当前持有的借用 kbuf，mmap object 随即增加
  独立引用。
- `vm_fault` 对 `VMA_KBUF` 调用 `kbuf_page_get` 获取 PTE 引用，后续双页表、
  fork 和 unmap 复用统一流程。
- `DEV_KBUF` 的跨页 fill/check ioctl 验证驱动与用户映射确实访问同一物理页。
- QEMU 完整 `mmaptest`、QEMU/K210 构建和 `git diff --check` 均通过。
- K210 未执行实机运行测试；新增测试设备本身无平台专属硬件依赖。
