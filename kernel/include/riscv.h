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

// ====================== S 模式 CSR 寄存器操作 ======================

// 1. S 模式状态寄存器 sstatus
#define SSTATUS_SPP (1L << 8)  // 先前模式：1=S模式，0=U模式
#define SSTATUS_SPIE (1L << 5) // S 模式先前中断使能
#define SSTATUS_UPIE (1L << 4) // U 模式先前中断使能
#define SSTATUS_SIE (1L << 1)  // S 模式中断使能（核心！控制全局中断）
#define SSTATUS_UIE (1L << 0)  // U 模式中断使能

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

// 2. S 模式中断使能寄存器 sie
#define SIE_SEIE (1L << 9) // 外部中断使能
#define SIE_STIE (1L << 5) // 定时器中断使能
#define SIE_SSIE (1L << 1) // 软件中断使能

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

// 3. S 模式中断挂起寄存器 sip
#define SIP_SEIP (1L << 9) // 外部中断挂起
#define SIP_STIP (1L << 5) // 定时器中断挂起
#define SIP_SSIP (1L << 1) // 软件中断挂起

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

// 4. S 模式陷阱向量基址寄存器 stvec（异常/中断处理入口）
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

// 5. S 模式异常程序计数器 sepc（保存异常触发时的指令地址）
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

// 6. S 模式陷阱原因寄存器 scause（异常/中断类型）
static inline uint64
r_scause()
{
  uint64 x;
  asm volatile("csrr %0, scause" : "=r" (x) );
  return x;
}

// 7. S 模式陷阱值寄存器 stval（异常相关的附加信息，如错误地址）
static inline uint64
r_stval()
{
  uint64 x;
  asm volatile("csrr %0, stval" : "=r" (x) );
  return x;
}

// 8. S 模式临时寄存器 sscratch（陷阱处理时保存上下文）
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
#define MAKE_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12)) // 构造 satp 值

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