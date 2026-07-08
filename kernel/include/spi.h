/* Copyright 2018 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef _SPI_H
#define _SPI_H

#include "types.h"
#include "dmac.h"
#include "stdbool.h"
#include "memlayout.h"

/* clang-format off */
typedef struct _spi
{
    /* SPI Control Register 0                                    (0x00)*/
    volatile uint32 ctrlr0;
    /* SPI Control Register 1                                    (0x04)*/
    volatile uint32 ctrlr1;
    /* SPI Enable Register                                       (0x08)*/
    volatile uint32 ssienr;
    /* SPI Microwire Control Register                            (0x0c)*/
    volatile uint32 mwcr;
    /* SPI Slave Enable Register                                 (0x10)*/
    volatile uint32 ser;
    /* SPI Baud Rate Select                                      (0x14)*/
    volatile uint32 baudr;
    /* SPI Transmit FIFO Threshold Level                         (0x18)*/
    volatile uint32 txftlr;
    /* SPI Receive FIFO Threshold Level                          (0x1c)*/
    volatile uint32 rxftlr;
    /* SPI Transmit FIFO Level Register                          (0x20)*/
    volatile uint32 txflr;
    /* SPI Receive FIFO Level Register                           (0x24)*/
    volatile uint32 rxflr;
    /* SPI Status Register                                       (0x28)*/
    volatile uint32 sr;
    /* SPI Interrupt Mask Register                               (0x2c)*/
    volatile uint32 imr;
    /* SPI Interrupt Status Register                             (0x30)*/
    volatile uint32 isr;
    /* SPI Raw Interrupt Status Register                         (0x34)*/
    volatile uint32 risr;
    /* SPI Transmit FIFO Overflow Interrupt Clear Register       (0x38)*/
    volatile uint32 txoicr;
    /* SPI Receive FIFO Overflow Interrupt Clear Register        (0x3c)*/
    volatile uint32 rxoicr;
    /* SPI Receive FIFO Underflow Interrupt Clear Register       (0x40)*/
    volatile uint32 rxuicr;
    /* SPI Multi-Master Interrupt Clear Register                 (0x44)*/
    volatile uint32 msticr;
    /* SPI Interrupt Clear Register                              (0x48)*/
    volatile uint32 icr;
    /* SPI DMA Control Register                                  (0x4c)*/
    volatile uint32 dmacr;
    /* SPI DMA Transmit Data Level                               (0x50)*/
    volatile uint32 dmatdlr;
    /* SPI DMA Receive Data Level                                (0x54)*/
    volatile uint32 dmardlr;
    /* SPI Identification Register                               (0x58)*/
    volatile uint32 idr;
    /* SPI DWC_ssi component version                             (0x5c)*/
    volatile uint32 ssic_version_id;
    /* SPI Data Register 0-36                                    (0x60 -- 0xec)*/
    volatile uint32 dr[36];
    /* SPI RX Sample Delay Register                              (0xf0)*/
    volatile uint32 rx_sample_delay;
    /* SPI SPI Control Register                                  (0xf4)*/
    volatile uint32 spi_ctrlr0;
    /* reserved                                                  (0xf8)*/
    volatile uint32 resv;
    /* SPI XIP Mode bits                                         (0xfc)*/
    volatile uint32 xip_mode_bits;
    /* SPI XIP INCR transfer opcode                              (0x100)*/
    volatile uint32 xip_incr_inst;
    /* SPI XIP WRAP transfer opcode                              (0x104)*/
    volatile uint32 xip_wrap_inst;
    /* SPI XIP Control Register                                  (0x108)*/
    volatile uint32 xip_ctrl;
    /* SPI XIP Slave Enable Register                             (0x10c)*/
    volatile uint32 xip_ser;
    /* SPI XIP Receive FIFO Overflow Interrupt Clear Register    (0x110)*/
    volatile uint32 xrxoicr;
    /* SPI XIP time out register for continuous transfers        (0x114)*/
    volatile uint32 xip_cnt_time_out;
    volatile uint32 endian;
} __attribute__((packed, aligned(4))) spi_t;
/* clang-format on */

typedef enum _spi_device_num
{
    SPI_DEVICE_0,
    SPI_DEVICE_1,
    SPI_DEVICE_2,
    SPI_DEVICE_3,
    SPI_DEVICE_MAX,
} spi_device_num_t;

typedef enum _spi_work_mode
{
    SPI_WORK_MODE_0,
    SPI_WORK_MODE_1,
    SPI_WORK_MODE_2,
    SPI_WORK_MODE_3,
} spi_work_mode_t;

typedef enum _spi_frame_format
{
    SPI_FF_STANDARD,
    SPI_FF_DUAL,
    SPI_FF_QUAD,
    SPI_FF_OCTAL
} spi_frame_format_t;

typedef enum _spi_instruction_address_trans_mode
{
    SPI_AITM_STANDARD,
    SPI_AITM_ADDR_STANDARD,
    SPI_AITM_AS_FRAME_FORMAT
} spi_instruction_address_trans_mode_t;

typedef enum _spi_transfer_mode
{
    SPI_TMOD_TRANS_RECV,
    SPI_TMOD_TRANS,
    SPI_TMOD_RECV,
    SPI_TMOD_EEROM
} spi_transfer_mode_t;

typedef enum _spi_transfer_width
{
    SPI_TRANS_CHAR = 0x1,
    SPI_TRANS_SHORT = 0x2,
    SPI_TRANS_INT = 0x4,
} spi_transfer_width_t;

typedef enum _spi_chip_select
{
    SPI_CHIP_SELECT_0,
    SPI_CHIP_SELECT_1,
    SPI_CHIP_SELECT_2,
    SPI_CHIP_SELECT_3,
    SPI_CHIP_SELECT_MAX,
} spi_chip_select_t;

struct spi_transfer {
    const uint8 *tx_buf;
    uint8 *rx_buf;
    uint32 len;
};

struct spi_dw_data {
    uint8 index;
    void *rx_buf;
    const void *tx_buf;
    unsigned int count;
    unsigned int bytes_per_word;
    dmac_channel_number_t chan_tx;
    dmac_channel_number_t chan_rx;
    spi_chip_select_t chip_select;
    bool dma_enable;
};

void spi_init(spi_device_num_t spi_num, spi_work_mode_t work_mode, spi_frame_format_t frame_format,
              uint64 data_bit_length, uint32 endian);
int spi_transfer(spi_device_num_t spi_num, spi_chip_select_t chip_select, 
                    struct spi_transfer *transfer, uint64 num);
int spi_write(spi_device_num_t spi_num, spi_chip_select_t chip_select,
                const void *buf, uint64 len);
int spi_read(spi_device_num_t spi_num, spi_chip_select_t chip_select,
                void *buf, uint64 len);

#endif /* _DRIVER_SPI_H */
