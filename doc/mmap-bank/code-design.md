# Step 2 私有匿名 mmap 代码设计

## 当前功能

- 功能名称：`MAP_PRIVATE | MAP_ANONYMOUS`
- 所属实施步骤：Step 2
- 关联需求：大块虚拟内存、零页懒分配、fork COW、内核 copy fault-in

## 成功标准

- 行为：创建匿名 VMA 时不分配数据页，首次读写页面得到全零内容。
- 行为：fork 后已驻留匿名页使用 COW，父子写入互不影响。
- 结构：匿名 fault 不调用文件层，不引入 Step 3 的共享页槽。
- 验证：QEMU 匿名映射测试与原文件 mmap 回归通过，K210 构建通过。

## 现有代码观察

- `vma_map_file` 已包含地址分配与 VMA 初始化，可抽出共用插入 helper。
- `vm_fault` 已先分配并清零页面，文件读取是唯一需要按 type 分流的步骤。
- `vma_fork` 的 PRIVATE COW 逻辑不依赖文件 backing，可直接复用。
- syscall 当前无条件通过 `argfd` 解析 fd，匿名映射需要接受 `fd == -1`。

## 修改文件

- `kernel/include/fcntl.h`：增加用户/内核共享的 `MAP_ANONYMOUS`。
- `kernel/include/mmap.h`：增加 `VMA_ANON` 和 `vma_map_anon`。
- `kernel/vm/mmap.c`：创建私有匿名 object、共用 VMA 插入及匿名 fault。
- `kernel/syscall/sysfile.c`：按 anonymous flag 决定是否解析文件。
- `testcase/mmaptest.c`：增加懒分配、零页、copy 和 fork COW 测试。
- `doc/mmap-bank/*`：记录设计、验证和架构变化。

## 禁止修改范围

- anonymous object 共享页槽：属于 Step 3。
- 设备 file operations/kbuf：属于 Step 4。
- 文件 mmap page cache：属于 Step 5。

## 职责划分与依赖方向

- syscall 只区分 ABI 参数组合并选择 `vma_map_file/vma_map_anon`。
- mmap VM 根据 `vma->type` 选择 file read 或匿名零页。
- PRIVATE fork 继续统一由 `vma_fork` 处理。
- 禁止匿名 fault 调用 `file.c/FAT32`。

## 数据流

1. 用户以 `fd=-1, offset=0` 请求 PRIVATE anonymous mapping。
2. mmap VM 创建无文件 backing 的 `VMA_ANON` object，仅插入 VMA。
3. trap/copy 首次访问调用 `vm_fault`，分配并保留清零页。
4. fork 对已驻留可写页设置 COW；未驻留页由父子以后独立分配。
5. munmap/exec/exit 沿用统一页面和 object 释放流程。

## 接口设计

- `uint64 vma_map_anon(struct proc *, uint64, uint64, int, int)`
- `MAP_ANONYMOUS = 0x20`
- anonymous ABI 限制：仅 `MAP_PRIVATE`，`fd == -1`，`offset == 0`。

## 方案取舍

- 采用：Step 2 创建轻量 `VMA_ANON` object，但不保存页面数组。
- 不采用：以全局零页实现第一次读；会引入额外只读/COW状态，当前内存规模下收益有限。
- 不采用：提前让 MAP_SHARED 复用 PRIVATE 页；无法表达缺页后跨进程共享。

## 风险与避免方式

- flags 组合解析容易破坏文件 mmap：分别校验 anonymous bit 和 sharing bit。
- object 析构不能无条件 `fileclose`：按 object type 释放。
- fault 分流必须发生在页面已清零之后、PTE 安装之前。

## 结构自查清单

- 只修改 Step 2 必需文件：是。
- 未引入共享匿名页槽：是。
- 文件 mmap 行为保持不变：QEMU 全量回归通过。
- 无新的反向依赖：匿名逻辑仅位于 mmap VM。

## 验证方式

```bash
make build platform=qemu
make fs platform=qemu
make run platform=qemu
# shell 中执行 mmaptest
make build platform=k210
git diff --check
```

## 实施结果

- 16 MiB PRIVATE anonymous VMA 在 QEMU 小内存配置下成功建立。
- 选定页面首次读取为零，写入及 copyout fault-in 正常。
- fork 后父子对已驻留和未驻留页面均保持 PRIVATE 隔离。
- QEMU `mmaptest`、QEMU/K210 构建和差异检查均通过。
