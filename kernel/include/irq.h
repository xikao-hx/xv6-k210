#ifndef __IRQ_H
#define __IRQ_H

// PLIC interrupt registry, modelled on the SDK's plic_irq_register(): a
// device driver registers (irq, handler, ctx) once and both the PLIC priority
// and the current hart's enable bit are taken care of.  trap.c's devintr()
// degrades to a plain table lookup (irq_dispatch).
#define MAXIRQ 64

typedef void (*irq_handler_t)(void *ctx);

void irq_register(int irq, irq_handler_t h, void *ctx); /* 登记 + 设优先级 + 使能 */
void irq_apply_all(void);                               /* 清残留后重建使能字 */
void irq_dispatch(int irq);                             /* 查表分发，miss 打印 */

#endif
