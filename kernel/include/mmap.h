#ifndef __MMAP_H
#define __MMAP_H

#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "types.h"

#define NVMA 16
#define MMAP_TOP KSTACK(0)

struct file;
struct proc;
struct anon_object;
struct kbuf;

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
  struct kbuf *kbuf;
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
};

uint64 vma_map_file(struct proc *, uint64, uint64, int, int,
                    struct file *, uint64);
uint64 vma_map_anon(struct proc *, uint64, uint64, int, int);
uint64 vma_map_device(struct proc *, uint64, uint64, int, int,
                      struct file *, uint64);
int vma_unmap(struct proc *, uint64, uint64);
int vm_fault(struct proc *, uint64, int);
int vma_fork(struct proc *, struct proc *);
void vma_destroy_all(struct proc *);
uint64 vma_heap_limit(struct proc *);

#endif
