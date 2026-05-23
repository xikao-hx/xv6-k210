# XV6-RISCV On K210
## 参考说明
* 移植XV6到K210的部分，完全是照搬的如下仓库，对此表示感谢
https://github.com/HUST-OS/xv6-k210#
* 我的主要工作是对XV6内核增加相应功能，新增功能如下进度章节所示

## 启动日志
### QEMU
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

### K210
```shell
\ / (_/ /  .'    ( OO).-> |  .'   / \_,-.  | /_  |  /  ..  \
  \    .')  \   /   / .  / -.  (,------. |      /)    .' .'  |  | |  /  \  .
  .'    \  _ \     /_)'  .-. \  `------' |  .   '   .'  /_   |  | '  \  /  '
 /  .'.  \ \-'\   /   \  `-' /           |  |\   \ |      |  |  |  \  `'  /
`--'   '--'    `-'     `----'            `--' '--' `------'  `--'   `---''

xv6 kernel is booting

SDHC/SDXC detected
hart 0 init done
hart 1 starting
init: starting sh
$ 
```

## 依赖
* k210 开发板或者 qemu-system-riscv64
* RISC-V GCC 编译链: riscv-gnu-toolchain
* 编译前需要设置环境变量，如下所示：
  * 工具链1:
    ```shell
    export ARCH=riscv  
    export CROSS_COMPILE=riscv64-unknown-linux-gnu- 
    export PATH=$PATH:/home/xikao/
    TOOLPREFIX = riscv64-unknown-linux-gnu-
    ```
  * 工具链2:
    ```shell
    sudo apt update
    sudo apt install gcc-riscv64-unknown-elf
    TOOLPREFIX = riscv64-unknown-elf-
    ```

## 在 qemu-system-riscv64 模拟器上运行
首先，确保 qemu-system-riscv64 已经下载到您的机器上并且加到了环境变量中；
```shell
make fs
make qemu platform=qemu
```

## 在 k210 开发板上运行
### 烧写SDK到Flash中
```shell
make qemu platform=k210   # 编译并连接到 k210 开发板串口
```

### 烧写文件系统
**方式1**：读卡器
首先，将SD卡插入到读卡器上，并连接到PC上
```shell
make sdcard dev-sd=/dev/sdX
```

**方式2**：直接烧写
```shell
# 1.首先确保板子是正常启动状态
# 2.执行下载
make download
# 默认 115200 握手，主机 230400 传输，板端 baud 自动补偿到约 253440
python3 tools/burn.py /dev/ttyUSB0 target/fs.img
# 如需手动指定补偿值：
python3 tools/burn.py --baud 230400 --board-baud 253400 /dev/ttyUSB0 target/fs.img
```

## 关于 Shell
目前已经支持几个常用命令，如 cd，ls，cat 等。
此外，shell 支持下列快捷键：
* Ctrl-H -- 退格
* Ctrl-U -- 删除行
* Ctrl-D -- 文件尾（EOF）
* Ctrl-P -- 打印进程列表
* Tab    -- 联想功能

## 进度
* ✅完善xv6内核
  * ✅支持COW、Lazy allocation、Mmap
  * ✅增加进程的内核页表，并将用户空间的映射添加到内核页表，使得内核能够直接解引用用户指针
  * ✅内存分配器的共享空闲链表拆分成每个CPU的内存池
  * ✅对磁盘块号进行哈希分桶并拆分锁
  * ✅修改inode添加大文件支持，并添加符号链接功能
* ✅多核启动：2个CPU
* ✅修改文件系统格式为 FAT32
* ✅移植到k210开发板
* ✅支持SD卡读写：GPIO、SPI、DMA和SD卡
* ✅支持通过I2C和SPI协议读写设备
  * ✅编写设备驱动
  * ✅增加 ioctl 系统调用支持用户态完成读取
* ✅支持 UARTHS 驱动: 目前分析来看高波特下存在BUG，具体原因还不清楚，先强行解决
  * ✅raw UART 使用中断 + ring buffer 接收二进制数据
  * ✅通过 UART 高速接收 `fs.img`，并写入到 SD 卡中
