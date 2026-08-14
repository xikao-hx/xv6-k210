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

/**
 * @file
 * @brief       Universal Asynchronous Receiver/Transmitter (UART)
 *
 *              The UART peripheral supports the following features:
 *
 *              - 8-N-1 and 8-N-2 formats: 8 data bits, no parity bit, 1 start
 *                bit, 1 or 2 stop bits
 *
 *              - 8-entry transmit and receive FIFO buffers with programmable
 *                watermark interrupts
 *
 *              - 16× Rx oversampling with 2/3 majority voting per bit
 *
 *              The UART peripheral does not support hardware flow control or
 *              other modem control signals, or synchronous serial data
 *              tranfesrs.
 *
 *
 */

#ifndef __UART_DW_H
#define __UART_DW_H

#include <stddef.h>
#include <stdint.h>
#include "dmac.h"
#include "plic.h"

typedef enum _uart_dev
{
    UART_DEV1 = 0,
    UART_DEV2,
    UART_DEV3,
} uart_dev_t;

typedef struct _uart {
  union { volatile uint32 RBR; volatile uint32 DLL; volatile uint32 THR; };
  union { volatile uint32 DLH; volatile uint32 IER; };
  union { volatile uint32 FCR; volatile uint32 IIR; };
  volatile uint32 LCR;
  volatile uint32 MCR;
  volatile uint32 LSR;
  volatile uint32 MSR;
  volatile uint32 SCR;
  volatile uint32 LPDLL;
  volatile uint32 LPDLH;
  volatile uint32 reserved1[2];
  volatile uint32 SRBR[16];
  volatile uint32 FAR;
  volatile uint32 TFR;
  volatile uint32 RFW;
  volatile uint32 USR;
  volatile uint32 TFL;
  volatile uint32 RFL;
  volatile uint32 SRR;
  volatile uint32 SRTS;
  volatile uint32 SBCR;
  volatile uint32 SDMAM;
  volatile uint32 SFE;
  volatile uint32 SRT;
  volatile uint32 STET;
  volatile uint32 HTX;
  volatile uint32 DMASA;
  volatile uint32 TCR;
  volatile uint32 DE_EN;
  volatile uint32 RE_EN;
  volatile uint32 DET;
  volatile uint32 TAT;
  volatile uint32 DLF;
  volatile uint32 RAR;
  volatile uint32 TAR;
  volatile uint32 LCR_EXT;
} uart_t;

typedef enum _uart_device_number
{
    UART_DEVICE_1,
    UART_DEVICE_2,
    UART_DEVICE_3,
    UART_DEVICE_MAX,
} uart_device_number_t;

typedef enum _uart_bitwidth
{
    UART_BITWIDTH_5BIT = 5,
    UART_BITWIDTH_6BIT,
    UART_BITWIDTH_7BIT,
    UART_BITWIDTH_8BIT,
} uart_bitwidth_t;

typedef enum _uart_stopbit
{
    UART_STOP_1,
    UART_STOP_1_5,
    UART_STOP_2
} uart_stopbit_t;

typedef enum _uart_rede_sel
{
    DISABLE = 0,
    ENABLE,
} uart_rede_sel_t;

typedef enum _uart_parity
{
    UART_PARITY_NONE,
    UART_PARITY_ODD,
    UART_PARITY_EVEN
} uart_parity_t;

typedef enum _uart_interrupt_mode
{
    UART_SEND = 1,
    UART_RECEIVE = 2,
} uart_interrupt_mode_t;

typedef enum _uart_send_trigger
{
    UART_SEND_FIFO_0,
    UART_SEND_FIFO_2,
    UART_SEND_FIFO_4,
    UART_SEND_FIFO_8,
} uart_send_trigger_t;

typedef enum _uart_receive_trigger
{
    UART_RECEIVE_FIFO_1,
    UART_RECEIVE_FIFO_4,
    UART_RECEIVE_FIFO_8,
    UART_RECEIVE_FIFO_14,
} uart_receive_trigger_t;

typedef struct _uart_data_t
{
    dmac_channel_number_t tx_channel;
    dmac_channel_number_t rx_channel;
    uint32_t *tx_buf;
    size_t tx_len;
    uint32_t *rx_buf;
    size_t rx_len;
    uart_interrupt_mode_t transfer_mode;
} uart_data_t;

typedef struct _uart_tcr
{
    uint32_t rs485_en : 1;
    uint32_t re_pol : 1;
    uint32_t de_pol : 1;
    uint32_t xfer_mode : 2;
    uint32_t reserve : 27;
} uart_tcr_t;

typedef enum _uart_work_mode
{
    UART_NORMAL,
    UART_IRDA,
    UART_RS485_FULL_DUPLEX,
    UART_RS485_HALF_DUPLEX,
} uart_work_mode_t;

typedef enum _uart_rs485_rede
{
    UART_RS485_DE,
    UART_RS485_RE,
    UART_RS485_REDE,
} uart_rs485_rede_t;

typedef enum _uart_polarity
{
    UART_LOW,
    UART_HIGH,
} uart_polarity_t;

typedef enum _uart_det_mode
{
    UART_DE_ASSERTION,
    UART_DE_DE_ASSERTION,
    UART_DE_ALL,
} uart_det_mode_t;

typedef struct _uart_det
{
    uint32_t de_assertion_time : 8;
    uint32_t reserve0 : 8;
    uint32_t de_de_assertion_time : 8;
    uint32_t reserve1 : 8;
} uart_det_t;

typedef enum _uart_tat_mode
{
    UART_DE_TO_RE,
    UART_RE_TO_DE,
    UART_TAT_ALL,
} uart_tat_mode_t;

typedef struct _uart_tat
{
    uint32_t de_to_re : 16;
    uint32_t re_to_de : 16;
} uart_tat_t;


#endif /* __UART_DW_H */
