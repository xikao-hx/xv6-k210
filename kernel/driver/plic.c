#include "irq.h"
#include "memlayout.h"
#include "plic.h"
#include "proc.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//
// Per-source priorities and the hart enable words are owned by the irq
// registration mechanism (irq.c): irq_register() sets priority + enable at
// registration time, and irq_apply_all() rebuilds the enable words from the
// registered set.  plicinit() is kept as a no-op so the boot sequence stays
// unchanged; plicinithart() only sets the priority threshold and defers the
// enable words to irq_apply_all().

void
plicinit(void)
{
  // Priorities are set by irq_register() as each device registers; nothing
  // to do here anymore.
}

void
plicinithart(void)
{
  int hart = cpuid();
#ifdef QEMU
  // set this hart's S-mode priority threshold to 0.
  *(uint32*)PLIC_SPRIORITY(hart) = 0;
#else
  // K210: PLIC runs in M-mode.  Zero the M-mode priority threshold so all
  // enabled IRQs are forwarded to S-mode.
  *(uint32*)PLIC_MPRIORITY(hart) = 0;
#endif
  // Rebuild the enable words from the registered handlers.  The assignment
  // in irq_apply_all() also clears enable bits left over from the previous
  // boot stage (RustSBI/bootloader, e.g. GPIOHS0 = IRQ 34) instead of leaving
  // them asserting forever.  Devices that register later than this call
  // (UART1/DMAC/DISK) enable themselves inside irq_register().
  irq_apply_all();
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
