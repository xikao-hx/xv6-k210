#ifndef __VM_H
#define __VM_H

#include "types.h"
#include "riscv.h"

struct proc;

enum vm_fault_access {
  VM_FAULT_READ,
  VM_FAULT_WRITE,
  VM_FAULT_EXEC,
};

enum vm_fault_result {
  VM_FAULT_RESOLVED = 0,
  VM_FAULT_INVALID = -1,
  VM_FAULT_NOMEM = -2,
  VM_FAULT_BACKING = -3,
};

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
int             uvm_stack_init(pagetable_t);
int             uvm_stack_copy(pagetable_t, pagetable_t);
void            uvm_stack_sync(pagetable_t, pagetable_t);
void            uvm_stack_unmap(pagetable_t, int);
void            uvmfree(pagetable_t, uint64);
void            uvmunmap(pagetable_t, uint64, uint64, int);
void            uvmclear(pagetable_t, uint64);
uint64          walkaddr(pagetable_t, uint64);
int             copyout(pagetable_t, uint64, char *, uint64);
int             copyin(pagetable_t, char *, uint64, uint64);
int             copyinstr(pagetable_t, char *, uint64, uint64);
void            ukvmmap(pagetable_t pagetable, uint64 va, uint64 pa, uint64 sz, int perm);
pagetable_t     ukvminit(void);
void            ukvmfree(pagetable_t pagetable);
void            ukvmunmap(pagetable_t pagetable);
void            ukvminithart(pagetable_t pagetable);
int             upg2ukpg(pagetable_t u_pagetable, pagetable_t k_pagetable, uint64 begin_addr, uint64 end_addr);
uint64          ukvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int alloc);
int             uvm_fault_alloc(struct proc *, pagetable_t, uint64, int);
int             vm_resolve_fault(struct proc *, pagetable_t, uint64, int);
int             vm_sync_user_page(struct proc *, uint64);
pte_t *         walk(pagetable_t pagetable, uint64 va, int alloc);
void *          uvmcowmalloc(pagetable_t pagetable, uint64 va);
int             copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
int             copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);

#endif
