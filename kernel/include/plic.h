#ifndef __PLIC_H
#define __PLIC_H

#ifdef QEMU
#define UARTHS_IRQ  10
#define DISK_IRQ    1
#else
#define UARTHS_IRQ  33
#define DISK_IRQ    27
#endif

void            plicinithart(void);
int             plic_claim(void);
void            plic_complete(int);

#endif
