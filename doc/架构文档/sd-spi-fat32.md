# SD 卡、SPI 与 FAT32 的关系

## 1. 整体路径

K210 真板上，文件系统数据最终保存在 SD 卡中。访问路径可以分成两条：

普通文件系统路径：

```text
用户程序 open/read/write/exec
  -> sysfile
  -> file.c
  -> fat32.c
  -> bio.c buffer cache
  -> disk.c
  -> sdcard.c
  -> spi.c / dmac.c
  -> SD 卡
```

UART 烧写路径：

```text
主机 burn.py
  -> UART
  -> user/app/burn.c
  -> DEV_UART
  -> DEV_SDCARD
  -> sdcard.c
  -> spi.c / dmac.c
  -> SD 卡
```

两条路径最终都访问同一张 SD 卡，但层次不同：普通文件访问经过 FAT32 和 buffer cache，UART 烧写直接按扇区写卡。

## 2. 平台差异

QEMU 平台：

```text
FAT32
  -> bio
  -> disk.c
  -> virtio_disk.c
  -> QEMU virtio block device
```

K210 平台：

```text
FAT32
  -> bio
  -> disk.c
  -> sdcard.c
  -> SPI/DMAC
  -> SD card
```

`kernel/fs/disk.c` 负责屏蔽差异：

```text
QEMU:
  disk_init  -> virtio_disk_init
  disk_read  -> virtio_disk_rw(..., 0)
  disk_write -> virtio_disk_rw(..., 1)

K210:
  disk_init  -> sdcard_init
  disk_read  -> sdcard_read_sector
  disk_write -> sdcard_write_sector
```

因此 FAT32 上层不需要知道自己运行在 QEMU 还是 K210。

## 3. SD 卡驱动角色

K210 上 SD 卡驱动主要提供扇区级读写能力：

```text
sdcard_init()
sdcard_read_sector(buf, sector)
sdcard_write_sector(buf, sector)
sdcard_nsectors()
```

文件系统不会直接理解 SD 卡协议，而是通过 `disk_read()` 和 `disk_write()` 间接访问块设备。

## 4. SPI 与 DMAC 的角色

K210 通过 SPI 控制器访问 SD 卡。SPI 驱动负责配置控制器、片选、发送命令和收发数据。

DMAC 用于提升数据搬运效率，尤其是大块数据传输时，避免 CPU 逐字节搬运。

可以按以下关系理解：

```text
FAT32 关心文件和目录
bio 关心块缓存
disk 关心统一块设备接口
sdcard 关心 SD 协议和扇区
spi/dmac 关心硬件收发
```

这是嵌入式驱动开发里常见的分层：文件系统、块设备、总线控制器、具体介质各自负责不同抽象。

## 5. FAT32 层

FAT32 层负责：

- 路径解析。
- 目录项管理。
- 文件读写。
- cluster 链管理。
- 文件大小和属性维护。
- 为 `exec` 提供用户程序读取能力。

用户态的 `ls`、`cat`、`sh`、`exec` 等最终都依赖 FAT32 能正确读写 SD 卡中的文件。

## 6. buffer cache 层

`kernel/fs/bio.c` 提供 buffer cache。它的作用是：

- 缓存最近访问过的磁盘块。
- 减少重复读 SD 卡。
- 为文件系统提供带锁的块访问接口。

这也带来一个问题：如果有路径绕过 FAT32 和 bio 直接改写 SD 卡，cache 中可能还保存旧数据。

UART 烧写 `fs.img` 就属于这种情况。

## 7. raw SD 卡设备

`kernel/devsw/sdcarddev.c` 提供用户态 raw SD 卡访问：

- `read(fd, buf, 512)`：读当前扇区。
- `write(fd, buf, 512)`：写当前扇区。
- 每次读写后扇区号自动加 1。

相关 ioctl：

| ioctl | 作用 |
|------|------|
| `SDCARD_IOCTL_SEEK` | 设置当前扇区号 |
| `SDCARD_IOCTL_TELL` | 查询当前扇区号 |
| `SDCARD_IOCTL_NSECTORS` | 查询 SD 卡总扇区数 |
| `SDCARD_IOCTL_INVALIDATE_CACHE` | raw 写盘后失效 FAT32 和 buffer cache |

`burn` 使用这个设备从 0 扇区开始写入 `fs.img`。

## 8. 为什么烧写后要失效缓存

UART 烧写流程直接写 SD 卡：

```text
burn.c
  -> write(sdcard_fd, payload, 512)
  -> sdcarddev_write()
  -> sdcard_write_sector()
```

这条路径绕过了：

- FAT32 目录项缓存。
- buffer cache 中已有块。

如果不失效缓存，可能出现：

- SD 卡上已经写入新文件系统。
- 内核仍然从旧 cache 中读取目录或文件。
- 连续烧写后 `exec` 或 `ls` 看到旧内容。

因此烧写完成后调用：

```text
SDCARD_IOCTL_INVALIDATE_CACHE
  -> fat32_invalidate()
  -> binvalidate(0)
```

这样可以让后续文件系统访问重新从 SD 卡读取数据。

## 9. 常见调试点

SD/SPI/FAT32 相关问题通常可以按层次定位：

1. SPI 层：片选、时钟、模式、引脚复用是否正确。
2. SD 协议层：初始化命令是否成功，卡类型和容量是否识别正确。
3. 块读写层：固定扇区读写是否一致。
4. buffer cache 层：是否读到旧缓存。
5. FAT32 层：BPB、FAT 表、目录项、cluster 链是否解析正确。
6. 用户态：`ls`、`cat`、`exec` 是否表现异常。

建议后续补充一个 `sdtest`，先脱离 FAT32 做扇区读写验证，再用 `ls/cat/exec` 验证文件系统路径。

## 10. 面试讲述重点

可以这样讲：

1. QEMU 用 virtio disk，K210 用 SPI SD 卡，但我用 `disk.c` 抽象统一块设备。
2. FAT32 只依赖 `disk_read/write`，不用关心底层是 virtio 还是 SD 卡。
3. K210 下 SD 卡驱动通过 SPI 和 DMAC 访问真实硬件。
4. UART 烧写为了更新整个文件系统镜像，绕过 FAT32 直接写 raw sector。
5. raw 写盘后必须失效 FAT32 和 buffer cache，否则会读到旧数据。

这部分适合回答：“你的文件系统数据最后怎么落到板子上？”、“SD 卡驱动和 FAT32 是什么关系？”

