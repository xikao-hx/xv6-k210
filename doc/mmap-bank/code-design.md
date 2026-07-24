# Step 5 共享文件 mmap page cache 代码设计

## 当前功能

- 功能名称：共享文件映射页缓存
- 所属实施步骤：Step 5
- 关联需求：不同进程、不同 mmap object 对同一文件页修改立即互见并正确写回

## 成功标准

- 缓存键为稳定文件身份 `file->ep` 与页对齐文件 offset。
- 不同映射独立 fault 同一键时得到同一 PA，并发首次 fault 只发布一个缓存页。
- SHARED 可写页在 unmap/exit/最后映射释放时写回正确范围。
- fork、fault 回滚、部分 unmap 和最后引用回收时映射计数与物理引用严格配对。
- PRIVATE 文件映射继续使用独立页面与 COW，不进入共享缓存。
- QEMU 完整 mmaptest 和 K210 构建通过。

## 现有代码观察

- FAT32 entry cache 对同一路径返回同一 `struct dirent`，且 mmap object 的
  `filedup` 保证 VMA 存活期间 entry 身份稳定。
- 当前 SHARED 文件页只在 fork 复制已驻留 PTE 时共享；独立 fault 会重复读页。
- `vma_writeback_page` 已按 VMA valid_end 和显式 offset 计算写回范围。
- 页面分配器已有物理引用计数，但不了解文件键、dirty 或缓存映射数量。

## 修改文件

- `kernel/include/mmap_file.h`、`kernel/vm/mmap_file.c`：全局 mmap 文件页缓存。
- `kernel/vm/mmap.c`：SHARED 文件 fault、writeback、unmap、fork 接入缓存。
- `kernel/main.c`、`Makefile`：缓存初始化和构建。
- `testcase/mmaptest.c`：跨进程独立映射即时可见、写回和重复回收测试。
- `doc/mmap-bank/*`：最终进度、架构和长期约束。

## 禁止修改范围

- FAT32 buffer cache 数据结构和 `struct buf::data`。
- PRIVATE 文件映射语义。
- 通用文件 read/write 的 `file->off`。
- swap、msync、mprotect、MAP_FIXED 或完整 Linux page cache。

## 职责划分

- mmap file cache：按文件键拥有共享 PA、状态、映射计数和 dirty 有效长度。
- VMA：提供文件 offset、valid_end、flags/prot，决定是否走共享缓存。
- mmap object：保持 `struct file/dirent` 身份在 VMA 生命周期内稳定。
- PTE：每个实际映射持有一个物理引用。
- FAT32：只提供显式 offset I/O，不查询缓存或进程 VM。

## 缓存项状态

```text
LOADING -> READY <-> WRITING
             |
             v
          EVICTING -> removed
```

- `LOADING`：首次 fault 已发布占位项，文件读取在锁外进行；竞争者 sleep。
- `READY`：可增加映射计数并取得同一 PA。
- `WRITING`：显式写回在锁外进行；新 fault/并发写回等待完成。
- `EVICTING`：最后映射正在锁外写回；完成后删除并唤醒等待者重新查找。

状态转换只在 cache spinlock 下发生，页面分配和文件 I/O 不持有 spinlock。

## 数据与引用模型

```text
cache entry(file identity, page offset)
  -> PA [1 个 cache 基础引用]
       + 每个实际 PTE 1 个物理引用
  -> mappings [与实际 SHARED FILE PTE 数量一致]
  -> dirty_length [本页允许写回的最大有效前缀]
```

1. 首次 fault 创建候选节点和清零页，发布 LOADING 后读取完整文件页。
2. READY 发布时增加 mappings 与 PTE 物理引用。
3. 后续 fault/独立 mmap 按键命中同一 PA，并分别增加 mappings/PTE 引用。
4. fork 在安装子 PTE 前取得同键、同 PA 的缓存映射引用；失败时成对归还。
5. unmap 删除双页表 PTE 后归还缓存映射/PTE 引用。
6. mappings 归零时进入 EVICTING，按 dirty_length 写回后释放 cache 基础引用。

