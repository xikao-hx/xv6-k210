#include "kalloc.h"
#include "kbuf.h"
#include "printf.h"
#include "spinlock.h"
#include "string.h"
#include "vm.h"

struct kbuf_page {
  void *address;
  struct kbuf_page *next;
};

struct kbuf {
  struct spinlock lock;
  int refcnt;
  uint64 size;
  uint64 npages;
  struct kbuf_page *pages;
};

static void
kbuf_destroy(struct kbuf *kbuf)
{
  struct kbuf_page *page = kbuf->pages;

  while(page){
    struct kbuf_page *next = page->next;
    kfree_page(page->address);
    kfree(page);
    page = next;
  }
  kfree(kbuf);
}

struct kbuf *
kbuf_create(uint64 size)
{
  struct kbuf *kbuf;
  struct kbuf_page **next;
  uint64 rounded;

  if(size == 0)
    return 0;
  rounded = PGROUNDUP(size);
  if(rounded < size)
    return 0;
  if((kbuf = kmalloc(sizeof(*kbuf))) == 0)
    return 0;
  memset(kbuf, 0, sizeof(*kbuf));
  initlock(&kbuf->lock, "kbuf");
  kbuf->refcnt = 1;
  kbuf->size = size;
  kbuf->npages = rounded / PGSIZE;

  next = &kbuf->pages;
  for(uint64 i = 0; i < kbuf->npages; i++){
    struct kbuf_page *page = kmalloc(sizeof(*page));

    if(page == 0 || (page->address = kalloc_page()) == 0){
      if(page)
        kfree(page);
      kbuf_destroy(kbuf);
      return 0;
    }
    memset(page->address, 0, PGSIZE);
    page->next = 0;
    *next = page;
    next = &page->next;
  }
  return kbuf;
}

void
kbuf_get(struct kbuf *kbuf)
{
  acquire(&kbuf->lock);
  if(kbuf->refcnt < 1)
    panic("kbuf_get");
  kbuf->refcnt++;
  release(&kbuf->lock);
}

void
kbuf_put(struct kbuf *kbuf)
{
  int destroy;

  acquire(&kbuf->lock);
  if(kbuf->refcnt < 1)
    panic("kbuf_put");
  kbuf->refcnt--;
  destroy = kbuf->refcnt == 0;
  release(&kbuf->lock);
  if(destroy)
    kbuf_destroy(kbuf);
}

uint64
kbuf_size(struct kbuf *kbuf)
{
  return kbuf->size;
}

void *
kbuf_page_address(struct kbuf *kbuf, uint64 index)
{
  struct kbuf_page *page;

  if(index >= kbuf->npages)
    return 0;
  page = kbuf->pages;
  while(index-- > 0)
    page = page->next;
  return page->address;
}

void *
kbuf_page_get(struct kbuf *kbuf, uint64 index)
{
  void *address = kbuf_page_address(kbuf, index);

  if(address)
    kaddquota(address);
  return address;
}
