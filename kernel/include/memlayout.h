// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- UARTHS 
// 10001000 -- virtio disk 
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// ------ define irq num ------
#ifdef QEMU   // QEMU 
#define UARTHS_IRQ   10 
#define DISK_IRQ     1
#else         // K210
#define UARTHS_IRQ   33
#define DISK_IRQ     27     // DMA channel 0

#define DMAC_CH0_IRQ 27
#define DMAC_CH1_IRQ 28
#define DMAC_CH2_IRQ 29
#define DMAC_CH3_IRQ 30
#define DMAC_CH4_IRQ 31
#define DMAC_CH5_IRQ 32

#define SPI0_IRQ      1
#define SPI1_IRQ      2
#define SPI2_IRQ      4    // special
#define I2C0_IRQ      8
#define I2C1_IRQ      9
#define I2C2_IRQ      10
#define UART0_IRQ     11
#endif

// ------ device physical addresses ------
#define VIRT_OFFSET 0x3F00000000L

#define CLINT 0x02000000L
#define PLIC  0x0C000000L

// ------ define Phyaddr and VA ------
#ifdef QEMU    // QEMU 
#define UARTHS 0x10000000L
#define VIRTIO0 0x10001000L
#else          // K210
#define UARTHS      0x38000000L
#define UART0       0x50210000L
#define UART1       0x50220000L
#define UART2       0x50230000L
#define GPIOHS      0x38001000L
#define DMAC        0x50000000L
#define GPIO        0x50200000L
#define FPIOA       0x502B0000L
#define SYSCTL      0x50440000L
#define SPI0        0x52000000L
#define SPI1        0x53000000L
#define SPI2        0x54000000L
#define SPI_SLAVE   0x50240000L
#define I2C0        0x50280000L
#define I2C1        0x50290000L
#define I2C2        0x502A0000L
#endif

// ------ device virtual addresses ------
#define CLINT_V     (CLINT + VIRT_OFFSET)
#define PLIC_V      (PLIC + VIRT_OFFSET)
#define UARTHS_V    (UARTHS + VIRT_OFFSET)

#ifdef QEMU
#define VIRTIO0_V   (VIRTIO0 + VIRT_OFFSET)
#else
#define UART0_V     (UART0 + VIRT_OFFSET)
#define UART1_V     (UART1 + VIRT_OFFSET)
#define UART2_V     (UART2 + VIRT_OFFSET)
#define GPIOHS_V    (GPIOHS + VIRT_OFFSET)
#define DMAC_V      (DMAC + VIRT_OFFSET)
#define GPIO_V      (GPIO + VIRT_OFFSET)
#define FPIOA_V     (FPIOA + VIRT_OFFSET)
#define SYSCTL_V    (SYSCTL + VIRT_OFFSET)
#define SPI0_V      (SPI0 + VIRT_OFFSET)
#define SPI1_V      (SPI1 + VIRT_OFFSET)
#define SPI2_V      (SPI2 + VIRT_OFFSET)
#define SPI_SLAVE_V (SPI_SLAVE + VIRT_OFFSET)
#define I2C0_V      (I2C0 + VIRT_OFFSET)
#define I2C1_V      (I2C1 + VIRT_OFFSET)
#define I2C2_V      (I2C2 + VIRT_OFFSET)
#endif

#define CLINT_MTIMECMP(hartid) (CLINT_V + 0x4000 + 8*(hartid))
#define CLINT_MTIME (CLINT_V + 0xBFF8)

#define PLIC_PRIORITY (PLIC_V + 0x0)
#define PLIC_PENDING (PLIC_V + 0x1000)
#define PLIC_MENABLE(hart) (PLIC_V + 0x2000 + (hart)*0x100)
#define PLIC_SENABLE(hart) (PLIC_V + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC_V + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC_V + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC_V + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC_V + 0x201004 + (hart)*0x2000)

// ------ define special address ------
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

// User mappings occupy only the first two Sv39 L2 entries.
#define MAXUVA              0x80000000L
#define USER_STACK_TOP      MAXUVA
#define USER_STACK_SIZE     (1L << 20)
#define USER_STACK_BOTTOM   (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_STACK_GUARD    USER_STACK_BOTTOM
#define USER_STACK_START    (USER_STACK_GUARD + PGSIZE)

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
