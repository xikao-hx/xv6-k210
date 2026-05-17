#ifndef __SLEEPLOCK_H
#define __SLEEPLOCK_H

#include "spinlock.h"

// Long-term locks for processes
struct sleeplock {
  uint locked;       // Is the lock held?
  struct spinlock lk; // spinlock protecting this sleep lock

  // For debugging:
  char *name;        // Name of lock.
  int pid;           // Process holding lock
};

void            initsleeplock(struct sleeplock*, char*);
void            acquiresleep(struct sleeplock*);
void            releasesleep(struct sleeplock*);
int             holdingsleep(struct sleeplock*);

#endif

