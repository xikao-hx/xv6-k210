#include "memlayout.h"
#include "plic.h"
#include "proc.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//

void
plicinit(void)
{
  // set desired IRQ priorities non-zero (otherwise disabled).
  *(uint32*)(PLIC + UART_IRQ * sizeof(uint32)) = 1;
  *(uint32*)(PLIC + DISK_IRQ * sizeof(uint32)) = 1;
}

void
plicinithart(void)
{
  int hart = cpuid();
#ifdef QEMU
  // set uart's enable bit for this hart's S-mode.
  *(uint32*)PLIC_SENABLE(hart) = (1 << UART_IRQ) | (1 << DISK_IRQ);
  // set this hart's S-mode priority threshold to 0.
  *(uint32*)PLIC_SPRIORITY(hart) = 0;
#else
  // K210: PLIC runs in M-mode, enable both UART and DISK interrupts
  uint32 *hart_m_enable = (uint32*)PLIC_MENABLE(hart);
  *hart_m_enable = *hart_m_enable | (1 << DISK_IRQ);
  uint32 *hart_m_enable_hi = hart_m_enable + 1;
  *hart_m_enable_hi = *hart_m_enable_hi | (1 << (UART_IRQ % 32));
#endif
}

// ask the PLIC what interrupt we should serve.
int
plic_claim(void)
{
  int hart = cpuid();
#ifdef QEMU
  int irq = *(uint32*)PLIC_SCLAIM(hart);
#else
  int irq = *(uint32*)PLIC_MCLAIM(hart);
#endif
  return irq;
}

// tell the PLIC we've served this IRQ.
void
plic_complete(int irq)
{
  int hart = cpuid();
#ifdef QEMU
  *(uint32*)PLIC_SCLAIM(hart) = irq;
#else
  *(uint32*)PLIC_MCLAIM(hart) = irq;
#endif
}
