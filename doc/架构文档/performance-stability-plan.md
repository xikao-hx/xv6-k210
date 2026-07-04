# 性能与稳定性数据计划

## 目标

本计划用于把项目从“功能能跑”推进到“有量化数据证明稳定性”。最终产物包括：

- 板端 benchmark/test 命令。
- 主机侧采集脚本或 CSV 输出。
- 原始数据 CSV。
- 汇总报告 Markdown。
- README 或面试材料中可引用的关键指标。

本阶段不伪造数据，只定义采集方案、指标和报告格式。

## 测试环境记录

每次采集数据前记录：

| 字段 | 示例 |
| --- | --- |
| 开发板 | Sipeed Maix/K210 |
| SD 卡 | 品牌、容量、速度等级 |
| SPI Flash | W25Q64 或兼容型号 |
| I2C 外设 | SSD1306 OLED |
| 串口工具 | USB-UART 型号 |
| 编译版本 | git commit 或日期 |
| fs.img 大小 | 64MiB |
| 编译命令 | `make build platform=k210 && make fs` |

## 指标一：UART 下载稳定性

覆盖链路：

```text
tools/burn.py
  -> UART frame protocol
  -> user/app/burn.c
  -> DEV_UART read/write/ioctl
  -> DEV_SDCARD write/ioctl
  -> SD card
```

建议测试波特率：

| baud | 目的 |
| ---: | --- |
| 115200 | 基线稳定性 |
| 230400 | 保守推荐配置 |
| 460800 | 当前常用配置 |
| 500000 | K210 常见高波特率配置 |
| 921600 | 压力边界 |

采集字段：

```csv
date,board,sd_card,baud,board_baud,image_size,elapsed_ms,throughput_kib_s,retries,timeouts,naks,crc_errors,raw_dropped,result,notes
```

成功标准：

- 主机收到 DONE ACK。
- `raw_dropped=0`。
- 多轮传输中 `retries/timeouts/naks/crc_errors` 长期接近 0。
- 写入后重启能正常进入 shell 并读取 FAT32 文件。

建议运行：

```sh
python3 tools/burn.py --baud 230400 --board-baud 230400 /dev/ttyUSB0 target/fs.img
python3 tools/burn.py --baud 460800 --board-baud 500000 /dev/ttyUSB0 target/fs.img
```

报告输出：

- 最稳定 baud。
- 最高可用 baud。
- 吞吐最高配置。
- 失败配置和现象。

## 指标二：SD 卡 raw 扇区性能

覆盖链路：

```text
user/test/sdbench.c
  -> DEV_SDCARD read/write/ioctl
  -> kernel/devsw/sdcarddev.c
  -> kernel/driver/sdcard.c
```

建议新增命令：

```text
sdbench read <sectors>
sdbench verify <sectors>
sdbench write <start-sector> <sectors> --danger-write
```

默认只做只读测试，写测试必须显式传入 `--danger-write`，避免破坏 FAT32 镜像。

采集字段：

```csv
date,board,sd_card,mode,start_sector,sectors,bytes,elapsed_ticks,throughput_kib_s,checksum,errors,result,notes
```

成功标准：

- 连续读无错误。
- checksum 稳定。
- 写测试仅在安全 sector 范围执行，写后 verify 通过。

## 指标三：SPI / W25Q64 性能与稳定性

覆盖链路：

```text
user/test/spibench.c
  -> DEV_SPI + SPI_MINOR(1, 0)
  -> SPI_IOCTL_TRANSFER
  -> kernel/devsw/spidev.c
  -> kernel/driver/spi.c
  -> W25Q64
```

建议新增或增强命令：

```text
spibench id <count>
spibench read <addr> <len> <count>
spibench write-verify <addr> <len> --danger-erase
```

默认只做 JEDEC ID 和 read，不擦写 Flash。

采集字段：

```csv
date,board,flash,mode,spi_bus,cs,addr,len,count,elapsed_ticks,throughput_kib_s,errors,checksum,result,notes
```

