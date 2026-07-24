#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "riscv.h"
#include "signal.h"
#include "string.h"
#include "vm.h"

extern struct proc proc[NPROC];

static int
signal_valid(int signum)
{
  return signum > 0 && signum < NSIG;
}

static uint32
signal_bit(int signum)
{
  return 1U << signum;
}

static int
signal_handler_valid(struct proc *p, uint64 handler)
{
  pte_t *pte;

  if(handler >= MAXVA)
    return 0;
  pte = walk(p->pagetable, handler, 0);
  if(pte == 0)
    return 0;
  return (*pte & (PTE_V | PTE_U | PTE_X)) == (PTE_V | PTE_U | PTE_X);
}

void
signal_proc_init(struct proc *p)
{
  p->sig_pending = 0;
  p->sig_mask = 0;
  memset(p->sig_handlers, 0, sizeof(p->sig_handlers));
  p->sig_handling = 0;
  p->sig_current = 0;
  p->sig_term = 0;
  memset(&p->sig_saved_trapframe, 0, sizeof(p->sig_saved_trapframe));
}

void
signal_proc_fork(struct proc *parent, struct proc *child)
{
  child->sig_pending = 0;
  child->sig_mask = parent->sig_mask;
  memmove(child->sig_handlers, parent->sig_handlers,
          sizeof(child->sig_handlers));
  child->sig_handling = 0;
  child->sig_current = 0;
  child->sig_term = 0;
  memset(&child->sig_saved_trapframe, 0,
         sizeof(child->sig_saved_trapframe));
}

void
signal_reset_on_exec(struct proc *p)
{
  acquire(&p->lock);
  for(int signum = 1; signum < NSIG; signum++) {
    if(p->sig_handlers[signum] != (uint64)SIG_IGN)
      p->sig_handlers[signum] = (uint64)SIG_DFL;
  }
  p->sig_pending = 0;
  p->sig_mask = 0;
  p->sig_handling = 0;
  p->sig_current = 0;
  p->sig_term = 0;
  memset(&p->sig_saved_trapframe, 0,
         sizeof(p->sig_saved_trapframe));
  release(&p->lock);
}

uint64
signal_set_handler(struct proc *p, int signum, uint64 handler)
{
  uint64 old;

  if(!signal_valid(signum))
    return (uint64)SIG_ERR;
  if(signum == SIGKILL)
    return (uint64)SIG_ERR;
  if(handler != (uint64)SIG_DFL && handler != (uint64)SIG_IGN &&
     !signal_handler_valid(p, handler))
    return (uint64)SIG_ERR;

  acquire(&p->lock);
  old = p->sig_handlers[signum];
  p->sig_handlers[signum] = handler;
  release(&p->lock);
  return old;
}

int
signal_send_pid(int pid, int signum)
{
  struct proc *p;

  if(pid <= 0 || !signal_valid(signum))
    return -1;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state != UNUSED && p->pid == pid) {
      p->sig_pending |= signal_bit(signum);
      if(signum == SIGKILL) {
        p->sig_term = signum;
        p->killed = 1;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

static int
signal_choose(uint32 pending)
{
  if(pending & signal_bit(SIGKILL))
    return SIGKILL;
  for(int signum = 1; signum < NSIG; signum++) {
    if(pending & signal_bit(signum))
      return signum;
  }
  return 0;
}

static int
signal_default_ignored(int signum)
{
  return signum == SIGCHLD;
}

void
signal_deliver(struct proc *p)
{
  uint32 pending;
  uint64 handler;
  int signum;

  acquire(&p->lock);
  pending = p->sig_pending & ~p->sig_mask;
  if(p->sig_pending & signal_bit(SIGKILL))
    pending |= signal_bit(SIGKILL);
  if(p->sig_handling && !(pending & signal_bit(SIGKILL))) {
    release(&p->lock);
    return;
  }

  signum = signal_choose(pending);
  if(signum == 0) {
    release(&p->lock);
    return;
  }

  p->sig_pending &= ~signal_bit(signum);
  handler = p->sig_handlers[signum];

  if(signum == SIGKILL ||
     (handler == (uint64)SIG_DFL && !signal_default_ignored(signum))) {
    p->sig_term = signum;
    p->killed = 1;
    release(&p->lock);
    return;
  }

  if(handler == (uint64)SIG_IGN ||
     (handler == (uint64)SIG_DFL && signal_default_ignored(signum))) {
    release(&p->lock);
    return;
  }

  if(!signal_handler_valid(p, handler)) {
    p->sig_term = signum;
    p->killed = 1;
    release(&p->lock);
    return;
  }

  p->sig_saved_trapframe = *p->trapframe;
  p->sig_handling = 1;
  p->sig_current = signum;
  p->sig_mask |= signal_bit(signum);
  p->trapframe->a0 = signum;
  p->trapframe->ra = SIGTRAMP;
  p->trapframe->epc = handler;
  release(&p->lock);
}

uint64
signal_sigreturn(struct proc *p)
{
  uint64 a0;
  int signum;

  acquire(&p->lock);
  if(!p->sig_handling) {
    release(&p->lock);
    return -1;
  }

  signum = p->sig_current;
  a0 = p->sig_saved_trapframe.a0;
  *p->trapframe = p->sig_saved_trapframe;
  if(signal_valid(signum))
    p->sig_mask &= ~signal_bit(signum);
  p->sig_handling = 0;
  p->sig_current = 0;
  memset(&p->sig_saved_trapframe, 0,
         sizeof(p->sig_saved_trapframe));
  release(&p->lock);
  return a0;
}

int
signal_exit_status(struct proc *p)
{
  int signum;

  acquire(&p->lock);
  signum = p->sig_term;
  release(&p->lock);
  return signum ? -signum : -1;
}
