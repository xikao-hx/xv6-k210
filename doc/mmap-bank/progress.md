# mmap 重构进度记录

## 当前状态

- 开发分支：`refactor/mmap-core-subset`，基于用户请求时的当前分支创建。
- 已阅读 `doc/重构文档/mmap重构方案.md` 并完成现有 mmap 路径审计。
- 已初始化 PRD、设计、技术栈、实施计划、架构、代码设计和开发规则。
- Step 1、Step 2 已完成；Step 2 等待独立提交。

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
