#ifndef __TRAP_H
#define __TRAP_H

#include "types.h"
#include "spinlock.h"

extern uint     ticks;
extern struct spinlock tickslock;
void            trapinit(void);
void            trapinithart(void);
void            usertrapret(void);

#endif
