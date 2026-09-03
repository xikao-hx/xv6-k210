#include "buf.h"
#include "console.h"
#include "disk.h"
#include "file.h"
#include "kalloc.h"
#include "plic.h"
#include "printf.h"
#include "proc.h"
#include "stats.h"
#include "sbi.h"
#include "trap.h"
#include "vm.h"
#include "kbufdev.h"
#include "eagerdev.h"
#ifndef QEMU
#include "oledfb.h"
#include "dmac.h"
#include "fpioa.h"
#include "i2cdev.h"
#include "sdcarddev.h"
#include "spidev.h"
#include "uartdev.h"
#endif

static inline void inithartid(unsigned long hartid) {
  asm volatile("mv tp, %0" : : "r" (hartid & 0x1));
}

volatile static int started = 0;

void
main(unsigned long hartid, unsigned long dtb_pa)
{
  inithartid(hartid);
  
  if (hartid == 0) {
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    plicinithart();  // clear all device interrupts
    consoleinit();
    printfinit();
    print_logo();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
#ifndef QEMU
    sbi_set_mie();   // enable M-mode external interrupts (RustSBI disables by default)
#endif
    binit();         // buffer cache
    fileinit();      // file table
    statsinit();     // register stats device
    kbufdev_init();  // register page-backed mmap test device
    eagerdev_init(); // register eager-mapped mmap test device
#ifndef QEMU
    fpioa_pin_init(); // configure SPI0 pins for SD card
    dmac_init();      // initialize DMA controller
    spidev_init();   // register SPI device for user-space access
    i2cdev_init();   // register I2C device for user-space access
    sdcarddev_init(); // register SD card device for user-space access
    oledfbdev_init(); // register oledfb device for user-space access
    uartdev_init();  // register /dev/ttyS0 (DW UART1, DMA-capable)
#endif
    disk_init();     // initialize disk driver (virtio for QEMU, sdcard for K210)
    userinit();      // first user process

    /* workaround: wait some time, k210 need this to boot success */
    printf("hart 0 init done\n");
    
    // Start secondary harts via SBI HSM
    for(int i = 0; i < NCPU; i++) {
      if(i == hartid)
        continue;
      unsigned long mask = 1 << i;
      sbi_send_ipi(&mask);
    }

    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    printf("hart %d starting\n", hartid);
  }

  scheduler();
}
