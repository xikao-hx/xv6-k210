# mmap Linux 风格核心子集 PRD

## 背景

- 现有 mmap 与 heap 共用 `p->sz`，会让 `sbrk`、lazy allocation、fork 和映射区互相混淆。
- VMA 只能持有文件，不能表达匿名对象和设备共享缓冲区。
- 缺页权限、文件末页、写回、部分 `munmap` 和双页表同步存在缺陷。
- 内核 `copyin/copyout/copyinstr` 无法访问尚未驻留但合法的 mmap 地址。

## 目标

- 提供 Linux 风格 mmap 核心子集，覆盖文件、匿名内存和设备共享缓冲区。
- mmap 区域与 heap 分离，页面按需驻留。
- fork、exec、exit、close fd 和部分 `munmap` 具有明确且安全的生命周期。
- 保持 VM、FAT32、syscall 和设备驱动之间的单向依赖。
- QEMU 与 K210 使用同一套 mmap 核心逻辑。

## 非目标

- 不实现 `MAP_FIXED`、`MAP_FIXED_NOREPLACE`。
- 不实现 `mprotect`、`msync`、`madvise`、`mlock`。
- 不实现 swap、huge page、NUMA、ASLR 和完整 Linux `SIGBUS` 语义。
- 不允许映射任意内核虚拟地址或 buffer cache 内嵌数据。
- 不提供无亲缘进程之间、没有共享 fd 或命名对象的匿名共享。

## 核心功能

- 文件 `MAP_PRIVATE`：按需读页、fork COW、修改不写回。
- 文件 `MAP_SHARED`：按需读页、脏页写回；最终支持同一文件页的多进程即时可见。
- 匿名 `MAP_PRIVATE`：零页懒分配、fork COW。
- 匿名 `MAP_SHARED`：fork 后共享 anonymous object 和物理页。
- 通过设备 fd 映射安全的 page-backed kbuf，实现用户态与驱动零拷贝。
- 完整部分 `munmap`、跨 VMA 解除映射及统一资源引用计数。
- trap 与内核拷贝路径共用 `vm_fault()`。

## 用户使用路径

1. 用户调用 `mmap(0, length, prot, flags, fd, offset)`。
2. 内核校验参数并在独立高地址 mmap 区预留 VMA。
3. 用户或内核首次访问页面时触发统一 fault-in。
4. fork 根据 PRIVATE/SHARED 语义复制或共享页面。
5. `munmap`、exec 或 exit 写回需要写回的页并释放映射引用。

## 验收标准

- 文件末页、非零页对齐 offset、close fd 后访问和权限错误行为正确。
- 建立大匿名 VMA 时不立即增加驻留页，首次访问得到全零页。
- 私有映射 fork 后写入互不影响，共享映射写入互相可见。
- VMA 可全部、头部、尾部、中间或跨区间解除映射。
- 用户页表与进程内核页表在 fault-in/unmap 后保持一致。
- 设备 kbuf 映射关闭 fd 后仍有效，最后一个引用释放后无泄漏。
- 每个阶段通过对应的 QEMU/K210 构建与功能测试。
