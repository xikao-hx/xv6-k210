# Step 1 文件 mmap 基础代码设计

## 当前功能

- 功能名称：文件 mmap 基础与 VM 分层
- 所属实施步骤：Step 1
- 关联需求：文件 PRIVATE/SHARED、独立地址区、统一 fault、部分 unmap、fork/exec/exit

## 成功标准

- 行为：短末页、非零 offset、严格权限、close fd、部分 unmap 和文件写回正确。
- 结构：VMA 修改集中在 mmap VM，syscall/trap/proc 不实现 backing 细节。
- 验证：QEMU/K210 构建通过，QEMU mmap 测试通过。

## 现有代码观察

- `sysfile.c` 同时承担 mmap syscall、文件 fault、VMA 查找和 munmap。
- `proc.c` 的 fork/exit 直接复制或释放 `struct file *`。
- `uvmcopy` 只遍历 `p->sz`，无法复制独立高地址 VMA 页。
- `upg2ukpg` 可复用来同步已安装 PTE，但 unmap 需要成对清除两份页表。
- `eread/ewrite` 已支持显式 offset 和 kernel/user 地址标记。

## 修改文件

- `kernel/include/mmap.h`：新增 VMA/object 和公共接口。
- `kernel/vm/mmap.c`：集中实现 Step 1 文件映射行为。
- `kernel/include/proc.h`：进程持有新 VMA 结构。
- `kernel/include/file.h`、`kernel/fs/file.c`：显式 offset mmap I/O。
- `kernel/syscall/sysfile.c`：只解析 mmap/munmap 参数并调用 VM。
- `kernel/trap/trap.c`：fault 原因转为 access。
- `kernel/proc/proc.c`、`kernel/proc/exec.c`：调用 fork/destroy 接口并检查 heap 冲突。
- `kernel/vm/vm.c`、`kernel/vm/vmcopyin.c`：为合法 VMA 提供 copy fault-in。
- `Makefile`：编译新增 mmap VM 文件及测试程序。
- `testcase/mmaptest.c` 或等价新测试：补齐 Step 1 验收场景。

## 禁止修改范围

- K210 外设驱动：Step 1 没有设备 mmap。
- buffer cache 与 FAT32 数据结构：只增加窄文件 I/O 包装。
- anonymous/kbuf/page cache 实现：属于后续步骤。

## 职责划分

- syscall：ABI 参数获取，不选择地址、不改 VMA。
- mmap VM：校验映射语义、分配地址、管理 VMA/object/PTE。
- file：执行给定 offset 的文件内容 I/O，不管理 VMA。
- trap/copy：只提供 access 类型和访问入口。
- proc/exec：只触发生命周期事件。

## 依赖方向

- 允许：`sysfile/trap/proc/vmcopy -> mmap VM -> file/FAT32`。
- 禁止：`file/FAT32 -> mmap VM -> proc 流程`。
- 禁止 trap 直接读 VMA 字段。

## 数据流与状态流

1. syscall 取得 addr/length/prot/flags/file/offset。
2. `vma_map_file` 校验并创建 file object，向下分配 VMA。
3. fault/copy 调用 `vm_fault`，按 VMA offset 读入清零页。
4. mmap VM 同步安装用户与内核 PTE。
5. `vma_unmap` 按驻留页写回/释放并裁剪或拆分 VMA。
6. fork/exec/exit 通过 object 引用维持或销毁资源。

## 接口设计

- `uint64 vma_map_file(struct proc *, uint64, uint64, int, int, struct file *, uint64)`
- `int vma_unmap(struct proc *, uint64, uint64)`
- `int vm_fault(struct proc *, uint64, int)`
- `int vma_fork(struct proc *, struct proc *)`
- `void vma_destroy_all(struct proc *)`
- `int vma_range_valid(struct proc *, uint64, uint64, int)`
- `int fileread_at(struct file *, uint64, uint64, int)`
- `int filewrite_at(struct file *, uint64, uint64, int)`

## 方案取舍

- 采用：Step 1 就建立 object 抽象，因为 fd 提前关闭和 fork 生命周期已经需要独立引用。
- 采用：高地址向下 first-fit，固定 VMA 数组。
- 不采用：Step 1 共享文件全局 page cache；其并发和 dirty 生命周期单独放在 Step 5。
- 不采用：通用 `uvmunmap(..., do_free)` 直接处理所有 backing；由 mmap VM 决定引用语义。

## 风险与约束

- 最容易写脏：fork 失败回滚、VMA 中间拆分、双页表先后顺序、exec 旧页表清理。
- 避免方式：先预留槽、逐页只处理有效 PTE、把 object/PTE 操作集中在窄 helper。
- 兼容性：现有低地址 lazy allocation 和 COW 行为必须保持。
- 降级：Step 1 的不同进程独立 fault 尚不保证同一文件页即时共享，留到 Step 5。

## 结构自查清单

- 只修改 Step 1 必需文件：是；未触碰外设、匿名对象或 page cache。
- 复用现有页面引用计数/COW/FAT32 I/O：是。
- 避免重复 VMA 查找和写回逻辑：由 mmap VM 集中。
- 避免越层调用：是；syscall/trap/proc 只调用 mmap 公共接口。
- 未提前实现匿名、kbuf、全局 page cache：是。

## 验证方式

```bash
make build platform=qemu
make fs
make build platform=k210
git diff --check
```

- QEMU 预期：`mmaptest: all tests succeeded`，新增 Step 1 用例全部通过。

## 实施结果

- 行为成功标准：已通过 QEMU 功能测试。
- 结构成功标准：VMA 状态和 backing 生命周期已集中到 mmap VM。
- 验证成功标准：QEMU/K210 构建、QEMU `mmaptest`、`git diff --check` 均通过。
- 当前限制：不同进程在 fork 后分别首次 fault 的 SHARED 文件页尚不即时共享，
  符合 Step 1 边界，留到 Step 5。
- 下一步门禁：等待用户明确确认 Step 1 测试通过后，重新阅读 Memory Bank 并
  为 Step 2 私有匿名 mmap 更新本代码设计。
