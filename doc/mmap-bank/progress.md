# mmap 重构进度记录

## 当前状态

- 开发分支：`refactor/mmap-core-subset`，基于用户请求时的当前分支创建。
- 已阅读 `doc/重构文档/mmap重构方案.md` 并完成现有 mmap 路径审计。
- 已初始化 PRD、设计、技术栈、实施计划、架构、代码设计和开发规则。
- Step 1 至 Step 5 已全部完成、验证并独立提交。
- mmap 核心子集实现已收口，当前进入最终审计状态。

## Step 1：文件 mmap 基础与 VM 分层

- 完成时间：2026-07-24
- 状态：已完成并由用户确认。
- 提交：`4623c00 mmap: refactor file-backed mappings`
- 修改文件：
  - 新增 `kernel/include/mmap.h`、`kernel/vm/mmap.c`。
  - 修改 `kernel/include/proc.h`、`kernel/include/file.h`。
  - 修改 `kernel/syscall/sysfile.c`、`kernel/fs/file.c`。
  - 修改 `kernel/trap/trap.c`、`kernel/proc/proc.c`、`kernel/proc/exec.c`。
  - 修改 `kernel/vm/vm.c`、`kernel/vm/vmcopyin.c`、`Makefile`。
  - 扩展 `testcase/mmaptest.c`。
- 完成内容：
  - mmap 从 `MMAP_TOP` 向下分配，不再修改 `p->sz`；`growproc` 检查 VMA 下界。
  - VMA 与 file-backed mmap object 分离，fd 关闭后映射继续持有文件。
  - trap 和内核 copy 路径统一调用 `vm_fault`，严格按访问类型检查权限。
  - 文件页先清零并按显式 offset 读取，支持非零 offset 和文件短末页。
  - `munmap` 支持头尾裁剪、中间拆分、跨 VMA 和未驻留页，并同步双页表。
  - SHARED 驻留页按明确 offset 写回；PRIVATE 页不写回。
  - fork 对 PRIVATE 驻留页使用 COW，对 SHARED 驻留页保持物理页共享。
  - exec、exit 和 fork 失败统一经 VMA 生命周期接口释放。
  - `mmaptest` 正式加入用户程序镜像。
- 测试方式：
  - `make build platform=qemu`
  - `make fs platform=qemu`
  - `make run platform=qemu` 后运行 `mmaptest`
  - `make build platform=k210`
  - `git diff --check`
- 测试结果：
  - QEMU mmap 原测试全部通过。
  - 新增 PRIVATE COW、SHARED fork、非零 offset、中间拆分、跨 VMA、
    copyin/copyout/copyinstr fault-in、权限拒绝和 exec 写回测试全部通过。
  - QEMU/K210 构建成功；仅有仓库已有的 RWX linker warning。
  - 旧 `mmap_handler/find_vma/vfile/vfd` 引用无残留。
- 下一步提醒：
  - Step 1 尚未提供不同进程独立 fault 的文件页即时共享；留到 Step 5 page cache。
  - Step 2 只增加 `MAP_PRIVATE | MAP_ANONYMOUS`，不得提前加入共享匿名页槽。

## Step 2：私有匿名 mmap

- 完成时间：2026-07-24
- 状态：代码和自动验证完成。
- 提交：`2ce5b47 mmap: add private anonymous mappings`
- 修改文件：
  - `kernel/include/fcntl.h`、`kernel/include/mmap.h`
  - `kernel/vm/mmap.c`、`kernel/syscall/sysfile.c`
  - `testcase/mmaptest.c`
  - `doc/mmap-bank/implementation-plan.md`、`progress.md`、
    `architecture.md`、`code-design.md`
- 完成内容：
  - 增加 `MAP_ANONYMOUS` 和 `VMA_ANON`。
  - 支持 `MAP_PRIVATE | MAP_ANONYMOUS`，要求 `fd == -1`、`offset == 0`。
  - 创建映射时只分配 VMA/object，不分配匿名数据页。
  - 首次 trap 或 copyout 访问时按页分配并清零。
  - fork 后已驻留页沿用 COW，未驻留页由父子独立分配。
  - object 析构按 backing type 处理，不再假定始终持有文件。
  - 本步明确拒绝 `MAP_SHARED | MAP_ANONYMOUS`。
- 测试结果：
  - QEMU 完整 `mmaptest` 通过，包含 16 MiB 懒分配、零页、copyout
    fault-in、PRIVATE fork COW 和非法参数测试。
  - Step 1 文件 mmap 全量回归通过。
  - `make build platform=qemu`、`make build platform=k210` 通过。
  - `git diff --check` 通过。
- 下一步提醒：
  - Step 3 需要在 anonymous object 内增加共享页槽；PRIVATE anonymous
    仍不依赖这些页槽。

## Step 3：共享匿名 mmap

- 完成时间：2026-07-24
- 状态：代码和自动验证完成。
- 提交：`e940690 mmap: add shared anonymous mappings`
- 修改文件：
  - `kernel/include/mmap.h`、`kernel/vm/mmap.c`
  - `testcase/mmaptest.c`
  - `doc/mmap-bank/progress.md`、`architecture.md`、`code-design.md`、
    `dev-rules.md`
- 完成内容：
  - 支持 `MAP_SHARED | MAP_ANONYMOUS`，沿用匿名映射的
    `fd == -1`、`offset == 0` ABI。
  - shared anonymous object 用稀疏页槽按需保存已驻留页。
  - object 为每个共享页持有基础引用，每个用户 PTE 单独持有映射引用。
  - 并发首次缺页采用锁外分配、锁内二次查找，避免在自旋锁内分配内存。
  - fork 前已驻留页和 fork 后任一方首次驻留页都映射同一物理页。
  - PRIVATE anonymous 路径保持独立零页/COW，不查询共享页槽。
