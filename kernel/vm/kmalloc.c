// Small kernel object allocator built on top of page allocation.

#include "kalloc.h"
#include "memlayout.h"
#include "printf.h"
#include "riscv.h"
#include "spinlock.h"
#include "string.h"

extern char end[];

struct slab_node {
  struct slab_node *next;
};

struct slab_page {
  uint magic;
  uint obj_size;
  uint total;
  uint free_count;
  struct slab_page *next;
  struct slab_node *free_list;
};

struct slab_bucket {
  struct spinlock lock;
  uint obj_size;
  struct slab_page *slabs;
};

#define SLAB_MAGIC 0x51ab51ab
#define NSLAB_BUCKETS 8
#define SLAB_ALIGN 16

static struct slab_bucket slab_buckets[NSLAB_BUCKETS];
static uint slab_sizes[NSLAB_BUCKETS] = {
  16, 32, 64, 128, 256, 512, 1024, KMALLOC_MAX_SIZE
};

void
kminit(void)
{
  static char names[NSLAB_BUCKETS][16];
  for (int i = 0; i < NSLAB_BUCKETS; i ++) {
    snprintf(names[i], sizeof(names[i]), "kslab_%d", i);
    initlock(&slab_buckets[i].lock, names[i]);
    slab_buckets[i].obj_size = slab_sizes[i];
    slab_buckets[i].slabs = 0;
  }
}

static int
slab_bucket_index(uint64 size)
{
  for (int i = 0; i < NSLAB_BUCKETS; i ++) {
    if (size <= slab_buckets[i].obj_size)
      return i;
  }

  return -1;
}

static uint64
align_up(uint64 value, uint64 align)
{
  return (value + align - 1) & ~(align - 1);
}

static uint64
slab_data_offset(void)
{
  return align_up(sizeof(struct slab_page), SLAB_ALIGN);
}

void *
kmalloc(uint64 size)
{
  struct slab_bucket *bucket;
  struct slab_page *slab;
  struct slab_node *node;
  char *page;
  uint obj_size;
  uint offset;
  uint total;
  int index;

  if (size == 0)
    return 0;

  index = slab_bucket_index(size);
  if (index < 0)
    return 0;

  bucket = &slab_buckets[index];
  obj_size = bucket->obj_size;

  acquire(&bucket->lock);
  for (slab = bucket->slabs; slab; slab = slab->next) {
    if (slab->free_count > 0)
      break;
  }

  /* alloc page and cut small block */
  if (slab == 0) {
    release(&bucket->lock);

    page = kalloc_page();
    if (page == 0)
      return 0;

    slab = (struct slab_page *)page;
    slab->magic = SLAB_MAGIC;
    slab->obj_size = obj_size;
    slab->free_count = 0;
    slab->free_list = 0;

    offset = slab_data_offset();
    total = (PGSIZE - offset) / obj_size;
    slab->total = total;

    for (uint i = 0; i < total; i ++) {
      /* head insert */
      node = (struct slab_node *)(page + offset + i * obj_size);
      node->next = slab->free_list;
      slab->free_list = node;  // update free_list place
      slab->free_count ++;
    }

    acquire(&bucket->lock);
    /* head insert */
    slab->next = bucket->slabs;
    bucket->slabs = slab;  // update slab place
  }

  /* head alloc */
  node = slab->free_list;
  slab->free_list = node->next;  // update free_list
  slab->free_count --;
  release(&bucket->lock);

  memset(node, 5, obj_size);
  return node;
}

void
kfree(void *ptr)
{
  struct slab_bucket *bucket;
  struct slab_page *slab;
  struct slab_page **prev;
  struct slab_node *node;
  uint64 page;
  uint64 offset;
  int index;
  int release_page;

  if (ptr == 0)
    return;

  page = PGROUNDDOWN((uint64)ptr);
  if (page < (uint64)end || page >= PHYSTOP)
    panic("kfree");

  slab = (struct slab_page *)page;
  if (slab->magic != SLAB_MAGIC)
    panic("kfree: not kmalloc object");

  index = slab_bucket_index(slab->obj_size);
  if (index < 0 || slab_buckets[index].obj_size != slab->obj_size)
    panic("kfree: bad slab");

  offset = slab_data_offset();
  if ((uint64)ptr < page + offset ||
      (uint64)ptr >= page + PGSIZE ||
      (((uint64)ptr - page - offset) % slab->obj_size) != 0)
    panic("kfree: bad object");

  bucket = &slab_buckets[index];
  node = (struct slab_node *)ptr;
  release_page = 0;

  acquire(&bucket->lock);
  if (slab->free_count >= slab->total)
    panic("kfree: full slab");
  memset(ptr, 1, slab->obj_size);
  
  /* insert head */
  node->next = slab->free_list;
  slab->free_list = node;
  slab->free_count ++;

  /* prepart free page */
  if (slab->free_count == slab->total) {
    prev = &bucket->slabs;
    /* ensure not head slab */
    while (*prev && *prev != slab)
      prev = &(*prev)->next;
    if (*prev == 0)
      panic("kfree: missing slab");
    *prev = slab->next;  // update head
    slab->magic = 0;
    release_page = 1;
  }
  release(&bucket->lock);

  // kfree() releases kmalloc objects; return whole free slabs to page allocator.
  if (release_page)
    kfree_page((void *)page);
}
