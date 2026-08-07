# XV6-RISCV On K210
## 参考说明
* 移植XV6到K210的部分，完全是使用的如下仓库的内容，对此表示感谢
  * https://github.com/HUST-OS/xv6-k210#
* 我的主要工作是对XV6内核增加部分功能和优化驱动，所有功能如下进度章节所示

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
* 工具链下载和使用:
```shell
sudo apt update
sudo apt install gcc-riscv64-unknown-elf
TOOLPREFIX = riscv64-unknown-elf-
```

## 在 qemu-system-riscv64 模拟器上运行
首先，确保 qemu-system-riscv64 已经下载到您的机器上并且加到了环境变量中；
```shell
make clean
make run platform=qemu
```

## 在 k210 开发板上运行
### 烧写 SDK 到 Flash 中
```shell
make clean

# 编译并连接到 k210 开发板串口，然后完成固件下载到Flash
lsblk          # 查看设备节点
make run platform=k210   

# 强制重新编译，并执行日志等级
make build platform=k210 LOG_LEVEL=LOG_LEVEL_DEBUG -B
```

### 烧写文件系统到 SD 中
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

# 2.1 下载命令解释
# 默认 115200 握手，主机 460800 传输，板端 baud 自动补偿到约 500000
python3 tools/burn.py /dev/ttyUSB0 target/fs.img
# 手动指定补偿值
python3 tools/burn.py --baud 460800 --board-baud 500000 /dev/ttyUSB0 target/fs.img
```

## 关于 Shell
目前已经支持几个常用命令，如 cd，ls，cat 等。
此外，shell 支持下列快捷键：
* 上键  -- 历史命令（兼容部分串口工具丢失 `ESC [ A` 的情况）
* Ctrl-H -- 退格
* Ctrl-U -- 删除行
* Ctrl-D -- 文件尾（EOF）
* Ctrl-P -- 打印进程列表
* Tab    -- 联想功能

## 功能

### 平台支持
* ✅ 移植到 K210 开发板：适配 GPIO、SPI、I2C、DMA、FPIOA、SYSCTL、UARTHS、SD 卡等底层驱动
* ✅ 双平台支持：QEMU 模拟器（virtio 磁盘 + PLIC）与 K210 硬件共用同一套内核上层逻辑，`make build platform=qemu|k210` 一键切换

### 文件系统与存储
* ✅ 修改文件系统格式为 FAT32，支持 SD 卡读写
* ✅ 修改 inode 添加大文件支持
* ✅ 添加符号链接功能

### 内存管理
* ✅ 支持 COW、Lazy allocation、Mmap（文件映射 / 匿名映射 / 设备共享缓冲区映射）
* ✅ 增加进程的内核页表，并将用户空间的映射添加到内核页表，使得内核能够直接解引用用户指针
* ✅ 内核态访问用户内存统一收敛为 copyin/copyout 中处理 page fault
* ✅ 内存分配器：共享空闲链表拆分成每个 CPU 的内存池，支持 kmalloc/kfree 小块分配内存

### 并发与同步
* ✅ 对磁盘块号进行哈希分桶并拆分锁
* ✅ 优化 buffer cache 分桶层锁、per-CPU allocator 锁的拆分

### 进程与调度
* ✅ 实现 MLFQ 多级反馈调度算法：4 级队列、时间片 1/2/4/8 ticks、周期 Boost 防饥饿、IO 唤醒优先级提升；支持 `SCHED=mlfq` / `SCHED=rr` 编译切换
* ✅ 新增 trace、sysinfo 系统调用

### 信号机制
* ✅ 新增 signal / sigsend / sigreturn 系统调用，支持用户态注册信号处理函数
* ✅ TTY 模式下 Ctrl+C 中断前台任务，信号可唤醒阻塞在 sleep / pipe / console 等可中断睡眠中的进程

### 设备驱动与 I2C/SPI
* ✅ 支持通过 I2C 和 SPI 协议读写设备，编写对应外设驱动（MPU6050、W25Q64、OLED 等）
* ✅ 增加 ioctl 系统调用，支持用户态完成读取与控制
* ✅ 重构设备框架：`device_register` + `file_operations` 抽象，设备节点统一通过 FAT32 `mknod` + `open` 管理
* ✅ 支持 UARTHS 驱动，提供两条链路：
  * ✅ 服务 console 路径
  * ✅ 刷写 SD 卡：raw UART 使用中断 + ring buffer 接收二进制数据

### UART 刷写 SD 卡
* ✅ 设计传输协议，支持应答、超时检测、重传等机制
* ✅ 主机/板端波特率分离，两步切换握手（旧 baud 确认 → 新 baud 同步），解决高波特率下两侧不同步的问题
* ✅ 通过 ioctl 支持清除旧的 bcache
* ✅ 完成 UART 高速接收 `fs.img`，并写入到 SD 卡中
