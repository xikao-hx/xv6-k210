#include "irq.h"
#include "memlayout.h"
#include "plic.h"
#include "proc.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//
// Priorities and per-hart enable bits are driven by the irq registry
// (irq.c): each device registers itself with irq_register().

void
plicinithart(void)
{
  int hart = cpuid();
  uint32 *men;

#ifdef QEMU
  // set this hart's S-mode priority threshold to 0.
  *(uint32 *)PLIC_SPRIORITY(hart) = 0;
  men = (uint32 *)PLIC_SENABLE(hart);
#else
  // K210: zero this hart's M-mode priority threshold (forward all enabled
  // IRQs), then clear the enable words. 
  *(uint32 *)PLIC_MPRIORITY(hart) = 0;
  men = (uint32 *)PLIC_MENABLE(hart);
#endif
  men[0] = 0;
  men[1] = 0;
}

// ask the PLIC what interrupt we should serve.
int
plic_claim(void)
{
  int hart = cpuid();
#ifdef QEMU
  int irq = *(uint32 *)PLIC_SCLAIM(hart);
#else
  int irq = *(uint32 *)PLIC_MCLAIM(hart);
#endif
  return irq;
}

// tell the PLIC we've served this IRQ.
void
plic_complete(int irq)
{
  int hart = cpuid();
#ifdef QEMU
  *(uint32 *)PLIC_SCLAIM(hart) = irq;
#else
  *(uint32 *)PLIC_MCLAIM(hart) = irq;
#endif
}
