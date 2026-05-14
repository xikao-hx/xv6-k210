#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "sbi.h"
#include "disk.h"

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
#ifdef LAB_LOCK
    statsinit();
#endif
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging

    // enable S-mode global interrupts
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1)); // SIE = 1
    asm volatile("csrs sie, %0" : : "r"(1 << 9));     // SEIE = 1
    asm volatile("csrs sie, %0" : : "r"(1 << 5));     // STIE = 1

    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    binit();         // buffer cache
    fileinit();      // file table
    disk_init();     // initialize disk driver (virtio for QEMU, sdcard for K210)
    userinit();      // first user process

    // Start secondary harts via SBI HSM
    for(int i = 0; i < NCPU; i++) {
      if(i == hartid)
        continue;
#ifdef QEMU
      int rc = sbi_hart_start(i, 0x80200000, i);
#else
      int rc = sbi_hart_start(i, 0x80020000, i);
#endif
      if(rc != 0)
        printf("failed to start hart %d: error %d\n", i, rc);
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

    // enable S-mode global interrupts
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1)); // SIE = 1
    asm volatile("csrs sie, %0" : : "r"(1 << 9));     // SEIE = 1
    asm volatile("csrs sie, %0" : : "r"(1 << 5));     // STIE = 1
  }

  // set the first timer interrupt via SBI
  sbi_set_timer(r_time() + 1000000);
  scheduler();
}
