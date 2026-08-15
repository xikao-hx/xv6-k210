#include "irq.h"
#include "memlayout.h"
#include "plic.h"
#include "proc.h"
#include "printf.h"

// PLIC interrupt registry.
//
// Each device driver registers (irq, handler, ctx) once with irq_register(),
// which records the action, raises the PLIC priority above zero, and ORs the
// current hart's enable bit (self-enable for registrations that happen after
// plicinithart, e.g. UART1/DMAC/DISK).
//
// The early registrations (UARTHS, from consoleinit) land before plicinithart
// has rebuilt the enable words; plicinithart clears the enable words
// authoritatively (K210: RustSBI leaves stale bits behind, e.g. GPIOHS0 =
// IRQ 34) and then calls irq_apply_all() to re-enable every registered
// handler.  Secondary harts reach the same rebuilt words through their own
// plicinithart().
struct irq_action {
  irq_handler_t handler;
  void *ctx;
};

static struct irq_action actions[MAXIRQ];

static void
irq_enable_hart(int irq)
{
  int hart = cpuid();

#ifdef QEMU
  *(uint32 *)PLIC_SENABLE(hart) |= (1u << (irq % 32));
#else
  uint32 *men = (uint32 *)PLIC_MENABLE(hart);

  men[irq / 32] |= (1u << (irq % 32));
#endif
}

void
irq_register(int irq, irq_handler_t h, void *ctx)
{
  if (irq < 0 || irq >= MAXIRQ)
    panic("irq_register: bad irq");
  actions[irq].handler = h;
  actions[irq].ctx = ctx;
  // Non-zero priority, otherwise the PLIC treats the source as disabled.
  *(uint32 *)(PLIC + irq * sizeof(uint32)) = 1;
  irq_enable_hart(irq);
}

// Rebuild this hart's PLIC enable words from the registered handlers,
// starting from zero so bits left over from the previous boot stage (or from
// an earlier, now-unused driver) are cleared.  Called at the end of
// plicinithart(), after the residual bits have been wiped.
void
irq_apply_all(void)
{
  int hart = cpuid();
  uint32 en[2] = {0, 0};
  uint32 *men;
  int irq;

  for (irq = 1; irq < MAXIRQ; irq++)
    if (actions[irq].handler)
      en[irq / 32] |= (1u << (irq % 32));

#ifdef QEMU
  men = (uint32 *)PLIC_SENABLE(hart);
#else
  men = (uint32 *)PLIC_MENABLE(hart);
#endif
  men[0] = en[0];
  men[1] = en[1];
}

// Dispatch a claimed external interrupt to its registered handler.
void
irq_dispatch(int irq)
{
  if (irq > 0 && irq < MAXIRQ && actions[irq].handler)
    actions[irq].handler(actions[irq].ctx);
  else if (irq)
    printf("unexpected interrupt irq=%d\n", irq);
}
