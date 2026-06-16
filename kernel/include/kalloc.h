#ifndef __KALLOC_H
#define __KALLOC_H

#include "types.h"

void*           kalloc(void);
void            kfree(void *);
void            kinit(void);
uint64          freemem(void);
void            kaddquota(void *pa);
int             kgetquota(void *pa);

#endif
