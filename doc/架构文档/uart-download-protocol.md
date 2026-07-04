# UART 文件系统镜像烧写协议

## 1. 背景

原始更新 K210 板端文件系统通常需要把 SD 卡取下，用读卡器写入新的 `fs.img`。这个流程调试成本高，也不符合真实嵌入式产品中常见的在线升级、串口烧写或量产下载思路。

本项目实现了通过 UART 将主机端 `target/fs.img` 写入 K210 SD 卡的流程：

```text
主机 tools/burn.py
  -> UART
  -> 板端 user/app/burn.c
  -> DEV_UART raw 模式
  -> DEV_SDCARD 扇区写入
  -> SD 卡 FAT32 镜像
```

这个功能是项目中最适合作为秋招亮点讲述的工程闭环。

## 2. 相关文件

| 文件 | 作用 |
|------|------|
| `user/app/burn.c` | 板端烧写程序，接收协议包并写 SD 卡 |
| `tools/burn.py` | 主机端发送脚本 |
| `kernel/devsw/uartdev.c` | raw UART 字符设备 |
| `kernel/driver/uarths.c` | UART 底层驱动和 raw ring buffer |
| `kernel/devsw/sdcarddev.c` | SD 卡 raw 扇区设备 |
| `kernel/include/uartdev.h` | UART ioctl 定义 |
| `kernel/include/sdcarddev.h` | SD 卡 ioctl 定义 |

## 3. 协议帧格式

当前协议使用二进制帧：

```text
magic(4) = 55 AA 55 AA
seq(4 LE)
type(1)
plen(2 LE)
payload(plen)
crc32(4 LE)
```

说明：

- `magic` 用于流同步。
- `seq` 是包序号，DATA 阶段等于扇区号。
- `type` 表示包类型。
- `plen` 表示 payload 长度。
- `crc32` 覆盖从 `magic` 到 `payload` 的所有字节。
- CRC32 使用 reflected polynomial `0xEDB88320`，与 Python `zlib.crc32()` 兼容。

接收端支持重新同步：如果字节流中出现无效 header，会继续滑动查找下一个 magic。

## 4. 包类型

```text
PKT_INFO = 0x01  主机 -> 板端，发送镜像大小和可选传输 baud
PKT_DATA = 0x02  主机 -> 板端，发送一个 512 字节扇区
PKT_DONE = 0x03  主机 -> 板端，表示所有数据发送完成
PKT_BAUD = 0x04  主机 -> 板端，用于 baud 切换同步
PKT_ACK  = 0x81  板端 -> 主机，表示接受
PKT_NAK  = 0x82  板端 -> 主机，表示拒绝
```

ACK payload：

```text
reason(1)
ticks(4 LE)
```

`reason` 当前包括：

| reason | 含义 |
|--------|------|
| `ACK_OK` | DATA 正常写入 |
| `ACK_DUP` | 收到已经写过的旧 DATA，重发 ACK，不重复写 SD |
| `ACK_INFO` | INFO 阶段确认 |
| `ACK_DONE` | DONE 阶段确认 |
| `ACK_BAUD` | baud 切换阶段确认 |

`ticks` 在 DATA 成功写入时记录一次 SD 卡 `write()` 花费的 `uptime()` tick，可用于后续统计 SD 写入耗时。

NAK payload 当前为错误码：

| 错误码 | 含义 |
|--------|------|
| `ERR_CRC` | CRC 或协议包错误 |
| `ERR_WRITE` | SD 卡写入失败 |

## 5. 板端流程

板端 `burn` 程序流程：

```text
打开 DEV_UART
打开 DEV_SDCARD
初始化 OLED
UART_IOCTL_RAW_START
发送 "BURN\n"

接收 INFO
  解析 total_size 和 transfer_baud
  计算 nsectors
  检查 SD 卡容量
  SDCARD_IOCTL_SEEK 到 0
  ACK INFO

可选 baud 切换
  接收 BAUD
  ACK BAUD
  UART_IOCTL_SET_BAUD
  再接收 BAUD 同步
  ACK BAUD

循环接收 DATA
  CRC 错误 -> NAK
  seq < 当前扇区 -> ACK_DUP，不重复写
  seq != 当前扇区 -> NAK
  正常 -> write(sdcard_fd, payload, 512) -> ACK_OK

接收 DONE
  ACK_DONE

恢复 115200 baud
SDCARD_IOCTL_INVALIDATE_CACHE
UART_IOCTL_RAW_END
打印完成日志
```

ACK 只在 SD 卡写入成功之后发送。这样主机如果超时或收到 NAK，可以安全重传当前扇区。

## 6. raw UART 模式

普通 console 输入路径不适合传输二进制文件，因为：

- console 会处理字符输入。
- `\n`、`\r` 等字符有特殊语义。
- 二进制数据中可能包含任意字节。