成功标准：

- JEDEC ID 连续读取稳定。
- read checksum 多轮一致。
- 擦写测试必须显式开启，并记录测试地址范围。

## 指标四：I2C / OLED 稳定性

覆盖链路：

```text
user/test/i2cbench.c
  -> DEV_I2C + I2C_MINOR(0)
  -> I2C_IOCTL_TRANSFER
  -> kernel/devsw/i2cdev.c
  -> kernel/driver/i2c.c
  -> SSD1306 OLED
```

建议新增命令：

```text
i2cbench scan <count>
i2cbench oled <count>
i2cbench oled-rate <clock> <count>
```

建议测试 I2C clock：

| clock | 目的 |
| ---: | --- |
| 50000 | 保守稳定性 |
| 100000 | 常规配置 |
| 400000 | 高速边界 |

采集字段：

```csv
date,board,device,mode,clock,count,elapsed_ticks,transfers,errors,result,notes
```

成功标准：

- OLED 初始化成功。
- 连续写屏无错误。
- 地址扫描结果稳定包含 `0x3c`。

## 指标五：DMA 路径

当前 mini 版先通过 SPI dev 内部 DMA-backed buffer 路径验证，不新增 DMA 用户态 ABI。

覆盖链路：

```text
user/test/dmactest.c or spibench dma-read
  -> DEV_SPI
  -> SPI_IOCTL_TRANSFER
  -> spidev DMA buffer path
```

采集字段：

```csv
date,board,mode,len,count,elapsed_ticks,throughput_kib_s,checksum,errors,result,notes
```

后续如果需要做 DMA 与非 DMA 对比，再考虑新增明确的 benchmark 模式，而不是直接暴露底层 DMA ioctl。

## 指标六：FAT32 与 buffer cache 压力

覆盖链路：

```text
user/test/fsbench.c
  -> open/read/write/unlink/readdir
  -> FAT32
  -> buffer cache
  -> disk
```

建议新增命令：

```text
fsbench create <files>
fsbench read <files>
fsbench remove <files>
fsbench ls <count>
```

采集字段：

```csv
date,board,mode,files,total_bytes,elapsed_ticks,errors,result,notes
```

成功标准：

- 多轮创建、读取、删除无错误。
- 重启后 FAT32 目录状态符合预期。

## 实施顺序

1. 定义数据目录和报告模板。

   建议新增：

   ```text
   data/
   data/uart-burn.csv
   data/sdbench.csv
   data/spibench.csv
   data/i2cbench.csv
   doc/架构文档/performance-stability-report.md
   ```

2. 先采集 UART 下载数据。

   这是项目最有工程亮点的链路，优先形成可展示表格。

3. 新增板端 bench 命令。

   优先级：

   ```text
   sdbench -> spibench -> i2cbench -> fsbench
   ```

4. 真板多轮采集。

   每个项目建议至少：

   - 3 轮普通测试。
   - 1 轮长时间稳定性测试。
   - 1 轮边界配置测试。

5. 写汇总报告。

   报告只放关键结论和表格，原始数据保存在 CSV。

## 报告模板

```md
# 性能与稳定性报告

## 测试环境

## UART 下载性能

## SD 卡 raw 扇区性能

## SPI / W25Q64 性能

## I2C / OLED 稳定性

## DMA 路径

## FAT32 与 buffer cache 压力

## 推荐演示配置

## 问题与边界
```

## 风险边界

- SD 写测试默认禁止，必须显式 `--danger-write`，并指定安全 sector。
- W25Q64 擦写测试默认禁止，必须显式 `--danger-erase`，并指定测试地址。
- 高 baud、高 I2C clock 属于边界测试，失败结果要记录，不应作为默认演示配置。
- 数据采集前要确认本次 `fs.img` 与板端镜像一致，否则 UART 下载耗时和文件系统验证没有可比性。
