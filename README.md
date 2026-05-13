# XV6-RISCV On K210
在 `K210` 开发板上运行 `xv6-riscv` 操作系统  
[English](./README.md) | [中文](./README_cn.md)   

```
(base) xikao@xikao-virtual-machine:~/xv6-k210-opensbi$ ./run.sh nographic

OpenSBI v0.9
   ____                    _____ ____ _____
  / __ \                  / ____|  _ \_   _|
 | |  | |_ __   ___ _ __ | (___ | |_) || |
 | |  | | '_ \ / _ \ '_ \ \___ \|  _ < | |
 | |__| | |_) |  __/ | | |____) | |_) || |_
  \____/| .__/ \___|_| |_|_____/|____/_____|
        | |
        |_|

Platform Name             : Generic
Platform Features         : timer,mfdeleg
Platform HART Count       : 2
Firmware Base             : 0x80000000
Firmware Size             : 112 KB
Runtime SBI Version       : 0.2

Domain0 Name              : root
Domain0 Boot HART         : 1
Domain0 HARTs             : 0*,1*
Domain0 Region00          : 0x0000000080000000-0x000000008001ffff ()
Domain0 Region01          : 0x0000000000000000-0xffffffffffffffff (R,W,X)
Domain0 Next Address      : 0x0000000080200000
Domain0 Next Arg1         : 0x0000000082200000
Domain0 Next Mode         : S-mode
Domain0 SysReset          : yes

Boot HART ID              : 1
Boot HART Domain          : root
Boot HART ISA             : rv64imafdcsu
Boot HART Features        : scounteren,mcounteren
Boot HART PMP Count       : 16
Boot HART PMP Granularity : 4
Boot HART PMP Address Bits: 54
Boot HART MHPM Count      : 0
Boot HART MHPM Count      : 0
Boot HART MIDELEG         : 0x0000000000000222
Boot HART MEDELEG         : 0x000000000000b109

xv6 kernel is booting

hart 0 starting
init: starting sh
$ 
```

<!-- ![run-k210](./img/xv6-k210_on_k210.gif)   -->

## 依赖
+ `k210` 开发板或者 `qemu-system-riscv64`
+ RISC-V GCC 编译链: [riscv-gnu-toolchain](https://github.com/riscv/riscv-gnu-toolchain.git)

## 下载
```bash
git clone https://github.com/xikao-hx/xv6-k210-opensbi.git
```

## <a id="title_qemu">在 qemu-system-riscv64 模拟器上运行</a>
首先，确保 `qemu-system-riscv64` 已经下载到您的机器上并且加到了环境变量中；  

```shell
./build.sh   # 编译内核和用户程序
./run.sh nographic   # 在 qemu 上运行 xv6
```

Ps: 按 `Ctrl + A` 然后 `X` 退出 `qemu`。     

## 关于 Shell
目前已经支持几个常用命令，如 `cd`，`ls`，`cat` 等。

此外，`shell` 支持下列快捷键：  
- Ctrl-H -- 退格  
- Ctrl-U -- 删除行  
- Ctrl-D -- 文件尾（EOF）  
- Ctrl-P -- 打印进程列表

## 进度
* ✅多核启动
* ❌修改文件系统格式为 FAT32
* ❌移植到k210开发板