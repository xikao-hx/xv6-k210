# Step 3 共享匿名 mmap 代码设计

## 当前功能

- 功能名称：`MAP_SHARED | MAP_ANONYMOUS`
- 所属实施步骤：Step 3
- 关联需求：fork 亲缘进程共享匿名页、任一进程退出后另一方继续访问

## 成功标准

- fork 后父子对已驻留和 fork 后首次驻留的共享页修改立即互见。
- 任一进程 unmap/exit 不释放仍被另一进程或 anonymous object 使用的页面。
- 页槽只属于 anonymous object，不进入 proc、trap 或 syscall。
- QEMU 完整 mmaptest 和 K210 构建通过。

## 现有代码观察

- mmap object 已跨 fork 和 VMA split 共享，适合作为 anonymous page 所有者。
- `vma->offset` 在头部裁剪和中间拆分时已调整，可作为 object 内页索引基址。
- `vma_fork` 对非 PRIVATE 驻留页已经增加物理引用并映射同一 PA。
- 缺失能力是 fork 后任一进程新 fault 时从 object 查找同一页面。

## 修改文件

- `kernel/include/mmap.h`：增加 anonymous object/page 声明与 object backing。
- `kernel/vm/mmap.c`：anonymous object 创建、稀疏页槽查找/发布/销毁和 shared fault。
- `testcase/mmaptest.c`：增加已驻留共享、fork 后首次 fault、退出生命周期测试。
- `doc/mmap-bank/*`：同步进度、架构和长期所有权规则。

## 禁止修改范围

- 设备 kbuf 与 file operations mmap 回调：Step 4。
- 共享文件 page cache：Step 5。
- 无亲缘进程的命名共享对象：非目标。

## 职责划分

- `anon_object`：拥有共享匿名物理页的基础引用和稀疏页槽。
- VMA：通过 object 和 offset 解释本进程地址对应的页索引。
- PTE：每个实际映射持有一个页面引用。
- `vm_fault`：取得带映射引用的 PA，安装 PTE；失败时归还映射引用。

## 依赖方向

- 允许：`vm_fault/vma lifecycle -> anon object -> kalloc/kmalloc`。
- 禁止：anon object 依赖 proc、syscall、trap 或 FAT32。
- PRIVATE anonymous 不查询共享页槽。

## 数据与引用模型

```text
mmap_object ref
  -> anon_object
       -> anon_page(index, pa)  [持有 1 个 object 页面引用]
                                  + 每个 PTE 各持有 1 个映射引用
```

1. shared fault 查找页槽；不存在则分配零页并以 object 引用发布。
2. fault 为当前 PTE 增加一个映射引用。
3. unmap/exit 删除 PTE 并减少映射引用。
4. 最后一个 VMA/object 引用释放时销毁页槽，减少 object 页面引用。

## 接口设计

- `struct anon_object { spinlock; struct anon_page *pages; }`
- `struct anon_page { uint64 index; uint64 pa; struct anon_page *next; }`
- 页索引：`(vma->offset + page - vma->start) / PGSIZE`
- 页槽使用稀疏链表，只为已驻留页面分配节点。

## 方案取舍

- 采用稀疏链表：大 VMA 仍保持懒分配，节点数量等于驻留页数。
- 不采用按 VMA 长度预分配指针数组：16 MiB 映射会产生大块元数据。
- 不让最后一个 PTE拥有页面：会在暂时无 PTE但 object 仍存在时丢失共享内容。

## 风险与避免方式

- 并发首次 fault 可能重复分配：锁外分配，锁内二次查找，输掉竞争的候选页完整释放。
- mappages 失败：anon getter 返回的映射引用必须归还，object 引用保留。
- split 后索引漂移：只使用已维护的 `vma->offset` 计算。
- 析构顺序：先解除所有 VMA PTE，再减少最后一个 object ref。

## 结构自查清单

- object 与 PTE 引用是否区分：已确认，页槽持有基础引用，每次安装 PTE 增加引用。
- PRIVATE 路径是否保持原实现：已由匿名 PRIVATE/COW 全量测试确认。
- 是否只为驻留页分配节点：已确认，创建 VMA/object 时不创建页节点。
- 是否未引入 file page cache/kbuf：已确认，留给 Step 4/Step 5。

## 验证方式

```bash
make build platform=qemu
make run platform=qemu
# shell 中运行 mmaptest
make build platform=k210
git diff --check
```

## 实施结果

- `mmap_object_create` 只在 shared anonymous 类型下创建 `anon_object`。
- `anon_page_get` 实现稀疏查找、锁外候选分配、锁内二次查找和映射引用获取。
- object 最后一个引用释放时遍历页槽，释放基础页引用和节点。
- `vm_fault` 对 shared anonymous 通过 object 页索引取页；PRIVATE anonymous
  和文件路径保持原行为。
- QEMU 完整 `mmaptest`、QEMU/K210 构建和 `git diff --check` 均通过。
