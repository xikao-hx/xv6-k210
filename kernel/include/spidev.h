#ifndef _SPIDEV_H
#define _SPIDEV_H

#include "types.h"
#include "dev.h"

// minor: SPI_MINOR(spi_bus, chip_select)

// ioctl commands
#define SPI_IOCTL_INIT      1   // arg = uint32 clock rate
#define SPI_IOCTL_TRANSFER  2   // arg = &spidev_transfer (user addr)

struct spidev_transfer {
    uint64 tx_buf;   // user-space address (NULL = receive only)
    uint64 rx_buf;   // user-space address (NULL = send only)
    uint32 len;      // total length
    uint32 cmd_len;  // when both tx_buf and rx_buf: bytes from tx_buf sent as command,
                     // then (len - cmd_len) bytes received into rx_buf (CS stays low)
                     // if cmd_len==0 when both are set, defaults to all-tx-no-rx
};

void spidev_init(void);

#endif /* _SPIDEV_H */
