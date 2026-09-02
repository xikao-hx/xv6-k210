// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "buf.h"
#include "disk.h"
#include "printf.h"
#include "proc.h"

#define NBUCKET 13
#define HASH(blockno) (blockno % NBUCKET)

struct hashbuf {
  struct spinlock lock;
  struct buf head;
  struct buf *clock_hand;
};

struct {
  struct buf buf[NBUF];
  struct hashbuf buckets[NBUCKET];
} bcache;

// Return an unused buffer selected by CLOCK.  The caller holds the
// bucket lock.  Two complete passes are enough to clear every reference
// bit and then select an unreferenced buffer.
static struct buf*
clock_victim(struct hashbuf *bucket)
{
  struct buf *b;
  int scanned = 0;

  while (scanned < 2 * NBUF) {
    b = bucket->clock_hand;
    if (b == &bucket->head) {
      b = b->next;
      bucket->clock_hand = b;
      if (b == &bucket->head)
        return 0;
      continue;
    }

    bucket->clock_hand = b->next;
    scanned++;

    if (b->refcnt != 0)
      continue;
    if (b->referenced) {
      b->referenced = 0;
      continue;
    }
    return b;
  }

  return 0;
}

void
binit(void)
{
  struct buf *b;
  int bid = 0;

  // init linked list of buckets
  static char names[NBUCKET][10];
  for (int i = 0; i < NBUCKET; i ++) {
    snprintf(names[i], sizeof(names[i]), "bcache_%d", i);
    initlock(&bcache.buckets[i].lock, names[i]);

    bcache.buckets[i].head.prev = &bcache.buckets[i].head;
    bcache.buckets[i].head.next = &bcache.buckets[i].head;
    bcache.buckets[i].clock_hand = &bcache.buckets[i].head;
  }

  // Spread buffers round-robin across buckets so no single bucket
  // starts out holding all of them (same idea as kalloc's freerange).
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.buckets[bid].head.next;
    b->prev = &bcache.buckets[bid].head;
    initsleeplock(&b->lock, "buffer");
    bcache.buckets[bid].head.next->prev = b;
    bcache.buckets[bid].head.next = b;
    bid = (bid + 1) % NBUCKET;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int bid = HASH(blockno);

  acquire(&bcache.buckets[bid].lock);

  // Is the block already cached?
  for(b = bcache.buckets[bid].head.next; b != &bcache.buckets[bid].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      b->referenced = 1;

      release(&bcache.buckets[bid].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.  Drop the target bucket's lock and recycle an unused
  // buffer selected by CLOCK.  Scan buckets one at a time,
  // holding at most one bucket lock, so the lock order can never form
  // an ABBA cycle.
  release(&bcache.buckets[bid].lock);

  b = 0;
  for (int i = 0; i < NBUCKET; i ++) {
    int bucket = (bid + i) % NBUCKET;

    acquire(&bcache.buckets[bucket].lock);
    b = clock_victim(&bcache.buckets[bucket]);

    if (b) {
      // Detach the victim from its bucket.  b is now on no list, so no
      // other CPU can see it; it is inserted into the target bucket
      // below under that bucket's lock.
      b->next->prev = b->prev;
      b->prev->next = b->next;
      release(&bcache.buckets[bucket].lock);
      break;
    }
    release(&bcache.buckets[bucket].lock);
  }

  if (b == 0)
    panic("bget: no buffers");

  acquire(&bcache.buckets[bid].lock);
  // Insert the recycled buffer into the target bucket.
  b->next = bcache.buckets[bid].head.next;
  b->prev = &bcache.buckets[bid].head;
  bcache.buckets[bid].head.next->prev = b;
  bcache.buckets[bid].head.next = b;
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  b->referenced = 1;

  release(&bcache.buckets[bid].lock);
  acquiresleep(&b->lock);

  return b;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    disk_read(b);
    b->valid = 1;
  }
  
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  disk_write(b);
}

// Drop cached clean block contents for a device.  Raw SD writes bypass the
// buffer cache, so cached FAT blocks must be marked invalid before the file
// system is used again.
void
binvalidate(uint dev)
{
  struct buf *b;

  for (int i = 0; i < NBUCKET; i++) {
    acquire(&bcache.buckets[i].lock);
    for (b = bcache.buckets[i].head.next; b != &bcache.buckets[i].head; b = b->next) {
      if (b->dev == dev && b->refcnt == 0)
        b->valid = 0;
    }
    release(&bcache.buckets[i].lock);
  }
}

// Release a locked buffer.
// Mark the buffer as recently used for CLOCK replacement.
void
brelse(struct buf *b)
{
  int bid = HASH(b->blockno);

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;
  b->referenced = 1;

  release(&bcache.buckets[bid].lock);
}

void
bpin(struct buf *b) {
  int bid = HASH(b->blockno);

  acquire(&bcache.buckets[bid].lock);
  b->refcnt++;
  release(&bcache.buckets[bid].lock);
}

void
bunpin(struct buf *b) {
  int bid = HASH(b->blockno);

  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;
  release(&bcache.buckets[bid].lock);
}
