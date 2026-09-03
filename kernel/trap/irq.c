#include "irq.h"
#include "memlayout.h"
#include "plic.h"
#include "proc.h"
#include "printf.h"
#include "param.h"

// PLIC interrupt registry.
//
// Each device driver registers (irq, handler, data) once with irq_register(),
// which records the action, raises the PLIC priority above zero, and arms the
// source's enable bit on every hart (irq_enable_hart).
//
// main() runs plicinithart() before any driver registers, clearing the enable
// words (K210: RustSBI leaves stale bits behind, e.g. GPIOHS0 = IRQ 34) and the
// priority threshold.  The enables that matter are then re-armed one source at
// a time by each driver's irq_register().  plicinithart() must stay ahead of
// every irq_register(): clearing after a registration would drop that source's
// enable and the PLIC would stop forwarding it.
struct irq_action {
  irq_handler_t handler;
  void *data;
};

static struct irq_action actions[MAXIRQ];

void
irq_enable_hart(int irq)
{
  uint32 *men;
  int cpu_id;

  for (cpu_id = 0; cpu_id < NCPU; cpu_id++) {
    #ifdef QEMU
      men = (uint32 *)PLIC_SENABLE(cpu_id);
    #else
      men = (uint32 *)PLIC_MENABLE(cpu_id);
    #endif
      men[irq / 32] |= (1u << (irq % 32));
  }
}

void
irq_set_priority(int irq, int priority)
{
  if (irq < 0 || irq >= MAXIRQ || priority < 0)
    panic("irq_set_priority: bad argument");
  *(uint32 *)(PLIC_PRIORITY + irq * sizeof(uint32)) = priority;
}

void
irq_register(int irq, irq_handler_t h, void *data)
{
  if (irq < 0 || irq >= MAXIRQ)
    panic("irq_register: bad irq");
  actions[irq].handler = h;
  actions[irq].data = data;
  // Non-zero priority, otherwise the PLIC treats the source as disabled.
  irq_set_priority(irq, 1);
  irq_enable_hart(irq);
#ifdef QEMU
  printf("DBG irq_register irq=%d men0=%x men1=%x\n", irq,
         *(uint32 *)PLIC_SENABLE(0), *(uint32 *)(PLIC_SENABLE(0) + 1));
#endif
}

// Dispatch a claimed external interrupt to its registered handler.
void
irq_dispatch(int irq)
{
  if (irq > 0 && irq < MAXIRQ && actions[irq].handler)
    actions[irq].handler(actions[irq].data);
  else if (irq)
    printf("unexpected interrupt irq=%d\n", irq);
}
