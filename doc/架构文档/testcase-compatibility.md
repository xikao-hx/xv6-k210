# testcase 兼容性检查

## 目标

`testcase/` 存放来自 xv6 lab 风格的用户态测试程序。目标是后续可以把其中的 `.c` 文件复制到 `user/test/` 或 `user/app/` 后，通过现有用户程序编译规则编译。

## 已处理项

- 已将 `testcase/*.c` 中的旧引用：

  ```c
  #include "user/user.h"
  ```

  改为当前工程的用户头文件：

  ```c
  #include "user.h"
  ```

  不新增 `user/user.h` 包装头。

- 在 `kernel/include/fs.h` 中补充用户测试常用宏：

  ```c
  #define BSIZE 512
  #define DIRSIZ 14
  ```

  这样 `usertests.c`、`bigfile.c`、`mmaptest.c` 等包含 `fs.h` 后可以直接使用 `BSIZE/DIRSIZ`。

- 在 `user/libc/ulib.c` 中实现 `statistics(void *buf, int sz)`，内部通过 `DEV_STATS` 读取统计设备。这样 `bcachetest.c`、`kalloctest.c`、`stats.c` 不再需要额外链接 `testcase/statistics.c`。

## 编译检查结果

已使用当前交叉编译器逐个编译：

```sh
riscv64-unknown-elf-gcc ... -I. -Ikernel/include -Iuser/include -c testcase/*.c
```

结果：

- 22 个 `.c` 文件均可单独编译为 `.o`。
- 带 `main()` 的 21 个 `.c` 文件中，除 `statistics.c` 外均可链接为用户程序。
- `statistics.c` 没有 `main()`，现在仅作为历史 helper 保留；后续不需要把它加入 `UPROGS`。

## 仍需注意

- `xargstest.sh` 是 shell 脚本，不是 C 用户程序，不能加入 `UPROGS`。
- 部分测试虽然能编译，但运行成本较高或依赖实验功能：
  - `bigfile.c` 会写大量文件数据。
  - `lazytests.c` 会申请大块虚拟内存。
  - `cowtest.c` 依赖 COW 行为。
  - `bcachetest.c`、`kalloctest.c` 依赖统计设备输出内容。
- 把测试文件复制到 `user/test/` 后，还需要在 `Makefile` 的 `UPROGS` 中加入对应 `$(UBUILD)/test/_name`。

## 推荐做法

新增用户测试时优先放到：

```text
user/test/<name>.c
```

然后在 `Makefile` 中加入：

```make
$(UBUILD)/test/_<name>\
```

如果测试文件来自 `testcase/`，当前目录内的 `.c` 已经改为 `#include "user.h"`。后续新增外部 xv6 lab 测试时，也应直接改 include，而不是新增包装头。
