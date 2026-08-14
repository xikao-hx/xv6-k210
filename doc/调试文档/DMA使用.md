SPI/I2C：DMA 是"同步事务"，CPU 自己轮询完成
看 spi.c:340-341 和 i2c.c:331-338：


dmac_set_single_mode(chan_tx, tx_buf, &spi_handle->dr[0], ...);  // 发起 DMA
dmac_wait_done(chan_tx, DMAC_WAIT_TIMEOUT);                       // 忙等完成
dmac_wait_done(chan_rx, DMAC_WAIT_TIMEOUT);
dmac_wait_done（dmac.c:336-347）是纯忙等——反复读 chen 寄存器直到通道 idle，根本不经过 PLIC、不注册任何中断处理函数：


int dmac_wait_done(dmac_channel_number_t channel_num, uint64 timeout) {
  while(!dmac_is_idle(channel_num)) {   // 轮询 chen 的 en 位
    if(timeout-- == 0) { ... return -1; }
  }
  return 0;
}
一次 SPI/I2C 事务的结构是：发起 → 忙等 → 返回。DMA 的整个生命周期都在调用者的控制流里，CPU 自己"看见"了完成时刻，不需要外部打断。所以这类中断源既不需要在 plicinit() 里设非零优先级，也不需要在 plicinithart() 里置 enable 位——它们的 DMA 完成根本没接进 PLIC 的中断通路。

UART RX DMA：DMA 是"异步常驻"，必须靠中断收尾
UART RX 完全不同：驱动一上来就 arm 一个 512 字的常驻传输，数据在任何时刻零散到达（串口没有固定帧长、没有事务边界）。CPU 不可能忙等它"传完"——

可能几毫秒来一个字节，也可能几个钟头不来；
忙等期间 CPU 被独占，其他进程全卡死；
而且 512 字常常根本收不满，不存在 SPI 那种"传完=完成"的终点。
所以必须让 DMA 完成（IRQ 32）/ UART RDA / CTI 这三个边界事件以中断形式打断 CPU：进 ISR → disable → harvest（把 dar 记的已搬字节推入 ring）→ 立即 re-arm → wake reader。既然要走中断，就绕不开 PLIC 的两道开关：

plicinit() 源级优先级非零（你问的那行）——否则 PLIC 永不转发这个源；
plicinithart() 该 hart 的 enable 位——否则这个 hart 不接收。
一个反证：UART TX DMA 就没开 CH4 中断
UART TX DMA 用的是和 SPI/I2C 一模一样的阻塞 wait_done（uart.c 里 dmac_wait_done(CH4, timeout)，在 tx.lock 外），所以 plicinit/plicinithart 只给 CH5（RX，异步）设了优先级和 enable 位，CH4（TX，同步）没配——和 SPI/I2C 的通道待遇完全一致。两方向一对比，"谁走中断谁就需要单独开"的规律一目了然。