#include "file.h"
#include "kalloc.h"
#include "mmap_file.h"
#include "proc.h"
#include "printf.h"
#include "spinlock.h"
#include "string.h"
#include "vm.h"

enum mmap_file_page_state {
  MMAP_FILE_LOADING,
  MMAP_FILE_READY,
  MMAP_FILE_WRITING,
  MMAP_FILE_EVICTING,
};

struct mmap_file_page {
  struct dirent *identity;
  uint64 offset;
  uint64 pa;
  uint64 dirty_length;
  int mappings;
  enum mmap_file_page_state state;
  struct mmap_file_page *next;
};

static struct {
  struct spinlock lock;
  struct mmap_file_page *pages;
} mmap_file_cache;

void
mmap_file_cache_init(void)
{
  initlock(&mmap_file_cache.lock, "mmap_file_cache");
}

static struct mmap_file_page *
mmap_file_find(struct file *file, uint64 offset)
{
  for(struct mmap_file_page *page = mmap_file_cache.pages;
      page; page = page->next){
    if(page->identity == file->ep && page->offset == offset)
      return page;
  }
  return 0;
}

static void
mmap_file_remove(struct mmap_file_page *target)
{
  struct mmap_file_page **link = &mmap_file_cache.pages;

  while(*link && *link != target)
    link = &(*link)->next;
  if(*link != target)
    panic("mmap_file_remove");
  *link = target->next;
}

static void
mmap_file_mark_dirty(struct mmap_file_page *page, uint64 dirty_length)
{
  if(dirty_length > PGSIZE)
    dirty_length = PGSIZE;
  if(dirty_length > page->dirty_length)
    page->dirty_length = dirty_length;
}

void *
mmap_file_page_get(struct file *file, uint64 offset, uint64 dirty_length)
{
  struct mmap_file_page *candidate = 0;
  void *mem = 0;

  for(;;){
    struct mmap_file_page *page;

    acquire(&mmap_file_cache.lock);
retry:
    page = mmap_file_find(file, offset);
    if(page){
      if(page->state != MMAP_FILE_READY){
        sleep(page, &mmap_file_cache.lock);
        goto retry;
      }
      page->mappings++;
      mmap_file_mark_dirty(page, dirty_length);
      kaddquota((void *)page->pa);
      release(&mmap_file_cache.lock);
      if(candidate){
        kfree_page(mem);
        kfree(candidate);
      }
      return (void *)page->pa;
    }
    if(candidate == 0){
      release(&mmap_file_cache.lock);
      candidate = kmalloc(sizeof(*candidate));
      mem = kalloc_page();
      if(candidate == 0 || mem == 0){
        if(candidate)
          kfree(candidate);
        if(mem)
          kfree_page(mem);
        return 0;
      }
      memset(candidate, 0, sizeof(*candidate));
      memset(mem, 0, PGSIZE);
      candidate->identity = file->ep;
      candidate->offset = offset;
      candidate->pa = (uint64)mem;
      candidate->state = MMAP_FILE_LOADING;
      continue;
    }

    candidate->next = mmap_file_cache.pages;
    mmap_file_cache.pages = candidate;
    release(&mmap_file_cache.lock);

    int result = fileread_at(file, candidate->pa, offset, PGSIZE);

    acquire(&mmap_file_cache.lock);
    if(result < 0){
      mmap_file_remove(candidate);
      wakeup(candidate);
      release(&mmap_file_cache.lock);
      kfree_page(mem);
      kfree(candidate);
      return 0;
    }
    candidate->state = MMAP_FILE_READY;
    candidate->mappings = 1;
    mmap_file_mark_dirty(candidate, dirty_length);
    kaddquota(mem);
    wakeup(candidate);
    release(&mmap_file_cache.lock);
    return mem;
  }
}

int
mmap_file_page_hold(struct file *file, uint64 offset, uint64 pa,
                    uint64 dirty_length)
{
  struct mmap_file_page *page;

  acquire(&mmap_file_cache.lock);
retry:
  page = mmap_file_find(file, offset);
  if(page && page->state != MMAP_FILE_READY){
    sleep(page, &mmap_file_cache.lock);
    goto retry;
  }
  if(page == 0 || page->pa != pa){
    release(&mmap_file_cache.lock);
    return -1;
  }
  page->mappings++;
  mmap_file_mark_dirty(page, dirty_length);
  kaddquota((void *)pa);
  release(&mmap_file_cache.lock);
  return 0;
}

int
mmap_file_page_writeback(struct file *file, uint64 offset, uint64 pa,
                         uint64 length)
{
  struct mmap_file_page *page;
  int result;

  if(length == 0)
    return 0;
  acquire(&mmap_file_cache.lock);
retry:
  page = mmap_file_find(file, offset);
  if(page && page->state != MMAP_FILE_READY){
    sleep(page, &mmap_file_cache.lock);
    goto retry;
  }
  if(page == 0 || page->pa != pa){
    release(&mmap_file_cache.lock);
    return -1;
  }
  mmap_file_mark_dirty(page, length);
  page->state = MMAP_FILE_WRITING;
  release(&mmap_file_cache.lock);

  result = filewrite_at(file, pa, offset, length);

  acquire(&mmap_file_cache.lock);
  page->state = MMAP_FILE_READY;
  wakeup(page);
  release(&mmap_file_cache.lock);
  return result < 0 ? -1 : 0;
}

int
mmap_file_page_put(struct file *file, uint64 offset, uint64 pa)
{
  struct mmap_file_page *page;
  uint64 dirty_length;
  int result = 0;

  acquire(&mmap_file_cache.lock);
retry:
  page = mmap_file_find(file, offset);
  if(page && page->state != MMAP_FILE_READY){
    sleep(page, &mmap_file_cache.lock);
    goto retry;
  }
  if(page == 0 || page->pa != pa || page->mappings < 1)
    panic("mmap_file_page_put");
  page->mappings--;
  if(page->mappings > 0){
    release(&mmap_file_cache.lock);
    kfree_page((void *)pa);
    return 0;
  }
  page->state = MMAP_FILE_EVICTING;
  dirty_length = page->dirty_length;
  release(&mmap_file_cache.lock);

  if(dirty_length > 0 &&
     filewrite_at(file, pa, offset, dirty_length) < 0)
    result = -1;

  acquire(&mmap_file_cache.lock);
  mmap_file_remove(page);
  wakeup(page);
  release(&mmap_file_cache.lock);

  kfree_page((void *)pa);
  kfree_page((void *)pa);
  kfree(page);
  return result;
}
