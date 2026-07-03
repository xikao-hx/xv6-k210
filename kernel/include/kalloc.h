#ifndef __KALLOC_H
#define __KALLOC_H

#include "types.h"

#define KMALLOC_MAX_SIZE 2048

void*           kalloc_page(void);
void            kfree_page(void *);
void*           kmalloc(uint64);
void            kfree(void *);
void            kminit(void);
void            kinit(void);
uint64          freemem(void);
void            kaddquota(void *pa);
int             kgetquota(void *pa);

#endif
