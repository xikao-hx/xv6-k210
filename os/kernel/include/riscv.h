#ifndef __RISCV_H__
#define __RISCV_H__

#include "types.h"

// ====================== 通用寄存器操作（无模式依赖，保留） ======================

// 读取线程指针 tp（存储当前 hartid，S 模式下可安全使用）
static inline uint64
r_tp()
{
  uint64 x;
  asm volatile("mv %0, tp" : "=r" (x) );
  return x;
}

// 写入线程指针 tp
static inline void
w_tp(uint64 x)
{
  asm volatile("mv tp, %0" : : "r" (x));
}

// 读取栈指针 sp
static inline uint64
r_sp()
{
  uint64 x;
  asm volatile("mv %0, sp" : "=r" (x) );
  return x;
}

// 读取帧指针 fp（s0）
static inline uint64
r_fp()
{
  uint64 x;
  asm volatile("mv %0, s0" : "=r" (x) );
  return x;
}

// 读取返回地址 ra
static inline uint64
r_ra()
{
  uint64 x;
  asm volatile("mv %0, ra" : "=r" (x) );
  return x;
}

// 读取时间计数器 time（S 模式可访问，依赖 OpenSBI 代理）
static inline uint64
r_time()
{
  uint64 x;
  asm volatile("csrr %0, time" : "=r" (x) );
  return x;
}

// ====================== S 模式 CSR 寄存器操作（核心保留，适配 S 模式） ======================

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

// 9. S 模式地址翻译与保护寄存器 satp（页表基址，开启分页）
#define SATP_SV39 (8L << 60) // Sv39 分页模式（RISC-V 64 标准）
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

// ====================== 中断控制函数（基于 S 模式，保留并修正） ======================

// 使能 S 模式全局中断
static inline void
intr_on()
{
  w_sstatus(r_sstatus() | SSTATUS_SIE);
}

// 关闭 S 模式全局中断
static inline void
intr_off()
{
  w_sstatus(r_sstatus() & ~SSTATUS_SIE);
}

// 查询中断是否使能
static inline int
intr_get()
{
  uint64 x = r_sstatus();
  return (x & SSTATUS_SIE) != 0;
}

// ====================== 内存管理相关宏（无模式依赖，保留） ======================

#define PGSIZE 4096        // 页大小（4KB）
#define PGSHIFT 12         // 页内偏移位数

#define PGROUNDUP(sz)  (((sz)+PGSIZE-1) & ~(PGSIZE-1)) // 向上对齐到页
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE-1))           // 向下对齐到页

// 页表项（PTE）标志位
#define PTE_V (1L << 0) // 有效位
#define PTE_R (1L << 1) // 可读
#define PTE_W (1L << 2) // 可写
#define PTE_X (1L << 3) // 可执行
#define PTE_U (1L << 4) // 用户态可访问
#define PTE_COW (1L << 8) // 写时复制

// 物理地址 ↔ PTE 转换
#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10)
#define PTE2PA(pte) (((pte) >> 10) << 12)
#define PTE_FLAGS(pte) ((pte) & 0x3FF) // 提取 PTE 标志位

// Sv39 虚拟地址的三级页表索引提取
#define PXMASK          0x1FF // 9 位索引
#define PXSHIFT(level)  (PGSHIFT+(9*(level)))
#define PX(level, va) ((((uint64) (va)) >> PXSHIFT(level)) & PXMASK)

// Sv39 最大虚拟地址（避免符号扩展问题）
#define MAXVA (1L << (9 + 9 + 9 + 12 - 1))

typedef uint64 pte_t;         // 页表项类型
typedef uint64 *pagetable_t;  // 页表类型（512 个 PTE）

// ====================== TLB 刷新指令（S 模式可用，保留） ======================
static inline void
sfence_vma()
{
  // 刷新所有 TLB 表项
  asm volatile("sfence.vma zero, zero");
}

#endif // __RISCV_H__