// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

struct {
  struct spinlock lock;
  char cow_quota[PHYSTOP / PGSIZE];
} cow_map;

void
kinit()
{
  for (int i = 0; i < NCPU; i ++) {
    char name[10];
    snprintf(name, sizeof(name), "kmem_%d", i); 
    initlock(&kmem[i].lock, name);
  }
  initlock(&cow_map.lock, "cow_map");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    cow_map.cow_quota[(uint64)p / PGSIZE] = 1;
    kfree(p);
  }
}

void kaddquota(void *pa) {
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&cow_map.lock);
  cow_map.cow_quota[(uint64)pa / PGSIZE] ++;
  release(&cow_map.lock);
}

int kgetquota(void *pa) {
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  return cow_map.cow_quota[(uint64)pa / PGSIZE];
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  push_off();
  int id = cpuid();
  
  acquire(&cow_map.lock);
  int cnt = --cow_map.cow_quota[(uint64)pa / PGSIZE];

  if (cnt == 0) {
    acquire(&kmem[id].lock);
    r = (struct run*)pa;

    release(&cow_map.lock);

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r->next = kmem[id].freelist;
    kmem[id].freelist = r;
    release(&kmem[id].lock);
  } else{
    release(&cow_map.lock);
  }

  pop_off();

  if (cnt < 0) {
    cow_map.cow_quota[(uint64)pa / PGSIZE] = 0;
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r) {
    kmem[id].freelist = r->next;
  } else {
    for (int n_id = 0; n_id < NCPU; n_id ++) {
      if (n_id == id) continue;
      acquire(&kmem[n_id].lock);
      r = kmem[n_id].freelist;
      if (r) {
        kmem[n_id].freelist = r->next;
        release(&kmem[n_id].lock);
        break;
      }
      release(&kmem[n_id].lock);
    }
  }
  release(&kmem[id].lock);

  pop_off();

  if(r) {
    acquire(&cow_map.lock);
    cow_map.cow_quota[(uint64)r / PGSIZE] = 1;
    release(&cow_map.lock);

    memset((char*)r, 5, PGSIZE); // fill with junk
  }
    
  return (void*)r;
}

uint64 
freemem(void)
{
  struct run *r;
  int num = 0;

  for (int id = 0; id < NCPU; id ++) {
    r = kmem[id].freelist;
    while (r) {
      num ++;
      r = r->next;
    }
  }

  return num * PGSIZE;
}