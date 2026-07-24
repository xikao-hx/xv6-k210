#include "file.h"
#include "fat32.h"
#include "kalloc.h"
#include "memlayout.h"
#include "printf.h"
#include "fcntl.h"
#include "proc.h"
#include "signal.h"
#include "string.h"
#include "trap.h"
#include "vm.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void wakeup1(struct proc *chan);
static void freeproc(struct proc *p);

#ifdef SCHED_MLFQ
// Logical MLFQ queues are represented by queue_level in proc[].
// Each level keeps its own scan cursor to provide RR fairness within that level.
static int mlfq_rr_next[MLFQ_LEVELS];
static uint mlfq_last_boost_tick;
static int mlfq_time_slices[MLFQ_LEVELS] = { 1, 2, 4, 8 };
#else
static int rr_next;
#endif

extern char trampoline[]; // trampoline.S
extern char sigtramp[];   // sigtramp.S

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  memset(cpus, 0, sizeof(cpus));
  memset(proc, 0, sizeof(proc));
#ifdef SCHED_MLFQ
  memset(mlfq_rr_next, 0, sizeof(mlfq_rr_next));
  mlfq_last_boost_tick = 0;
#else
  rr_next = 0;
#endif

  initlock(&pid_lock, "nextpid");
  for(p = proc; p < &proc[NPROC]; p++) {
    p->state = UNUSED;
    p->queue_level = MLFQ_TOP_LEVEL;
    p->last_wakeup_reason = WAKEUP_NORMAL;
    initlock(&p->lock, "proc");
  }
  kvminithart();
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  signal_proc_init(p);
  p->pgid = p->pid;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc_page()) == 0){
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user kernel's page table
  p->kpagetable = ukvminit();
  if(p->kpagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Allocate a page for the process's kernel stack.
  // Map it high in memory, followed by an invalid
  // guard page.
  char *pa = kalloc_page();
  if(pa == 0)
    panic("kalloc");
  uint64 va = KSTACK((int) 0);
  ukvmmap(p->kpagetable, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  p->kstack = va;

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  memset(&p->vmas, 0, sizeof(p->vmas));
  // New processes start interactive: highest priority and empty counters.
  p->queue_level = MLFQ_TOP_LEVEL;
  p->sched_ticks = 0;
  p->total_run_ticks = 0;
  p->wait_ticks = 0;
  p->preempt_count = 0;
  p->boost_count = 0;
  p->io_wakeup_count = 0;
  p->device_wakeup_count = 0;
  p->last_wakeup_reason = WAKEUP_NORMAL;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  vma_destroy_all(p);
  if(p->trapframe)
    kfree_page((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  if (p->kpagetable)
    proc_freekpagetable(p->kpagetable, p->kstack, p->sz);

  p->kpagetable = 0;
  p->kstack = 0;
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->pgid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  signal_proc_init(p);
  p->xstate = 0;
  p->queue_level = MLFQ_TOP_LEVEL;
  p->sched_ticks = 0;
  p->total_run_ticks = 0;
  p->wait_ticks = 0;
  p->preempt_count = 0;
  p->boost_count = 0;
  p->io_wakeup_count = 0;
  p->device_wakeup_count = 0;
  p->last_wakeup_reason = WAKEUP_NORMAL;
  p->state = UNUSED;
}

int 
proc_num(void)
{
  struct proc *p;
  int num = 0;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p->state != UNUSED) {
      num ++;
    } 
  }
  return num;
}

// Account how long runnable processes have waited without being selected.
static void
account_runnable_wait(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == RUNNABLE)
      p->wait_ticks++;
    release(&p->lock);
  }
}

#ifdef SCHED_MLFQ
// Periodically move all live processes back to the top MLFQ level.
static void
mlfq_boost_if_needed(void)
{
  struct proc *p;

  if(ticks - mlfq_last_boost_tick < MLFQ_BOOST_INTERVAL)
    return;

  mlfq_last_boost_tick = ticks;
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state != UNUSED) {
      p->queue_level = MLFQ_TOP_LEVEL;
      p->sched_ticks = 0;
      p->wait_ticks = 0;
      p->boost_count++;
    }
    release(&p->lock);
  }
}

// Promote IO/device-woken processes so interactive work responds quickly.
static void
mlfq_promote_on_wakeup(struct proc *p, int reason)
{
  if(reason == WAKEUP_IO || reason == WAKEUP_DEVICE) {
    if(p->queue_level > MLFQ_TOP_LEVEL)
      p->queue_level--;
    p->sched_ticks = 0;
    if(reason == WAKEUP_IO)
      p->io_wakeup_count++;
    else
      p->device_wakeup_count++;
  }
}
#endif

