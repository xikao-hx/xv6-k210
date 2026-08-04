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
#include "trap.h"

#define NBUCKET 13
#define HASH(blockno) (blockno % NBUCKET)

struct hashbuf {
  struct spinlock lock;
  struct buf head;
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head;
  struct hashbuf buckets[NBUCKET];
} bcache;

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

      acquire(&tickslock);
      b->timestamp = ticks;
      release(&tickslock);

      release(&bcache.buckets[bid].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.  Drop the target bucket's lock and recycle the least
  // recently used (LRU) unused buffer.  Scan buckets one at a time,
  // holding at most one bucket lock, so the lock order can never form
  // an ABBA cycle.
  release(&bcache.buckets[bid].lock);

  b = 0;
  struct buf *tmp;
  for (int i = 0; i < NBUCKET; i ++) {
    int bucket = (bid + i) % NBUCKET;

    acquire(&bcache.buckets[bucket].lock);
    for (tmp = bcache.buckets[bucket].head.prev; tmp != &bcache.buckets[bucket].head; tmp = tmp->prev) {
      if (tmp->refcnt == 0 && (b == 0 || tmp->timestamp < b->timestamp)) {
        b = tmp;
      }
    }

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

  acquire(&tickslock);
  b->timestamp = ticks;
  release(&tickslock);

  release(&bcache.buckets[bid].lock);
  acquiresleep(&b->lock);

  return b;
  /*
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  */
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
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  int bid = HASH(b->blockno);

  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.buckets[bid].lock);
  b->refcnt--;
  
  acquire(&tickslock);
  b->timestamp = ticks;
  release(&tickslock);

  release(&bcache.buckets[bid].lock);
  /*
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  */
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
