# XV6-RISCV On K210
在 K210 开发板上运行 xv6-riscv 操作系统
```
(base) xikao@xikao-virtual-machine:~/xv6_k210 $ make run platform=qemu
[rustsbi] RustSBI version 0.1.1
.______       __    __      _______.___________.  _______..______   __
|   _  \     |  |  |  |    /       |           | /       ||   _  \ |  |
|  |_)  |    |  |  |  |   |   (----`---|  |----`|   (----`|  |_)  ||  |
|      /     |  |  |  |    \   \       |  |      \   \    |   _  < |  |
|  |\  \----.|  `--'  |.----)   |      |  |  .----)   |   |  |_)  ||  |
| _| `._____| \______/ |_______/       |__|  |_______/    |______/ |__|

[rustsbi] Platform: QEMU (Version 0.1.0)
[rustsbi] misa: RV64ACDFIMSU
[rustsbi] mideleg: 0x222
[rustsbi] medeleg: 0xb1ab
[rustsbi-dtb] Hart count: cluster0 with 2 cores
[rustsbi] Kernel entry: 0x80200000
  (`-.            (`-.                            .-')       ('-.    _   .-')
 ( OO ).        _(OO  )_                        .(  OO)    _(  OO)  ( '.( OO )_ 
(_/.  \_)-. ,--(_/   ,. \  ,--.                (_)---\_)  (,------.  ,--.   ,--.) ,--. ,--.  
 \  `.'  /  \   \   /(__/ /  .'       .-')     '  .-.  '   |  .---'  |   `.'   |  |  | |  |   
  \     /\   \   \ /   / .  / -.    _(  OO)   ,|  | |  |   |  |      |         |  |  | | .-')
   \   \ |    \   '   /, | .-.  '  (,------. (_|  | |  |  (|  '--.   |  |'.'|  |  |  |_|( OO )
  .'    \_)    \     /__)' \  |  |  '------'   |  | |  |   |  .--'   |  |   |  |  |  | | `-' /
 /  .'.  \      \   /    \  `'  /              '  '-'  '-. |  `---.  |  |   |  | ('  '-'(_.-'
'--'   '--'      `-'      `----'                `-----'--' `------'  `--'   `--'   `-----'

xv6 kernel is booting

hart 1 starting
init: starting sh
$ 
```
## 依赖
* k210 开发板或者 qemu-system-riscv64
* RISC-V GCC 编译链: riscv-gnu-toolchain
* 编译前需要设置环境变量，如下所示：
```shell
export ARCH=riscv  
export CROSS_COMPILE=riscv64-unknown-linux-gnu- 
export PATH=$PATH:/home/xikao/quard_star_tutorial/toolchain/gcc-riscv64-unknown-linux-gnu/bin/
```

## 在 qemu-system-riscv64 模拟器上运行
首先，确保 qemu-system-riscv64 已经下载到您的机器上并且加到了环境变量中；
```shell
make fs
make qemu platform=qemu
```

## 在 k210 开发板上运行
首先，将SD卡插入到读卡器上，并连接到PC上
```shell
sudo mkfs.vfat -I -F 32 /dev/sdb    # 格式化为 FAT32 格式
sudo dd if=target/fs.img of=/dev/<SD卡设备> bs=1M status=progress  # 下载到SD卡
make fs
make qemu platform=k210   # 编译并连接到 k210 开发板串口
```

## 关于 Shell
目前已经支持几个常用命令，如 cd，ls，cat 等。
此外，shell 支持下列快捷键：
* Ctrl-H -- 退格
* Ctrl-U -- 删除行
* Ctrl-D -- 文件尾（EOF）
* Ctrl-P -- 打印进程列表

## 进度
* ✅多核启动
* ✅修改文件系统格式为 FAT32
* ✅移植到k210开发板
* ✅支持SD卡读写：GPIO、SPI、DMA和SD卡