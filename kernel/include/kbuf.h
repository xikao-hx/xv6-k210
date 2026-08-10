#ifndef __KBUF_H
#define __KBUF_H

#include "types.h"
#include "spinlock.h"

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

struct kbuf *kbuf_create(uint64);
void kbuf_get(struct kbuf *);
void kbuf_put(struct kbuf *);
uint64 kbuf_size(struct kbuf *);
void *kbuf_page_get(struct kbuf *, uint64);
void *kbuf_page_address(struct kbuf *, uint64);

#endif