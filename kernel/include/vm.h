#ifndef __VM_H
#define __VM_H

#include "types.h"
#include "riscv.h"

void            kvminit(void);
void            kvminithart(void);
uint64          kvmpa(uint64);
void            kvmmap(uint64, uint64, uint64, int);
int             mappages(pagetable_t, uint64, uint64, uint64, int);
pagetable_t     uvmcreate(void);
void            uvminit(pagetable_t, uchar *, uint);
uint64          uvmalloc(pagetable_t, uint64, uint64);
uint64          uvmdealloc(pagetable_t, uint64, uint64);
int             uvmcopy(pagetable_t, pagetable_t, uint64);
void            uvmfree(pagetable_t, uint64);
void            uvmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
uint64          walkaddr(pagetable_t, uint64);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);
void            ukvmmap(pagetable_t pagetable, uint64 va, uint64 pa, uint64 sz, int perm);
pagetable_t     ukvminit(void);
void            ukvmunmap(pagetable_t pagetable);
void            ukvminithart(pagetable_t pagetable);
void            upg2ukpg(pagetable_t u_pagetable, pagetable_t k_pagetable, uint64 begin_addr, uint64 end_addr);
uint64          ukvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int alloc);
int             uvmlazymalloc(pagetable_t pagetable, uint64 va);
pte_t *         walk(pagetable_t pagetable, uint64 va, int alloc);
void *          uvmcowmalloc(pagetable_t pagetable, uint64 va);
int             uvmcowpage(pagetable_t pagetable, uint64 va);
int             copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
int             copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);

#endif
