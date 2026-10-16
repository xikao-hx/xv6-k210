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
  uint64 end;
  uint64 n;

  (void)pagetable;

  if(srcva >= MAXUVA || len > MAXUVA - srcva)
    return -1;

  end = srcva + len;
  p->copy_fault_active = 1;
  p->copy_fault_recovery = (uint64)&&copy_fault;

  while(srcva < end){
    n = PGSIZE - (srcva & (PGSIZE - 1));
    if(n > end - srcva)
      n = end - srcva;
    while(n-- > 0)
      *dst++ = *(volatile uchar *)srcva++;
  }

  p->copy_fault_active = 0;
  p->copy_fault_recovery = 0;
  stats.ncopyin++;   // XXX lock
  return 0;

copy_fault:
  p->copy_fault_active = 0;
  p->copy_fault_recovery = 0;
  return -1;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  struct proc *p = myproc();
  uint64 n;

  (void)pagetable;

  if(srcva >= MAXUVA)
    return -1;

  stats.ncopyinstr++;   // XXX lock
  p->copy_fault_active = 1;
  p->copy_fault_recovery = (uint64)&&copy_fault;

  while(max > 0) {
    if(srcva >= MAXUVA)
      goto copy_fault;
    n = PGSIZE - (srcva & (PGSIZE - 1));
    if(n > max)
      n = max;
    while(n-- > 0){
      uchar c = *(volatile uchar *)srcva++;
      if(c == '\0'){
        *dst = '\0';
        p->copy_fault_active = 0;
        p->copy_fault_recovery = 0;
        return 0;
      }
      *dst++ = c;
      max--;
    }
  }

copy_fault:
  p->copy_fault_active = 0;
  p->copy_fault_recovery = 0;
  return -1;
}
