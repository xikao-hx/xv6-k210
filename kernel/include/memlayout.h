// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0 
// 10001000 -- virtio disk 
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

#ifdef QEMU
// qemu puts UART registers here in physical memory.
#define UART0 0x10000000L
// virtio mmio interface
#define VIRTIO0 0x10001000
#endif

#ifdef QEMU     // QEMU 
#define UART0_IRQ   10 
#define DISK_IRQ    1
#else           // k210
#define UART0_IRQ   33
#define UART_IRQ    11
#define DISK_IRQ    27
// DMAC channel completion -> PLIC source 27+n (SDK plic.h: DMA0=27 .. DMA5=32).
// CH5 (UART RX DMA) harvests via its own handler (must not be overridden);
// CH4 (UART TX DMA), CH2/CH3 (I2C0 TX/RX DMA, i2c_board.c) and CH0/CH1
// (SPI0 DMA) are dispatched interrupt-driven via dmac_intr().
#define DMAC_CH2_IRQ 29
#define DMAC_CH3_IRQ 30
#define DMAC_CH4_IRQ 31
#define DMAC_CH5_IRQ 32
// DW SPI / DW I2C PLIC sources (SDK plic.h: SPI0=1, SPI1=2, SPI_SLAVE=3,
// SPI3=4, I2C0=8, I2C1=9, I2C2=10).  The K210 SPI base-address table
// (spi.c) is ordered SPI0/SPI1/SPI_SLAVE/SPI2, so SPI0_IRQ+num and
// I2C0_IRQ+num derive each instance's source by index.
#define SPI0_IRQ 1
#define SPI1_IRQ 2
#define SPI_SLAVE_IRQ 3
#define SPI2_IRQ 4
#define I2C0_IRQ 8
#define I2C1_IRQ 9
#define I2C2_IRQ 10
#endif

// local interrupt controller, which contains the timer.
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid))
#define CLINT_MTIME (CLINT + 0xBFF8) // cycles since boot.

// qemu puts programmable interrupt controller here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart)*0x100)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// K210 peripheral base addresses (physical, identity-mapped)
#ifndef QEMU
#define UART0       0x38000000
#define UART        0x50210000
#define UART1       0x50210000
#define UART2       0x50220000
#define UART3       0x50230000
#define GPIOHS      0x38001000
#define DMAC        0x50000000
#define GPIO        0x50200000
#define SPI_SLAVE   0x50240000
#define FPIOA       0x502B0000
#define SYSCTL      0x50440000
#define SPI0        0x52000000
#define SPI1        0x53000000
#define SPI2        0x54000000
#define I2C0        0x50280000
#define I2C1        0x50290000
#define I2C2        0x502A0000

// Virtual addresses (identity-mapped on os/ tree)
#define UART_V      UART
#define UART1_V     UART1
#define UART2_V     UART2
#define UART3_V     UART3
#define GPIOHS_V    GPIOHS
#define DMAC_V      DMAC
#define GPIO_V      GPIO
#define SPI_SLAVE_V SPI_SLAVE
#define FPIOA_V     FPIOA
#define SYSCTL_V    SYSCTL
#define SPI0_V      SPI0
#define SPI1_V      SPI1
#define SPI2_V      SPI2
#define I2C0_V      I2C0
#define I2C1_V      I2C1
#define I2C2_V      I2C2
#endif

#define UART0_V     UART0

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#ifndef QEMU
#define KERNBASE 0x80020000L
#else
#define KERNBASE 0x80200000L
#endif
#define PHYSTOP 0x80600000L
// #define PHYSTOP (KERNBASE + 128*1024*1024)

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
#define SIGTRAMP  (TRAPFRAME - PGSIZE)
