
#include "proc.h"
#include "file.h"
#include "printf.h"

#define BUFSZ 4096
static struct {
  struct spinlock lock;
  char buf[BUFSZ];
  int sz;
  int off;
} stats;

int statscopyin(char*, int);
int statslock(char*, int);
  
int
statswrite(struct file *f, uint64 src, int n)
{
  (void)f;
  (void)src;
  (void)n;
  return -1;
}

int
statsread(struct file *f, uint64 dst, int n)
{
  int m;

  (void)f;
  acquire(&stats.lock);

  if(stats.sz == 0) {
#ifdef LAB_PGTBL
    stats.sz = statscopyin(stats.buf, BUFSZ);
#endif
#ifdef LAB_LOCK
    stats.sz = statslock(stats.buf, BUFSZ);
#endif
  }
  m = stats.sz - stats.off;

  if (m > 0) {
    if(m > n)
      m  = n;
    if(either_copyout(1, dst, stats.buf+stats.off, m) != -1) {
      stats.off += m;
    }
  } else {
    m = -1;
    stats.sz = 0;
    stats.off = 0;
  }
  release(&stats.lock);
  return m;
}

static int
statsopen(struct file *f)
{
  if(f->minor != 0 || f->writable)
    return -1;
  return 0;
}

static const struct file_operations stats_ops = {
  .open = statsopen,
  .read = statsread,
  .write = statswrite,
};

void
statsinit(void)
{
  initlock(&stats.lock, "stats");

  if(device_register(DEV_STATS, "stats", &stats_ops) < 0)
    panic("stats device register");
}
