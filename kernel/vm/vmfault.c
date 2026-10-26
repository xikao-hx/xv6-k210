#include "memlayout.h"
#include "mmap.h"
#include "proc.h"
#include "vm.h"

enum vm_fault_area {
  VM_AREA_INVALID,
  VM_AREA_HEAP,
  VM_AREA_STACK,
  VM_AREA_MMAP,
};

struct vm_fault_context {
  struct proc *proc;
  pagetable_t pagetable;
  uint64 address;
  uint64 page;
  int access;
  enum vm_fault_area area;
  struct vma_area *vma;
  pte_t *pte;
};

static enum vm_fault_area
vm_fault_classify(struct vm_fault_context *fault)
{
  fault->vma = mmap_vma_find(fault->proc, fault->address);
  if(fault->vma)
    return VM_AREA_MMAP;
  if(fault->address < fault->proc->sz)
    return VM_AREA_HEAP;
  if(fault->address >= USER_STACK_START &&
     fault->address < USER_STACK_TOP)
    return VM_AREA_STACK;
  return VM_AREA_INVALID;
}

static int
vm_fault_area_allows(struct vm_fault_context *fault)
{
  if(fault->area == VM_AREA_MMAP)
    return mmap_access_allowed(fault->vma, fault->access);
  if(fault->area == VM_AREA_STACK)
    return fault->access != VM_FAULT_EXEC;
  return fault->area == VM_AREA_HEAP;
}

static int
vm_fault_pte_allows(pte_t pte, int access)
{
  if(!(pte & PTE_V) || !(pte & PTE_U))
    return 0;
  if(access == VM_FAULT_READ)
    return (pte & PTE_R) != 0;
  if(access == VM_FAULT_WRITE)
    return (pte & PTE_W) != 0;
  if(access == VM_FAULT_EXEC)
    return (pte & PTE_X) != 0;
  return 0;
}

static int
vm_fault_present(struct vm_fault_context *fault)
{
  pte_t pte = *fault->pte;

  if(fault->access == VM_FAULT_WRITE && (pte & PTE_COW)){
    if(!(pte & PTE_U))
      return VM_FAULT_INVALID;
    if(uvmcowmalloc(fault->pagetable, fault->page) == 0)
      return VM_FAULT_NOMEM;
    return VM_FAULT_RESOLVED;
  }
  if(!vm_fault_pte_allows(pte, fault->access))
    return VM_FAULT_INVALID;

  if(vm_sync_user_page(fault->proc, fault->page) < 0)
    return VM_FAULT_NOMEM;
  return VM_FAULT_RESOLVED;
}

static int
vm_fault_missing(struct vm_fault_context *fault)
{
  int perm;

  if(fault->area == VM_AREA_MMAP)
    return mmap_fault_page(fault->proc, fault->vma, fault->page,
                           fault->access);

  perm = PTE_R | PTE_W;
  if(fault->area == VM_AREA_HEAP)
    perm |= PTE_X;
  if(uvm_fault_alloc(fault->proc, fault->pagetable,
                     fault->page, perm) < 0)
    return VM_FAULT_NOMEM;
  return VM_FAULT_RESOLVED;
}

int
vm_resolve_fault(struct proc *p, pagetable_t pagetable,
                 uint64 address, int access)
{
  struct vm_fault_context fault;

  if(p == 0 || pagetable != p->pagetable || address >= MAXUVA)
    return VM_FAULT_INVALID;
  if(access != VM_FAULT_READ && access != VM_FAULT_WRITE &&
     access != VM_FAULT_EXEC)
    return VM_FAULT_INVALID;

  fault.proc = p;
  fault.pagetable = pagetable;
  fault.address = address;
  fault.page = PGROUNDDOWN(address);
  fault.access = access;
  fault.area = vm_fault_classify(&fault);
  if(!vm_fault_area_allows(&fault))
    return VM_FAULT_INVALID;

  fault.pte = walk(pagetable, fault.page, 0);
  if(fault.pte && (*fault.pte & PTE_V))
    return vm_fault_present(&fault);
  return vm_fault_missing(&fault);
}