## 接口设计

- `mmap_file_cache_init()`
- `mmap_file_page_get(file, offset, dirty_length)`：fault 获取映射引用与 PA。
- `mmap_file_page_hold(file, offset, pa, dirty_length)`：fork 为既有 PA 增加映射。
- `mmap_file_page_writeback(file, offset, pa, length)`：显式同步缓存 PA。
- `mmap_file_page_put(file, offset, pa)`：归还一个 PTE/映射引用，必要时最后写回。

`dirty_length == 0` 表示只读映射；可写映射使用 VMA valid_end 计算本页有效前缀。

## 并发与锁顺序

- cache spinlock 只保护链表、状态、mappings 和 dirty_length。
- `kalloc/kmalloc/fileread_at/filewrite_at` 全部在 cache spinlock 外执行。
- 等待状态转换使用 `sleep(entry, &cache.lock)`，完成后 `wakeup(entry)`。
- 缓存项在唤醒等待者并从链表删除后才释放；等待者只以地址作 channel，
  醒来后重新按键查找。
- FAT32 不获取 cache lock，避免反向依赖。

## 写回策略

- 可写 SHARED 页首次映射时保守标记 dirty_length；无需依赖硬件 dirty bit。
- `munmap/exit` 继续先执行可失败的显式写回，再删除 PTE/VMA。
- 最后 mappings 释放时再次写回 dirty_length，覆盖其他仍存映射在先前写回后
  的修改。
- PRIVATE 页从不进入缓存，也不写回。

## 测试设计

- 父进程建立未驻留映射，子进程用独立 `open + mmap` 首次 fault 并写入。
- 子进程保持映射期间通知父进程 fault；父进程必须立即看到修改。
- 父进程修改后通知子进程，子进程必须在退出前立即看到。
- 子进程先 unmap/exit 后父进程继续访问，最后 unmap 后普通 read 验证写回。
- 预热后重复 fault/unmap，使用 sysinfo 检查物理页没有随循环线性减少。

## 风险与避免方式

- 先删除缓存项再写回导致新 fault 读旧数据：EVICTING 项保留键，等待写回完成。
- 两个首次 fault 重复发布：LOADING 占位与等待机制保证单一发布。
- fork 安装 PTE 失败：先 hold，失败立即 put；成功后由子 VMA 生命周期管理。
- VMA partial unmap 后 offset 漂移：所有键继续由维护后的 VMA offset 计算。
- 最末页错误扩展文件：dirty_length 使用 VMA valid_end，仅写有效前缀。
- 缓存永久持页：mappings 归零即回收节点和基础页引用，不以 VMA 是否仍存在为准。

## 结构自查清单

- PRIVATE 路径是否完全绕过缓存：已确认并由 PRIVATE/COW 回归覆盖。
- mappings 是否与每个 PTE 创建/删除成对：已审计 fault、fork、失败和 unmap。
- I/O 是否全部在 cache spinlock 外：已确认，状态发布后释放锁再 I/O。
- 最后引用是否在写回完成后删除键：已确认，EVICTING 保留到 I/O 完成。

## 验证方式

```bash
make build platform=qemu
make run platform=qemu
# shell 中运行 mmaptest
make build platform=k210
git diff --check
```

## 实施结果

- `mmap_file_page_get` 通过 LOADING 占位保证同键首次 fault 单一发布，完整文件页
  先清零再读取。
- `mmap_file_page_hold/put` 将 fork 和 unmap 纳入缓存 mappings 与物理页引用。
- `mmap_file_page_writeback` 使用 WRITING 状态串行化同一缓存项的显式写回。
- 最后 put 进入 EVICTING，写回 dirty_length 后才删除缓存项并释放基础页引用。
- 跨进程独立 open/mmap 测试确认同时驻留时双向即时可见，普通 read 确认最终写回。
- 重复 fault/unmap 的 sysinfo 检查未发现页面随迭代线性泄漏。
- QEMU 完整 `mmaptest`、QEMU/K210 构建和 `git diff --check` 均通过。
