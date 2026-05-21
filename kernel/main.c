#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "sbi.h"
#include "disk.h"
#include "sysctl.h"
#ifndef QEMU
#include "fpioa.h"
#include "dmac.h"
#endif
#include "i2cdev.h"

static inline void inithartid(unsigned long hartid) {
  asm volatile("mv tp, %0" : : "r" (hartid & 0x1));
}

volatile static int started = 0;

void
main(unsigned long hartid, unsigned long dtb_pa)
{
  inithartid(hartid);
  
  if (hartid == 0) {
    consoleinit();
    printfinit();
    print_logo();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    binit();         // buffer cache
    fileinit();      // file table
#ifndef QEMU
    fpioa_pin_init(); // configure SPI0 pins for SD card
    dmac_init();      // initialize DMA controller
    spidev_init();   // register SPI device for user-space access
    i2cdev_init();   // register I2C device for user-space access
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
    printf("hart %d starting\n", hartid);
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }

  scheduler();
}
