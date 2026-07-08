#ifndef _SPIDEV_H
#define _SPIDEV_H

#include "types.h"
#include "dev.h"

// minor: SPI_MINOR(spi_bus, chip_select)

// ioctl commands
#define SPI_IOCTL_INIT      1   // arg = uint32 clock rate

#define SPI_IOC_MAGIC       'k'

#define _IOC_NRBITS         8
#define _IOC_TYPEBITS       8
#define _IOC_SIZEBITS       14
#define _IOC_DIRBITS        2

#define _IOC_NRSHIFT        0
#define _IOC_TYPESHIFT      (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT      (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT       (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_WRITE          1U

#define _IOC(dir, type, nr, size) \
    (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
     ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IOC_DIR(nr)        (((nr) >> _IOC_DIRSHIFT) & ((1U << _IOC_DIRBITS) - 1))
#define _IOC_TYPE(nr)       (((nr) >> _IOC_TYPESHIFT) & ((1U << _IOC_TYPEBITS) - 1))
#define _IOC_NR(nr)         (((nr) >> _IOC_NRSHIFT) & ((1U << _IOC_NRBITS) - 1))
#define _IOC_SIZE(nr)       (((nr) >> _IOC_SIZESHIFT) & ((1U << _IOC_SIZEBITS) - 1))

#define SPI_MSGSIZE(n)      ((n) * sizeof(struct spi_ioc_transfer))
#define SPI_IOC_MESSAGE(n)  _IOC(_IOC_WRITE, SPI_IOC_MAGIC, 0, SPI_MSGSIZE(n))

struct spi_ioc_transfer {
    uint64 tx_buf;   // user-space address (NULL = receive only)
    uint64 rx_buf;   // user-space address (NULL = send only)
    uint32 len;      // total length
};

void spidev_init(void);

#endif /* _SPIDEV_H */
