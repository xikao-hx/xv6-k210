#ifndef __IRQ_H
#define __IRQ_H

// Supervisor-mode external interrupt (PLIC) handler registration.
//
// Replaces the hard-coded if/else dispatch in devintr(): each device calls
// irq_register() at init with its PLIC IRQ source and a handler that takes a
// context pointer, so one handler function can serve multiple instances of
// the same device (e.g. one uartintr registered for UART1/2/3 IRQs, each with
// a different struct uart_controller *ctx).

#define MAXIRQ 64

typedef void (*irq_handler_t)(void *ctx);

void irq_register(int irq, irq_handler_t h, void *ctx);  // record + priority + enable
void irq_apply_all(void);  // rebuild this hart's enable words from registrations
void irq_dispatch(int irq);  // table lookup; prints "unexpected" on a miss

#endif