因此项目实现了 `DEV_UART` raw 模式：

```text
normal mode:
  UART RX interrupt
    -> uartintr()
    -> consoleintr()

raw mode:
  UART RX interrupt
    -> uartintr()
    -> uart_raw_input()
    -> raw ring buffer
    -> uartdev_read()
    -> burn.c read()
```

关键 ioctl：

- `UART_IOCTL_RAW_START`：关闭 console 接管，清空硬件 RX FIFO，进入 raw 模式。
- `UART_IOCTL_RAW_END`：退出 raw 模式，清空 raw buffer，恢复 console。
- `UART_IOCTL_SET_BAUD`：设置 UART baud。
- `UART_IOCTL_GET_BAUD_INFO`：查询实际 baud 和分频信息。
- `UART_IOCTL_GET_RAW_STATS`：查询 raw ring 的 dropped、buffered、capacity 和当前模式。

raw ring buffer 当前在 `kernel/driver/uarths.c` 中维护，大小为 32768 字节，用于缓冲中断接收到的数据。

## 7. baud 切换策略

为了兼顾 shell 启动稳定性和数据传输效率，当前流程分为两个阶段：

- shell 和 INFO 初始握手使用 115200 baud。
- 数据阶段可以切换到更高 baud。

baud 切换采用两步同步：

1. 主机在旧 baud 下发送 `PKT_BAUD`，板端 ACK 后切换本地 UART baud。
2. 主机切换串口 baud 后，再发送一次 `PKT_BAUD`，板端收到后确认新 baud 可用。

这样可以避免双方一端已经切换、另一端还在旧 baud 时直接进入数据阶段。

## 8. 错误恢复

当前协议采用 stop-and-wait 模式，即一个 DATA 包写入并 ACK 后再发送下一个 DATA 包。

优点：

- 实现简单。
- 便于定位错误。
- 能保证 ACK 对应具体扇区。
- SD 卡写入耗时不会导致连续多个包积压。

错误处理策略：

- CRC 错误：板端返回 NAK，主机重传。
- 包类型、长度或序号错误：板端返回 NAK。
- SD 写入失败：板端返回 `ERR_WRITE`。
- 主机未收到 ACK：重发同一扇区。
- 板端收到旧扇区：返回 `ACK_DUP`，但不重复写 SD 卡。
- 板端在 raw ring 满时丢弃新字节并累加 dropped 计数，烧写结束或失败时通过 `UART_IOCTL_GET_RAW_STATS` 打印 `raw_drop` 和 `raw_buf`。

板端 `burn` 会统计并在恢复 console 后输出：

```text
burn: stats ticks=... sd_total=... sd_max=... dup=... crc=... io=... pkt=... sd=... raw_drop=... raw_buf=.../...
```

主机 `tools/burn.py` 会统计并输出：

```text
Transfer stats: elapsed=...s, throughput=... KiB/s, retries=..., timeouts=..., crc_errors=..., naks=..., stale=..., ack_dup=..., sd_ticks_total=..., sd_ticks_max=...
```

这些统计用于记录不同 baud 下的稳定性和吞吐表现。一次稳定传输的基本验收标准是：主机完成 DONE ACK，板端 `raw_drop=0`，重传/NAK/timeout 计数长期接近 0。

## 9. OLED 与 printf 的限制

数据热路径中应尽量避免：

- 频繁 `printf()`。
- 频繁刷新 OLED。
- 过长时间关中断。

原因是 K210 UARTHS 硬件 RX FIFO 很小，I2C 刷 OLED 或 console 输出过慢时，可能导致 UART 接收溢出。

当前策略：

- OLED 只显示阶段、错误和稀疏进度。
- 每 128 个扇区刷新一次进度。
- raw 模式期间尽量使用 UART fd 进行协议通信，不使用 stdout 打印调试信息。

## 10. 面试讲述重点

推荐讲法：

1. 为了解决频繁取 SD 卡的问题，我设计了 UART 烧写 `fs.img` 的流程。
2. 用户在 shell 中运行 `burn`，主机脚本发送镜像。
3. 协议有 magic、seq、type、len、payload、CRC32。
4. 板端使用 raw UART ring buffer 接收二进制流，避免 console 干扰。
5. 每个 DATA 包对应一个 512 字节扇区，写 SD 成功后才 ACK。
6. 支持 CRC 校验、NAK 重传、重复包识别、baud 切换和缓存失效。
7. 这个功能串起了 UART、中断、buffer、用户态协议、SD 卡写入和 FAT32 缓存一致性。

适合回答：“你项目里最有工程难度的部分是什么？”

稳定性数据：

host baud	board baud	镜像大小	扇区数	耗时	吞吐	retries	timeouts	naks	raw_drop	结论
460800	500000	2487296 B	4858	103.55 s	23.5 KiB/s	0	0	0	0	稳定

