#ifndef __IRQ_H
#define __IRQ_H

// PLIC interrupt registry, modelled on the SDK's plic_irq_register(): a
// device driver registers (irq, handler, data) once and both the PLIC priority
// and the current hart's enable bit are taken care of.  trap.c's devintr()
// degrades to a plain table lookup (irq_dispatch).
#define MAXIRQ 64

typedef void (*irq_handler_t)(void *data);

void irq_register(int irq, irq_handler_t h, void *data);
void irq_enable_hart(int irq);
void irq_dispatch(int irq);

#endif