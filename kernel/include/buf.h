#ifndef __BUF_H
#define __BUF_H

#ifndef BSIZE
#define BSIZE 512
#endif

#include "types.h"
#include "sleeplock.h"

struct buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;
  uint referenced;
  struct buf *prev; // hash bucket list
  struct buf *next;
  uchar data[BSIZE];
};

void            binit(void);
struct buf*     bread(uint, uint);
void            brelse(struct buf*);
void            bwrite(struct buf*);
void            bpin(struct buf*);
void            bunpin(struct buf*);
void            binvalidate(uint);

#endif
