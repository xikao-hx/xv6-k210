#ifndef __PLIC_H
#define __PLIC_H

#ifdef QEMU
#define UART_IRQ    10
#define DISK_IRQ    1
#else
#define UART_IRQ    33
#define DISK_IRQ    27
#endif

void            plicinit(void);
void            plicinithart(void);
int             plic_claim(void);
void            plic_complete(int);

#endif
