# 板端外设测试命令

本文记录 K210 真板 shell 中可直接运行的外设测试命令，用于快速验证 `devsw`、`read`、`write`、`ioctl` 到底层驱动的链路。

## 命令列表

| 命令 | 覆盖范围 | 默认行为 |
| --- | --- | --- |
| `uarttest` | UART dev、baud/raw stats ioctl、write 路径 | 读取 UART 配置和 raw ring 统计，写一行测试文本 |
| `sdtest` | SD card dev、sector seek/tell/read ioctl | 只读 sector 0 和 sector 32，输出 checksum，恢复原 sector 位置 |
| `spitest` | SPI dev、W25Q64 JEDEC ID | 读取 W25Q64 JEDEC ID，不擦写 flash |
| `i2ctest` | I2C dev、地址扫描、OLED 写入 | 扫描 0x08 到 0x77，探测 OLED `0x3C`，并在屏幕显示测试文本 |
| `dmactest` | SPI dev 内部 DMA-backed 读路径 | 通过 SPI 从 W25Q64 地址 0 读取 256 字节并输出 checksum |
| `devtest` | 统一设备打开和关键 ioctl | 使用 `dev.h` 的设备号和 minor 宏打开 console/stats/uart/sdcard/spi/i2c，并验证 UART/SD ioctl |

## 预期输出

每个命令最后都会输出统一结果：

```text
... test PASSED, failures=0
```

如果外设未连接、接线错误或驱动返回错误，会输出：

```text
FAIL: ...
... test FAILED, failures=N
```

## 使用建议

- 先运行 `devtest` 确认设备号和基础 ioctl 链路可用。
- 再运行 `uarttest`、`sdtest` 验证板载基础链路。
- 接入 W25Q64 后运行 `spitest` 和 `dmactest`。
- 接入 SSD1306 OLED 后运行 `i2ctest`。
- `sdtest` 当前为只读测试，不会写 raw sector，避免破坏 FAT32 镜像。
