#include "irq.h"
#include "memlayout.h"
#include "printf.h"
#include "proc.h"

struct irq_action {
  irq_handler_t h;
  void *ctx;
};

static struct irq_action actions[MAXIRQ];

// Record a handler for a PLIC IRQ source, set its priority non-zero (a zero
// priority is masked by the PLIC), and enable it for this hart.  Handlers
// registered after plicinithart() -- which clears stale boot-stage enable bits
// and then re-applies the set via irq_apply_all() -- get their enable bit
// OR'd in here, so registration order does not matter.
void
irq_register(int irq, irq_handler_t h, void *ctx)
{
  uint32 *en;
  int hart;

  if (irq < 0 || irq >= MAXIRQ)
    return;
  actions[irq].h = h;
  actions[irq].ctx = ctx;

  // non-zero priority, otherwise the PLIC keeps the source masked.
  *(uint32 *)(PLIC + irq * sizeof(uint32)) = 1;

  hart = cpuid();
#ifdef QEMU
  en = (uint32 *)PLIC_SENABLE(hart);
#else
  en = (uint32 *)PLIC_MENABLE(hart);
#endif
  en[irq / 32] |= 1u << (irq % 32);
}

// Rebuild this hart's PLIC enable words from the registered set (assignment,
// not OR), so enable bits left over from the previous boot stage (RustSBI,
// bootloader) are cleared instead of left asserting forever.  Called by
// plicinithart() after the priority threshold is set.
void
irq_apply_all(void)
{
  uint32 word0 = 0, word1 = 0;
  uint32 *en;
  int i;
  int hart;

  for (i = 0; i < MAXIRQ; i++)
    if (actions[i].h) {
      if (i < 32)
        word0 |= 1u << i;
      else
        word1 |= 1u << (i - 32);
    }

  hart = cpuid();
#ifdef QEMU
  en = (uint32 *)PLIC_SENABLE(hart);
#else
  en = (uint32 *)PLIC_MENABLE(hart);
#endif
  en[0] = word0;
  en[1] = word1;
}

// Dispatch a claimed PLIC IRQ source by table lookup.  devintr() calls this
// with plic_claim()'s result; a source with no registered handler is reported
// instead of silently dropping the interrupt.
void
irq_dispatch(int irq)
{
  if (irq > 0 && irq < MAXIRQ && actions[irq].h)
    actions[irq].h(actions[irq].ctx);
  else if (irq)
    printf("unexpected interrupt irq=%d\n", irq);
}
