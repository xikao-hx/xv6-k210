# mmap 核心子集实施计划

## 总体原则

- 严格按步骤实施，每一步独立构建、测试和记录。
- 不提前实现后续 backing 类型或 page cache。
- 用户已明确授权连续完成；每步必须通过 QEMU 功能测试并独立提交后再进入下一步。
- 行为修复与职责迁移在同一步内保持最小闭环。

## Step 1：文件 mmap 基础与 VM 分层

### 目标

- mmap 区与 `p->sz` 分离。
- 建立 VMA/object、统一 fault、部分 unmap 和生命周期接口。
- 修复文件末页、非零 offset、权限、写回和双页表同步。
- fork 对文件 PRIVATE 使用 COW，对已驻留 SHARED 页保持共享。

### 涉及文件/模块

- `kernel/include/mmap.h`、`kernel/vm/mmap.c`
- `kernel/include/proc.h`、`kernel/include/file.h`
- `kernel/syscall/sysfile.c`、`kernel/fs/file.c`
- `kernel/trap/trap.c`、`kernel/proc/proc.c`、`kernel/proc/exec.c`
- `kernel/vm/vm.c`、`kernel/vm/vmcopyin.c`、`Makefile`
- `testcase/mmaptest.c` 或仓库内等价用户测试

### 结构验收点

- syscall 只解析参数，VMA 状态只由 mmap VM 层修改。
- trap 只转换 fault 类型。
- proc/exec 只调用 VMA 生命周期接口。
- FAT32 指定 offset I/O 不读取或修改 `file->off`。

### 测试

```bash
make build platform=qemu
make fs
make build platform=k210
```

- QEMU 运行 mmap 基础、末页、offset、权限、中间 unmap、fork、exec/exit 测试。

## Step 2：私有匿名 mmap

### 目标

- 增加 `MAP_ANONYMOUS | MAP_PRIVATE`。
- 建立时只保留 VMA，首次访问分配全零页。
- fork 后已驻留页 COW，copy 路径可 fault-in。

### 结构验收点

- 匿名 fault 不依赖 FAT32。
- 不引入共享匿名对象页表。

### 测试

- 大 VMA 建立时不增加页面，逐页访问后按需增长。
- 初始内容全零，fork 后父子写入互不影响。

## Step 3：共享匿名 mmap

### 目标

- 增加 anonymous object 页槽与引用管理。
- `MAP_ANONYMOUS | MAP_SHARED` 在 fork 后共享物理页。

### 结构验收点

- 页槽所有权只属于 anonymous object。
- 单进程退出不破坏其他映射。

### 测试

- fork 后父子修改立即互见，任一方退出后另一方继续访问。

## Step 4：设备 kbuf mmap

### 目标

- 增加 page-backed kbuf 和设备 mmap 回调。
- 用户态与驱动映射同一组物理页。

### 结构验收点

- 通用 VM 不直接理解设备内部状态。
- kbuf 引用和物理页释放由 kbuf 层统一管理。
- 不映射现有 buffer cache 内存。

### 测试

- 权限/offset/长度越界拒绝。
- close fd、fork、不同释放顺序和驱动/用户数据一致。
- QEMU 构建测试及 K210 设备实机验证。

## Step 5：共享文件 mmap page cache

### 目标

- 按“文件身份 + 页 offset”共享文件映射页。
- 支持多进程即时可见和 dirty 写回。

### 结构验收点

- page cache 生命周期不依赖任一 VMA。
- FAT32 不反向依赖进程 VM。

### 测试

- 两个进程/映射对同一文件页修改立即互见。
- unmap/exit/最后引用回收时写回正确且无页面泄漏。
