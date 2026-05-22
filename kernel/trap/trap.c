#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sbi.h"
#include "disk.h"
#include "plic.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
  w_sstatus(r_sstatus() | SSTATUS_SIE);
  // enable supervisor-mode timer interrupts.
  w_sie(r_sie() | SIE_SEIE | SIE_SSIE | SIE_STIE);
  
  // set the first timer interrupt via SBI
  sbi_set_timer(r_time() + INTERVAL);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
	} else if (r_scause() == 13 || r_scause() == 15
	#ifndef QEMU
	             || r_scause() == 5 || r_scause() == 7
	#endif
	) {
    struct proc *p = myproc();
    uint64 va = r_stval();
    pagetable_t pagetable = p->pagetable;

    if (find_vma(p, va) == 0) {
      if (mmap_handler(va, r_scause()) != 0) {
        p->killed = 1;
      }
    } else
    {
      if (va < p->sz) {
        if (uvmcowpage(pagetable, va) == 0) {
          if (uvmcowmalloc(pagetable, PGROUNDDOWN(va)) == 0) {
            p->killed = 1;
          }
        } else if (PGROUNDUP(p->trapframe->sp) - 1 < va) {
          if (uvmlazymalloc(pagetable, PGROUNDDOWN(va)) != 0) {
            p->killed = 1;
          }
        }
      } else {
        printf("usertrap(): addr is illegal\n");
        p->killed = 1;
      }
    }
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt (unless no_preempt).
  if(which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING && !myproc()->no_preempt)
    yield();

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
  // set next timer interrupt via SBI
  sbi_set_timer(r_time() + INTERVAL);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();
  // printf("scause: %d\n", scause);
  #ifdef QEMU 
	// handle external interrupt 
	if ((0x8000000000000000L & scause) && 9 == (scause & 0xff)) 
	#else 
	// on k210, supervisor software interrupt is used 
	// in alternative to supervisor external interrupt, 
	// which is not available on k210. 
	if (0x8000000000000001L == scause && 9 == r_stval()) 
	#endif 
  {
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    // ======= 添加以下调试打印 =======
    // printf("DEBUG: Received external IRQ: %d\n", irq);
    // ===============================

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == DISK_IRQ){
      disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

#ifndef QEMU 
    // On K210 (v1.9.1), external interrupts are forwarded from M-mode.
    // Re-enable M-mode external interrupts before returning.
		w_sip(r_sip() & ~2);    // clear pending bit
		sbi_set_mie();
#endif 

    return 1;
  } else if(scause == 0x8000000000000005L){
    // S-mode timer interrupt from OpenSBI
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

