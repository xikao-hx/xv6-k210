/* Board-level SPI definitions — controller instances, device objects.
 *
 * The generic DW SPI driver (kernel/driver/spi.c) accesses controllers
 * via the extern table spi_ctrls[] declared in board_spi.h.  Device
 * descriptors are returned by spi_device_get().
 */
#include "spi_board.h"
#include "dev.h"
#include "printf.h"
#include "sleeplock.h"
#include "dmac.h"

/* ------------------------------------------------------------------ */
/*  Controller instances                                              */
/* ------------------------------------------------------------------ */

/* SPI1: DMA channels 0 (TX) / 1 (RX) */
static struct spi_controller spi_ctrl_0 = {
    .spi_data = {
        .chan_tx = DMAC_CHANNEL0,
        .chan_rx = DMAC_CHANNEL1,
    },
};

/* SPI1: DMA channels 4 (TX) / 5 (RX) */
static struct spi_controller spi_ctrl_1 = {
    .spi_data = {
        .chan_tx = DMAC_CHANNEL4,
        .chan_rx = DMAC_CHANNEL5,
    },
};

/* Public controller table — the generic driver indexes this by bus number */
struct spi_controller *spi_ctrls[SPI_DEVICE_MAX] = {
    [SPI_DEVICE_0] = &spi_ctrl_0,
    [SPI_DEVICE_1] = &spi_ctrl_1,
};

/* ------------------------------------------------------------------ */
/*  Device descriptors (static const — never modified after boot)     */
/* ------------------------------------------------------------------ */

static struct spi_device spi_w25q64_dev = {
    .bus_num = SPI_DEVICE_1,
    .chip_select = SPI_CHIP_SELECT_0,
    .cs_gpio = W25Q64_SELECT,
    .max_speed_hz = 10000000,
    .mode = SPI_WORK_MODE_0,
    .bits_per_word = 8,
};

static struct spi_device spi_sd_dev = {
	.bus_num = SPI_DEVICE_0,
    .chip_select = SPI_CHIP_SELECT_3,
	.max_speed_hz = 10000000,
	.mode = SPI_WORK_MODE_0,
	.bits_per_word = 8,
};

static struct spi_device *spi_devices[SPI_CHIP_SELECT_MAX] = {
    [SPI_DEV_W25Q64] = &spi_w25q64_dev,
    [SPI_DEV_SDCARD] = &spi_sd_dev,
};

struct spi_device *
spi_device_get(int minor)
{
    if(minor < 0 || minor >= (int)(sizeof(spi_devices) / sizeof(spi_devices[0])))
        return 0;
    return spi_devices[minor];
}