// Update scheduling statistics and MLFQ level after a timer yield.
static void
sched_on_yield(struct proc *p)
{
  p->total_run_ticks++;
  p->preempt_count++;

#ifdef SCHED_MLFQ
  p->sched_ticks++;
  // Processes that use a whole slice are treated as more CPU-bound and demoted.
  if(p->sched_ticks >= mlfq_time_slices[p->queue_level]) {
    if(p->queue_level < MLFQ_BOTTOM_LEVEL)
      p->queue_level++;
    p->sched_ticks = 0;
  }
#endif
}

#ifdef SCHED_MLFQ
// Pick the next runnable process using priority levels and per-level RR.
static struct proc*
pick_next_proc_mlfq(void)
{
  struct proc *p;
  int level;
  int i;

  mlfq_boost_if_needed();

  // Scan higher priority levels first; each level uses its own RR cursor.
  for(level = MLFQ_TOP_LEVEL; level <= MLFQ_BOTTOM_LEVEL; level++) {
    for(i = 0; i < NPROC; i++) {
      // The cursor is a scan start in proc[], not necessarily a process in this level.
      int idx = (mlfq_rr_next[level] + i) % NPROC;
      p = &proc[idx];
      acquire(&p->lock);
      if(p->state == RUNNABLE && p->queue_level == level) {
        mlfq_rr_next[level] = (idx + 1) % NPROC;
        p->wait_ticks = 0;
        // Return with p->lock held; run_proc() preserves xv6 scheduler locking.
        return p;
      }
      release(&p->lock);
    }
  }
  return 0;
}
#else
// Pick the next runnable process using a global round-robin cursor.
static struct proc*
pick_next_proc_rr(void)
{
  struct proc *p;
  int i;

  for(i = 0; i < NPROC; i++) {
    int idx = (rr_next + i) % NPROC;
    p = &proc[idx];
    acquire(&p->lock);
    if(p->state == RUNNABLE) {
      rr_next = (idx + 1) % NPROC;
      p->wait_ticks = 0;
      // Return with p->lock held to match the scheduler/run_proc contract.
      return p;
    }
    release(&p->lock);
  }
  return 0;
}
#endif

// Dispatch to the scheduler implementation selected at build time.
static struct proc*
pick_next_proc(void)
{
#ifdef SCHED_MLFQ
  return pick_next_proc_mlfq();
#else
  return pick_next_proc_rr();
#endif
}

// Run a selected process through the common xv6 context-switch path.
static void
run_proc(struct cpu *c, struct proc *p)
{
  // Common context-switch path; the selection policy is kept in pick_next_proc().
  p->state = RUNNING;
  c->proc = p;

  ukvminithart(p->kpagetable);
  swtch(&c->context, &p->context);
  kvminithart();

  c->proc = 0;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  // Map the user-only signal return stub below the trapframe.
  if(mappages(pagetable, SIGTRAMP, PGSIZE,
              (uint64)sigtramp, PTE_R | PTE_X | PTE_U) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmunmap(pagetable, SIGTRAMP, 1, 0);
  uvmfree(pagetable, sz);
}

void
proc_freekpagetable(pagetable_t pagetable, uint64 kstack, uint64 sz)
{
  ukvmunmap(pagetable);
  uvmunmap(pagetable, 0, PGROUNDUP(sz) / PGSIZE, 0);
  uvmunmap(pagetable, kstack, 1, 1);
  uvmfree(pagetable, 0);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer
  safestrcpy(p->name, "initcode", sizeof(p->name));

  upg2ukpg(p->pagetable, p->kpagetable, 0, p->sz);
  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + (uint64)n < sz || sz + (uint64)n > vma_heap_limit(p))
      return -1;
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
    upg2ukpg(p->pagetable, p->kpagetable, p->sz, sz);
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  sfence_vma();
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  if(vma_fork(p, np) < 0){
    upg2ukpg(p->pagetable, p->kpagetable, 0, p->sz);
    sfence_vma();
    freeproc(np);
    release(&np->lock);
    return -1;
  }

  np->parent = p;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = edup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  np->trace_mask = p->trace_mask;
  np->pgid = p->pgid;
  signal_proc_fork(p, np);
  pid = np->pid;

  upg2ukpg(np->pagetable, np->kpagetable, 0, np->sz);
  sfence_vma();

  np->state = RUNNABLE;

  release(&np->lock);

  // Keep the parent's kpagetable in sync with its user page table
  // after uvmcopy modified user PTEs (COW markings).
  upg2ukpg(p->pagetable, p->kpagetable, 0, p->sz);
  sfence_vma();

  return pid;
}