- 测试结果：
  - QEMU 完整 `mmaptest` 通过，包含父进程先驻留、子进程先驻留、
    父进程后驻留、二次 fork 和子进程退出后的共享可见性。
  - Step 1/Step 2 mmap 测试全量回归通过。
  - `make build platform=qemu`、`make build platform=k210` 通过。
  - `git diff --check` 通过。
- 下一步提醒：
  - Step 4 增加 page-backed kbuf 和设备 mmap 回调，不允许设备驱动直接操作
    用户页表。

## Step 4：设备 page-backed kbuf mmap

- 完成时间：2026-07-24
- 状态：代码和自动验证完成。
- 提交：`9e969d0 mmap: add device kbuf mappings`
- 修改文件：
  - 新增 `kernel/include/kbuf.h`、`kernel/vm/kbuf.c`。
  - 新增 `kernel/include/kbufdev.h`、`kernel/devsw/kbufdev.c`。
  - 修改 `kernel/include/file.h`、`dev.h`、`mmap.h`。
  - 修改 `kernel/vm/mmap.c`、`kernel/syscall/sysfile.c`、`kernel/main.c`、
    `Makefile`。
  - 扩展 `testcase/mmaptest.c`。
  - 更新 `doc/mmap-bank/code-design.md`、`progress.md`、`architecture.md`、
    `dev-rules.md`。
- 完成内容：
  - 增加独立引用计数的 page-backed kbuf，kbuf 拥有页面基础引用。
  - `file_operations` 增加 mmap 回调，回调只返回 kbuf，不接触 VMA/PTE。
  - 增加 `VMA_KBUF` 和设备 mmap syscall 分派，fault 按 offset 取得 kbuf 页。
  - mmap object 持有独立 kbuf 引用，fd 关闭不终止映射。
  - fork 和 unmap 沿用每 PTE 物理引用，支持不同释放顺序。
  - 增加跨平台 `DEV_KBUF` 测试设备及驱动侧 fill/check ioctl。
  - 设备映射仅接受 SHARED，拒绝执行权限、非页对齐 offset、越界长度和
    fd 权限不匹配。
- 测试结果：
  - QEMU 完整 `mmaptest` 通过，包含驱动/用户双向一致性、fd close、
    fork 共享、子进程先 unmap 和非法参数测试。
  - Step 1 至 Step 3 mmap 测试全量回归通过。
  - `make build platform=qemu`、`make build platform=k210` 通过。
  - `git diff --check` 通过。
  - 当前环境未连接 K210 实机；K210 只完成交叉构建，page-backed 测试设备
    的平台无关行为已在 QEMU 验证。
- 下一步提醒：
  - Step 5 为共享文件映射增加按文件身份与页 offset 索引的 page cache；
    不得复用 buffer cache 内存。

## Step 5：共享文件 mmap page cache

- 完成时间：2026-07-24
- 状态：代码和自动验证完成。
- 提交：`a8c8be9 mmap: share file-backed pages`
- 修改文件：
  - 新增 `kernel/include/mmap_file.h`、`kernel/vm/mmap_file.c`。
  - 修改 `kernel/vm/mmap.c`、`kernel/main.c`、`Makefile`。
  - 扩展 `testcase/mmaptest.c`。
  - 更新 `doc/mmap-bank/code-design.md`、`progress.md`、`architecture.md`、
    `dev-rules.md`、`implementation-plan.md`。
- 完成内容：
  - 以稳定 `struct dirent *` 文件身份和页对齐 offset 作为缓存键。
  - 增加 LOADING/READY/WRITING/EVICTING 状态，竞争 fault 等待同一缓存项。
  - 页面分配和 FAT32 I/O 全部在 cache spinlock 外执行。
  - cache 持有页面基础引用，每个 SHARED FILE PTE 对应 mappings 和物理引用。
  - fork 在安装子 PTE 前 hold 缓存页，失败路径成对 put。
  - unmap/exit 同步双页表后归还缓存映射；最后映射完成 dirty 写回再删除键。
  - 可写映射以 VMA valid_end 维护 dirty_length，避免末页写回越界。
  - PRIVATE 文件 mmap 完全绕过缓存，继续使用独立页和 COW。
- 测试结果：
  - QEMU 完整 `mmaptest` 通过。
  - 子进程通过独立 `open + mmap` 先 fault，父进程在子映射仍驻留时后 fault，
    双向修改均立即互见。
  - 子进程先 unmap/exit 后父映射继续有效，最后普通 read 验证写回。
  - 预热后连续 8 次 shared fault/unmap，sysinfo 未发现物理页线性减少。
  - Step 1 至 Step 4 mmap 测试全量回归通过。
  - `make build platform=qemu`、`make build platform=k210` 通过。
  - `git diff --check` 通过。
- 已知边界：
  - 这是 mmap 专用文件页缓存，不同步普通 read/write 或 truncate 的并发修改。
  - 未实现硬件 dirty bit、msync、mprotect、swap 或完整 Linux page cache。

## 最终结果

- 五个实施步骤均按顺序完成 QEMU 功能验证后独立提交。
- 最终支持：文件 PRIVATE/SHARED mmap、PRIVATE/SHARED anonymous mmap、
  设备 page-backed kbuf mmap、共享文件 mmap page cache。
- mmap 地址空间、统一 fault、copy fault-in、双页表、partial/cross-VMA munmap、
  fork/exec/exit 生命周期与显式 offset 写回均已纳入统一 VM 层。
- QEMU 完整 `mmaptest` 通过；QEMU/K210 交叉构建通过。
