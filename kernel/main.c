#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "sbi.h"
#include "disk.h"
#ifndef QEMU
#include "fpioa.h"
#include "dmac.h"
#endif

volatile static int started = 0;
volatile static int boot_leader_flag = 0;

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  int hartid = cpuid();
  int is_leader = __sync_bool_compare_and_swap(&boot_leader_flag, 0, 1);

  if(is_leader){
    consoleinit();
    printfinit();
    // print_logo();
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
#endif
    disk_init();     // initialize disk driver (virtio for QEMU, sdcard for K210)
    userinit();      // first user process

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
