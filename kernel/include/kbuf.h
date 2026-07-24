#ifndef __KBUF_H
#define __KBUF_H

#include "types.h"

struct kbuf;

struct kbuf *kbuf_create(uint64);
void kbuf_get(struct kbuf *);
void kbuf_put(struct kbuf *);
uint64 kbuf_size(struct kbuf *);
void *kbuf_page_get(struct kbuf *, uint64);
void *kbuf_page_address(struct kbuf *, uint64);

#endif
