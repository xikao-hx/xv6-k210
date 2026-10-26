#include "file.h"
#include "irq.h"
#include "memlayout.h"
#include "plic.h"
#include "printf.h"
#include "proc.h"
#include "sbi.h"
#include "syscall.h"
#include "trap.h"
#include "vm.h"
#include "mmap.h"
#include "signal.h"
#include "console.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

static int
trap_fault_access(uint64 scause, int *access)
{
  if(scause == 12)
    *access = VM_FAULT_EXEC;
  else if(scause == 13)
    *access = VM_FAULT_READ;
  else if(scause == 15)
    *access = VM_FAULT_WRITE;
  else
    return -1;
  return 0;
}

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
  int fault_access;
  uint64 scause = r_scause();

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(scause == 8){
    // system call

    if(p->killed)
      exit(signal_exit_status(p));

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok 
  } else if(trap_fault_access(scause, &fault_access) == 0) {
    if(vm_resolve_fault(p, p->pagetable, r_stval(), fault_access) < 0) {
      printf("usertrap(): page fault is illegal pid=%d sepc=%p stval=%p\n",
             p->pid, r_sepc(), r_stval());
      p->killed = 1;
    }
  } else {
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  console_dispatch_events();

  // Give up the CPU on a timer tick, or when a higher-priority process
  // has been woken and asked to preempt.
  if(p != 0 && p->state == RUNNING && (which_dev == 2 || mycpu()->need_resched))
    yield();

  signal_deliver(p);
  if(p->killed)
    exit(signal_exit_status(p));
    
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
kerneltrap(uint64 *regs)
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  struct proc *p = myproc();
  int fault_access;
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if(trap_fault_access(scause, &fault_access) == 0 &&
     fault_access != VM_FAULT_EXEC) {
    if(p != 0 && p->copy_fault_active) {
      if(vm_resolve_fault(p, p->pagetable,
                          r_stval(), fault_access) == VM_FAULT_RESOLVED) {
        goto handled;
      }

      // The fault is not a valid user access.  Leave the copy routine
      // through its local recovery label instead of retrying the fault.
      sepc = p->copy_fault_recovery;
      goto handled;
    }
  }

  if((which_dev = devintr()) == 0){
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

handled:

  // The idle scheduler has no process lock or device lock held.
  if(myproc() == 0)
    console_dispatch_events();

  // Give up the CPU on a timer tick, or when a higher-priority process
  // has been woken and asked to preempt.
  if(myproc() != 0 && myproc()->state == RUNNING &&
     (which_dev == 2 || mycpu()->need_resched))
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
  wakeup_reason(&ticks, WAKEUP_TIMER);
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

    // irq indicates which device interrupted; the irq registry dispatches it.
    int irq = plic_claim();
    irq_dispatch(irq);

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
