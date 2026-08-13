// Generic byte ring buffer.  See ringbuffer.h for the contract.

#include "ringbuffer.h"

void
ringbuffer_init(struct ringbuffer *r, uint8 *buf, uint capacity)
{
  r->buf = buf;
  r->capacity = capacity;   // usable bytes; buf[] must hold capacity + 1
  r->head = 0;
  r->tail = 0;
}

uint
ringbuffer_capacity(const struct ringbuffer *r)
{
  return r->capacity;
}

uint
ringbuffer_used(const struct ringbuffer *r)
{
  // Bytes between head and tail, wrapping around the capacity + 1 slot
  // array.  With one slot reserved, used can reach at most capacity.
  if (r->tail >= r->head)
    return r->tail - r->head;
  return r->capacity + 1 - r->head + r->tail;
}

uint
ringbuffer_free(const struct ringbuffer *r)
{
  return r->capacity - ringbuffer_used(r);
}

int
ringbuffer_empty(const struct ringbuffer *r)
{
  return r->head == r->tail;
}

int
ringbuffer_full(const struct ringbuffer *r)
{
  // Leave one slot unused so "full" stays distinguishable from "empty".
  return (r->tail + 1) % (r->capacity + 1) == r->head;
}

int
ringbuffer_push(struct ringbuffer *r, uint8 c)
{
  if (ringbuffer_full(r))
    return 0;
  r->buf[r->tail] = c;
  r->tail = (r->tail + 1) % (r->capacity + 1);
  return 1;
}

int
ringbuffer_pop(struct ringbuffer *r, uint8 *out)
{
  if (ringbuffer_empty(r))
    return 0;
  *out = r->buf[r->head];
  r->head = (r->head + 1) % (r->capacity + 1);
  return 1;
}

void
ringbuffer_reset(struct ringbuffer *r)
{
  r->head = 0;
  r->tail = 0;
}
