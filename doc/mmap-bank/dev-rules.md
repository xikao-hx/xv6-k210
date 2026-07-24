# mmap 长期编码约束

## 使用方式

- 每个 mmap 实施步骤编码前必须阅读本目录的设计、技术栈、计划、进度、架构和本文档。
- 每步完成后更新 progress、architecture 和 code-design。

## 通用原则

- 手术刀式修改，不提前实现后续 backing 类型。
- 内核禁止浮点、标准库依赖和不可控动态资源。
- 优先复用页面分配、引用计数、COW 和 FAT32 helper。
- 新增公共 helper 必须有明确的跨模块调用方。

## mmap 分层

- syscall 只解析 ABI，不能直接修改 VMA。
- trap 只将异常原因转换成 fault access。
- proc/exec 只调用 mmap 生命周期接口。
- FAT32 和设备驱动不能查找或修改进程 VMA。
- VMA 只描述地址区间，backing 生命周期属于 mmap object。

## 页表和所有权

- mmap 地址不得计入 `p->sz`。
- 所有驻留 VMA 页必须同步维护 `pagetable` 和 `kpagetable`。
- 修改 PTE 所有权前必须明确 PRIVATE、SHARED、COW 或 object 页引用。
- 失败路径必须回滚新增的 object 引用、物理页引用和 PTE。
- unmap 拆分需要的 VMA 槽必须在修改页表前预留。
- object-backed 共享页必须区分 object 基础引用和各 PTE 映射引用；任何单个
  VMA 的 unmap/exit 都不能提前释放 object 所有的页面。
- 共享页首次创建不得在自旋锁内执行 `kalloc/kmalloc`；采用锁外分配和锁内
  二次查找，并完整释放竞争失败的候选对象。

## 文件映射

- mmap 文件 I/O 必须使用显式 offset，不能读取或修改 `file->off`。
- 文件页先清零，再读取实际文件范围。
- PRIVATE 页永不由 mmap 写回。
- SHARED 只写回已驻留页面与有效映射/文件范围的交集。

## 设备映射

- 设备 mmap 回调只能验证设备参数并返回受生命周期保护的 kbuf，不得访问
  proc、VMA 或用户/内核页表。
- 设备 fd、mmap object、物理页 PTE 引用必须分层；fd close 不得使既有映射失效。
- page-backed kbuf 由 kbuf 层释放基础页面引用，设备驱动和通用
  `vma_unmap` 不得绕过该所有权。
- 不得把 buffer cache 数据区直接暴露给用户映射；连续 DMA 与 MMIO 必须使用
  后续专用接口。

## 验证

- 公共 VM 改动至少构建 QEMU 和 K210。
- 缺页、fork、写回、exec 或设备行为必须运行对应功能测试。
- 每步执行 `git diff --check`，并记录无法自动化的实机风险。