int
proc_setpgid(struct proc *caller, int pid, int pgid)
{
  struct proc *p;
  int target_pid = pid == 0 ? caller->pid : pid;

  if(target_pid <= 0 || pgid < 0)
    return -1;
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state != UNUSED && p->pid == target_pid) {
      if(p != caller && p->parent != caller) {
        release(&p->lock);
        return -1;
      }
      p->pgid = pgid == 0 ? p->pid : pgid;
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

int
proc_getpgrp(struct proc *p)
{
  int pgid;

  acquire(&p->lock);
  pgid = p->pgid;
  release(&p->lock);
  return pgid;
}

// Pass p's abandoned children to init.
// Caller must hold p->lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    // this code uses pp->parent without holding pp->lock.
    // acquiring the lock first could cause a deadlock
    // if pp or a child of pp were also in exit()
    // and about to try to lock p.
    if(pp->parent == p){
      // pp->parent can't change between the check and the acquire()
      // because only the parent changes it, and we're the parent.
      acquire(&pp->lock);
      pp->parent = initproc;
      // we should wake up init here, but that would require
      // initproc->lock, which would be a deadlock, since we hold
      // the lock on one of init's children (pp). this is why
      // exit() always wakes init (before acquiring any locks).
      release(&pp->lock);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  vma_destroy_all(p);

  eput(p->cwd);
  p->cwd = 0;

  // we might re-parent a child to init. we can't be precise about
  // waking up init, since we can't acquire its lock once we've
  // acquired any other proc lock. so wake up init whether that's
  // necessary or not. init may miss this wakeup, but that seems
  // harmless.
  acquire(&initproc->lock);
  wakeup1(initproc);
  release(&initproc->lock);

  // grab a copy of p->parent, to ensure that we unlock the same
  // parent we locked. in case our parent gives us away to init while
  // we're waiting for the parent lock. we may then race with an
  // exiting parent, but the result will be a harmless spurious wakeup
  // to a dead or wrong process; proc structs are never re-allocated
  // as anything else.
  acquire(&p->lock);
  struct proc *original_parent = p->parent;
  release(&p->lock);
  
  // we need the parent's lock in order to wake it up from wait().
  // the parent-then-child rule says we have to lock it first.
  acquire(&original_parent->lock);

  acquire(&p->lock);

  // Give any children to init.
  reparent(p);

  // Notify the stable parent while its lock is already held.
  signal_send_locked(original_parent, SIGCHLD);

  // Parent might be sleeping in wait().
  wakeup1(original_parent);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&original_parent->lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  // hold p->lock for the whole time to avoid lost
  // wakeups from a child's exit().
  acquire(&p->lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      // this code uses np->parent without holding np->lock.
      // acquiring the lock first would cause a deadlock,
      // since np might be an ancestor, and we already hold p->lock.
      if(np->parent == p){
        // np->parent can't change between the check and the acquire()
        // because only the parent changes it, and we're the parent.
        acquire(&np->lock);
        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&p->lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&p->lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || signal_pending_locked(p)){
      release(&p->lock);
      return -1;
    }
    
    // Wait for a child to exit.
    if(sleep_interruptible(p, &p->lock) < 0) {
      release(&p->lock);
      return -1;
    }
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    account_runnable_wait();
    p = pick_next_proc();
    if(p) {
      // Switch to chosen process. It releases p->lock before user space.
      run_proc(c, p);
      release(&p->lock);
    } else {
      intr_on();
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  sched_on_yield(p);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fat32_init();
    myproc()->cwd = ename("/");
  }
  
  usertrapret();
}

static int
sleep_on(void *chan, struct spinlock *lk, int interruptible)
{
  struct proc *p = myproc();
  int interrupted = 0;
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.
  if(lk != &p->lock){  //DOC: sleeplock0
    acquire(&p->lock);  //DOC: sleeplock1
    release(lk);
  }

  if(interruptible && signal_pending_locked(p)) {
    interrupted = 1;
    goto out;
  }

  // Go to sleep.
  p->chan = chan;
  p->interruptible_sleep = interruptible;
  p->state = SLEEPING;
#ifdef SCHED_MLFQ
  // Voluntary sleep should not be punished as CPU-bound behavior.
  p->sched_ticks = 0;
#endif

  sched();

  // Tidy up.
  p->chan = 0;
  p->interruptible_sleep = 0;
  if(interruptible && signal_pending_locked(p))
    interrupted = 1;

out:
  // Reacquire original lock.
  if(lk != &p->lock){
    release(&p->lock);
    acquire(lk);
  }
  return interrupted ? -1 : 0;
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  sleep_on(chan, lk, 0);
}

int
sleep_interruptible(void *chan, struct spinlock *lk)
{
  return sleep_on(chan, lk, 1);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  wakeup_reason(chan, WAKEUP_NORMAL);
}

void
wakeup_reason(void *chan, int reason)
{
  struct proc *p;

  // Extended wakeup path records why the process became runnable.
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == SLEEPING && p->chan == chan) {
      p->state = RUNNABLE;
      p->last_wakeup_reason = reason;
#ifdef SCHED_MLFQ
      mlfq_promote_on_wakeup(p, reason);
#endif
    }
    release(&p->lock);
  }
}

