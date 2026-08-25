#ifndef __MMAP_H
#define __MMAP_H

#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "types.h"

#define NVMA 16
#define MMAP_TOP USER_STACK_BOTTOM

struct file;
struct proc;
struct anon_object;
struct vma_area;

// Backing page-fault handlers, modeled after Linux vm_operations_struct.
// The core mm only dispatches: it never interprets the backing object.
struct vma_ops {
  // Fill *mem with the physical page backing the page-aligned va.
  // Returns 0 on success, -1 on failure.
  int (*fault)(struct vma_area *vma, uint64 va, void **mem);
};

enum vma_type {
  VMA_FILE,
  VMA_ANON,
  VMA_KBUF,
};

enum vm_fault_access {
  VM_FAULT_READ,
  VM_FAULT_WRITE,
  VM_FAULT_EXEC,
};

struct mmap_object {
  struct spinlock lock;
  int refcnt;
  enum vma_type type;
  struct file *file;
  struct anon_object *anon;
};

struct anon_page {
  uint64 index;
  uint64 pa;
  struct anon_page *next;
};

struct anon_object {
  struct spinlock lock;
  struct anon_page *pages;
};

struct vma_area {
  int used;
  enum vma_type type;
  uint64 start;
  uint64 end;
  uint64 valid_end;
  uint64 offset;
  int prot;
  int flags;
  struct mmap_object *object;
  const struct vma_ops *ops;  // backing fault handler; installed by core mm by type
  void *data;                 // driver-private data handed to ops->fault
};

uint64 vma_map_file(struct proc *, uint64, uint64, int, int,
                    struct file *, uint64);
uint64 vma_map_anon(struct proc *, uint64, uint64, int, int);
uint64 vma_map_device(struct proc *, uint64, uint64, int, int,
                      struct file *, uint64);
int vma_unmap(struct proc *, uint64, uint64);
int vm_fault(struct proc *, uint64, int);
int vma_populate(struct proc *, struct vma_area *);
int vma_fork(struct proc *, struct proc *);
void vma_destroy_all(struct proc *);
uint64 vma_heap_limit(struct proc *);

#endif
