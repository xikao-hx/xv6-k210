#include "printf.h"
#include "proc.h"
#include "string.h"
#include "vm.h"
//
// This file contains copyin_new() and copyinstr_new(), the
// replacements for copyin and coyinstr in vm.c.
//

static struct stats {
  int ncopyin;
  int ncopyinstr;
} stats;

int
statscopyin(char *buf, int sz) {
  int n;
  n = snprintf(buf, sz, "copyin: %d\n", stats.ncopyin);
  n += snprintf(buf+n, sz, "copyinstr: %d\n", stats.ncopyinstr);
  return n;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  struct proc *p = myproc();
  uint64 n;
  uint64 va0;
  uint64 pa0;
  pte_t *pte;

  if(srcva + len < srcva)
    return -1;
  
  while(len > 0){
    if(srcva >= MAXVA)
      return -1;
    va0 = PGROUNDDOWN(srcva);
    pte = walk(pagetable, va0, 0);
    if((pte == 0 || !(*pte & PTE_V)) && pagetable == p->pagetable){
      if(vm_fault(p, va0, VM_FAULT_READ) < 0)
        return -1;
      pte = walk(pagetable, va0, 0);
    }
    if(pte == 0 ||
       (*pte & (PTE_V | PTE_U | PTE_R)) != (PTE_V | PTE_U | PTE_R))
      return -1;
    pa0 = PTE2PA(*pte);
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);
    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }

  stats.ncopyin++;   // XXX lock
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  struct proc *p = myproc();
  uint64 va0;
  uint64 pa0;
  uint64 n;
  pte_t *pte;
  
  stats.ncopyinstr++;   // XXX lock
  while(max > 0) {
    if(srcva >= MAXVA)
      return -1;
    va0 = PGROUNDDOWN(srcva);
    pte = walk(pagetable, va0, 0);
    if((pte == 0 || !(*pte & PTE_V)) && pagetable == p->pagetable){
      if(vm_fault(p, va0, VM_FAULT_READ) < 0)
        return -1;
      pte = walk(pagetable, va0, 0);
    }
    if(pte == 0 ||
       (*pte & (PTE_V | PTE_U | PTE_R)) != (PTE_V | PTE_U | PTE_R))
      return -1;
    pa0 = PTE2PA(*pte);
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;
    char *src = (char *)(pa0 + (srcva - va0));
    while(n > 0){
      if(*src == '\0'){
        *dst = '\0';
        return 0;
      }
      *dst++ = *src++;
      srcva++;
      max--;
      n--;
    }
  }
  return -1;
}
