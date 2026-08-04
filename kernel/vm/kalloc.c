// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "kalloc.h"
#include "memlayout.h"
#include "printf.h"
#include "proc.h"
#include "string.h"

void freerange(void *pa_start, void *pa_end);

// Put page pa on CPU id's freelist (used to spread initial free pages).
static void kfree_page_to(int id, void *pa);

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
  static char names[NCPU][10];

  for (int i = 0; i < NCPU; i ++) {
    snprintf(names[i], sizeof(names[i]), "kmem_%d", i);
    initlock(&kmem[i].lock, names[i]);
  }
  initlock(&cow_map.lock, "cow_map");
  kminit();
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  int id = 0;

  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    cow_map.cow_quota[(uint64)p / PGSIZE] = 1;
    // Spread free pages round-robin across all CPUs' freelists so no
    // single CPU holds everything and the others must steal from it.
    kfree_page_to(id, p);
    id = (id + 1) % NCPU;
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
  int quota;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&cow_map.lock);
  quota = cow_map.cow_quota[(uint64)pa / PGSIZE];
  release(&cow_map.lock);

  return quota;
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc_page().  (The exception is when
// initializing the allocator; see kinit above.)
//
// kfree_page_to() puts the page on CPU id's freelist; kfree_page()
// uses the current CPU, freerange() uses it to spread the initial
// free pages across all CPUs.
static void
kfree_page_to(int id, void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree_page");

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

  if (cnt < 0) {
    panic("kfree_page: negative quota");
  }
}

void
kfree_page(void *pa)
{
  push_off();
  kfree_page_to(cpuid(), pa);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc_page(void)
{
  struct run *r;

  push_off();
  int id = cpuid();

  // Try the local freelist first.
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if (r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  // Local freelist empty: steal from another CPU.  Hold at most one
  // kmem lock at a time, so the per-CPU lock order can never form an
  // ABBA cycle.
  if (r == 0) {
    for (int n_id = 0; n_id < NCPU; n_id ++) {
      if (n_id == id) continue;
      acquire(&kmem[n_id].lock);
      r = kmem[n_id].freelist;
      if (r)
        kmem[n_id].freelist = r->next;
      release(&kmem[n_id].lock);
      if (r)
        break;
    }
  }

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
    acquire(&kmem[id].lock);
    r = kmem[id].freelist;
    while (r) {
      num ++;
      r = r->next;
    }
    release(&kmem[id].lock);
  }

  return num * PGSIZE;
}
