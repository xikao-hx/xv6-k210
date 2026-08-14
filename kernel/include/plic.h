#ifndef __PLIC_H
#define __PLIC_H

void            plicinit(void);
void            plicinithart(void);
int             plic_claim(void);
void            plic_complete(int);

#endif
