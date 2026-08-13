#ifndef _RINGBUFFER_H
#define _RINGBUFFER_H

#include "types.h"

// Byte ring buffer, extracted from the UART driver.
//
// The backing array must be capacity + 1 bytes long: one slot is reserved so
// that "empty" (head == tail) and "full" (tail + 1 == head) stay
// distinguishable.  ringbuf_capacity() returns the number of bytes actually
// usable, so callers never need to reason about the reserved slot.
struct ringbuffer {
  uint8 *buf;
  uint   capacity;   // usable bytes; array must hold capacity + 1
  uint   head;       // next byte to read
  uint   tail;       // next byte to write
};

void  ringbuffer_init(struct ringbuffer *r, uint8 *buf, uint capacity);
uint  ringbuffer_capacity(const struct ringbuffer *r);
uint  ringbuffer_used(const struct ringbuffer *r);
uint  ringbuffer_free(const struct ringbuffer *r);
int   ringbuffer_empty(const struct ringbuffer *r);
int   ringbuffer_full(const struct ringbuffer *r);
int   ringbuffer_push(struct ringbuffer *r, uint8 c);    // returns 0 if full
int   ringbuffer_pop(struct ringbuffer *r, uint8 *out);  // returns 0 if empty
void  ringbuffer_reset(struct ringbuffer *r);

#endif /* _RINGBUFFER_H */
