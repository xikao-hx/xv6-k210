/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Damien Le Moal <damien.lemoal@wdc.com>
 */
#ifndef _K210_PLATFORM_H_
#define _K210_PLATFORM_H_

#include <sbi/riscv_io.h>

#define K210_HART_COUNT		2

#define K210_UART_BAUDRATE	115200

#define K210_CLK0_FREQ		26000000UL
#define K210_PLIC_NUM_SOURCES	65

/* Registers base address */
#define K210_SYSCTL_BASE_ADDR	0x50440000ULL
#define K210_UART_BASE_ADDR	0x38000000ULL
#define K210_CLINT_BASE_ADDR	0x02000000ULL
#define K210_PLIC_BASE_ADDR	0x0C000000ULL

/* Registers */
#define K210_PLL0		0x08
#define K210_CLKSEL0		0x20

/* SYSCTL clock enable registers */
#define K210_CLK_EN_CENT	0x28
#define K210_CLK_EN_PERI	0x2C

#define K210_CLK_EN_CENT_APB0	(1 << 3)
#define K210_CLK_EN_PERI_FPIOA	(1 << 20)

/* FPIOA base address */
#define K210_FPIOA_BASE_ADDR	0x502B0000ULL

/* FPIOA function codes */
#define K210_FUNC_UARTHS_RX	18
#define K210_FUNC_UARTHS_TX	19

/* FPIOA IO register bit offsets */
#define FPIOA_CH_SEL_SHIFT	0
#define FPIOA_CH_SEL_MASK	0xFF
#define FPIOA_DS_SHIFT		8
#define FPIOA_DS_MASK		0xF
#define FPIOA_OE_EN		(1 << 12)
#define FPIOA_OE_INV		(1 << 13)
#define FPIOA_DO_SEL		(1 << 14)
#define FPIOA_DO_INV		(1 << 15)
#define FPIOA_PU		(1 << 16)
#define FPIOA_PD		(1 << 17)
#define FPIOA_SL		(1 << 19)
#define FPIOA_IE_EN		(1 << 20)
#define FPIOA_IE_INV		(1 << 21)
#define FPIOA_DI_INV		(1 << 22)
#define FPIOA_ST		(1 << 23)

#define FPIOA_IO_CFG(ch, ds, flags) \
	(((ch) & FPIOA_CH_SEL_MASK) | \
	 (((ds) & FPIOA_DS_MASK) << FPIOA_DS_SHIFT) | \
	 (flags))

static inline u32 k210_read_sysreg(u32 reg)
{
	return readl((volatile void *)(K210_SYSCTL_BASE_ADDR + reg));
}

#endif /* _K210_PLATFORM_H_ */