// Wake up p if it is sleeping in wait(); used by exit().
// Caller must hold p->lock.
static void
wakeup1(struct proc *p)
{
  if(!holding(&p->lock))
    panic("wakeup1");
  if(p->chan == p && p->state == SLEEPING) {
    p->state = RUNNABLE;
  }
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

static int
decimal_width(int n)
{
  int w = 1;

  if(n < 0) {
    w++;
    n = -n;
  }
  while(n >= 10) {
    n /= 10;
    w++;
  }
  return w;
}

static void
print_spaces(int n)
{
  while(n-- > 0)
    printf(" ");
}

static void
print_int_col(int n, int width)
{
  print_spaces(width - decimal_width(n));
  printf("%d", n);
}

static void
print_str_col(char *s, int width)
{
  int len = strlen(s);

  if(len > width)
    len = width;
  for(int i = 0; i < len; i++) {
    char ch[2];
    ch[0] = s[i];
    ch[1] = 0;
    printf("%s", ch);
  }
  print_spaces(width - len);
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;
  char name[17];

  printf("\n");
#ifdef SCHED_MLFQ
  printf(" PID  STATE  NAME          Q    RUN   WAIT  PREEMPT  BOOST    IO   DEV  WAKE\n");
  printf("----  -----  ------------  -  -----  -----  -------  -----  ----  ----  ----\n");
#else
  printf(" PID  STATE  NAME           RUN   WAIT  PREEMPT  WAKE\n");
  printf("----  -----  ------------  -----  -----  -------  ----\n");
#endif
  for(p = proc; p < &proc[NPROC]; p++){
    int idx = p - proc;
    int proc_state = p->state;

    if(proc_state == UNUSED)
      continue;
    if(proc_state < UNUSED || proc_state >= NELEM(states) || states[proc_state] == 0) {
      printf("slot %d badstate %d\n", idx, proc_state);
      continue;
    }

    state = states[proc_state];
    memmove(name, p->name, sizeof(p->name));
    name[sizeof(p->name)] = 0;
#ifdef SCHED_MLFQ
    print_int_col(p->pid, 4);
    printf("  ");
    print_str_col(state, 5);
    printf("  ");
    print_str_col(name, 12);
    printf("  ");
    print_int_col(p->queue_level, 1);
    printf("  ");
    print_int_col(p->total_run_ticks, 5);
    printf("  ");
    print_int_col(p->wait_ticks, 5);
    printf("  ");
    print_int_col(p->preempt_count, 7);
    printf("  ");
    print_int_col(p->boost_count, 5);
    printf("  ");
    print_int_col(p->io_wakeup_count, 4);
    printf("  ");
    print_int_col(p->device_wakeup_count, 4);
    printf("  ");
    print_int_col(p->last_wakeup_reason, 4);
#else
    print_int_col(p->pid, 4);
    printf("  ");
    print_str_col(state, 5);
    printf("  ");
    print_str_col(name, 12);
    printf("  ");
    print_int_col(p->total_run_ticks, 5);
    printf("  ");
    print_int_col(p->wait_ticks, 5);
    printf("  ");
    print_int_col(p->preempt_count, 7);
    printf("  ");
    print_int_col(p->last_wakeup_reason, 4);
#endif
    printf("\n");
  }
}
