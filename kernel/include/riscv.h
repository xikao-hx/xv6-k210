#ifndef __RISCV_H__
#define __RISCV_H__

#include "types.h"

static inline uint64
r_tp()
{
  uint64 x;
  asm volatile("mv %0, tp" : "=r" (x) );
  return x;
}

static inline void
w_tp(uint64 x)
{
  asm volatile("mv tp, %0" : : "r" (x));
}

static inline uint64
r_sp()
{
  uint64 x;
  asm volatile("mv %0, sp" : "=r" (x) );
  return x;
}

static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}

static inline uint64
r_ra()
{
  uint64 x;
  asm volatile("mv %0, ra" : "=r" (x) );
  return x;
}

static inline uint64
r_time()
{
  uint64 x;
  asm volatile("csrr %0, time" : "=r" (x) );
  return x;
}

// ====================== Supervisor-mode CSR operations ======================

// 1. Supervisor status register (sstatus)
#define SSTATUS_SPP (1L << 8)  // Previous privilege mode: 1=S-mode, 0=U-mode
#define SSTATUS_SPIE (1L << 5) // Previous S-mode interrupt-enable state
#define SSTATUS_UPIE (1L << 4) // Previous U-mode interrupt-enable state
#define SSTATUS_SIE (1L << 1)  // Global S-mode interrupt enable
#define SSTATUS_UIE (1L << 0)  // Global U-mode interrupt enable

static inline uint64
r_sstatus()
{
  uint64 x;
  asm volatile("csrr %0, sstatus" : "=r" (x) );
  return x;
}

static inline void
w_sstatus(uint64 x)
{
  asm volatile("csrw sstatus, %0" : : "r" (x));
}

// 2. Supervisor interrupt-enable register (sie)
#define SIE_SEIE (1L << 9) // Supervisor external interrupt enable
#define SIE_STIE (1L << 5) // Supervisor timer interrupt enable
#define SIE_SSIE (1L << 1) // Supervisor software interrupt enable

static inline uint64
r_sie()
{
  uint64 x;
  asm volatile("csrr %0, sie" : "=r" (x) );
  return x;
}

static inline void
w_sie(uint64 x)
{
  asm volatile("csrw sie, %0" : : "r" (x));
}

// 3. Supervisor interrupt-pending register (sip)
#define SIP_SEIP (1L << 9) // Supervisor external interrupt pending
#define SIP_STIP (1L << 5) // Supervisor timer interrupt pending
#define SIP_SSIP (1L << 1) // Supervisor software interrupt pending

static inline uint64
r_sip()
{
  uint64 x;
  asm volatile("csrr %0, sip" : "=r" (x) );
  return x;
}

static inline void
w_sip(uint64 x)
{
  asm volatile("csrw sip, %0" : : "r" (x));
}

// 4. Supervisor trap-vector base-address register (stvec)
static inline void
w_stvec(uint64 x)
{
  asm volatile("csrw stvec, %0" : : "r" (x));
}

static inline uint64
r_stvec()
{
  uint64 x;
  asm volatile("csrr %0, stvec" : "=r" (x) );
  return x;
}

// 5. Supervisor exception program counter (sepc)
static inline void
w_sepc(uint64 x)
{
  asm volatile("csrw sepc, %0" : : "r" (x));
}

static inline uint64
r_sepc()
{
  uint64 x;
  asm volatile("csrr %0, sepc" : "=r" (x) );
  return x;
}

// 6. Supervisor trap-cause register (scause)
static inline uint64
r_scause()
{
  uint64 x;
  asm volatile("csrr %0, scause" : "=r" (x) );
  return x;
}

// 7. Supervisor trap-value register (stval)
static inline uint64
r_stval()
{
  uint64 x;
  asm volatile("csrr %0, stval" : "=r" (x) );
  return x;
}

// 8. Supervisor scratch register (sscratch)
static inline void
w_sscratch(uint64 x)
{
  asm volatile("csrw sscratch, %0" : : "r" (x));
}

static inline uint64
r_sscratch()
{
  uint64 x;
  asm volatile("csrr %0, sscratch" : "=r" (x) );
  return x;
}

#define SATP_SV39 (8L << 60) 
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12)) // Build the satp value

static inline void
w_satp(uint64 x)
{
  asm volatile("csrw satp, %0" : : "r" (x));
}

static inline uint64
r_satp()
{
  uint64 x;
  asm volatile("csrr %0, satp" : "=r" (x) );
  return x;
}

static inline void
intr_on()
{
  w_sstatus(r_sstatus() | SSTATUS_SIE);
}

static inline void
intr_off()
{
  w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

static inline int
intr_get()
{
  uint64 x = r_sstatus();
  return (x & SSTATUS_SIE) != 0;
}

#define PGSIZE 4096       
#define PGSHIFT 12         

#define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1)) 
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE-1))           

#define PTE_V (1L << 0) 
#define PTE_R (1L << 1) 
#define PTE_W (1L << 2) 
#define PTE_X (1L << 3)
#define PTE_U (1L << 4) 
#define PTE_COW (1L << 8)

#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)
#define PTE2PA(pte) (((pte) >> 10) << 12)
#define PTE_FLAGS(pte) ((pte) & 0x3FF)

#define PXMASK          0x1FF
#define PXSHIFT(level)  (PGSHIFT+(9*(level)))
#define PX(level, va) ((((uint64) (va)) >> PXSHIFT(level)) & PXMASK)

#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))

typedef uint64 pte_t;         
typedef uint64 *pagetable_t; 

static inline void
sfence_vma()
{
  asm volatile("sfence.vma zero, zero");
#ifdef QEMU
  // QEMU handles TLB coherency automatically
#else
  // K210 v1.9.1: sfence.vm may not flush the instruction TLB,
  // so we also issue fence.i to flush the instruction cache.
  asm volatile("fence.i");
#endif
}

#endif // __RISCV_H__
