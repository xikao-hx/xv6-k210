#ifndef __SPINLOCK_H
#define __SPINLOCK_H

#include "types.h"

// Mutual exclusion lock.
struct spinlock {
  uint locked;       // Is the lock held?

  // For debugging:
  char *name;        // Name of lock.
  struct cpu *cpu;   // The cpu holding the lock.
#ifdef LAB_LOCK
  int nts;
  int n;
#endif
};

void            initlock(struct spinlock*, char*);
void            acquire(struct spinlock*);
void            release(struct spinlock*);
int             holding(struct spinlock*);
void            push_off(void);
void            pop_off(void);

#endif
