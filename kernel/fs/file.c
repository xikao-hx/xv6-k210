//
// Support functions for system calls that involve file descriptors.
// K210 (FAT32) version.
//

#include "printf.h"
#include "proc.h"
#include "file.h"
#include "fcntl.h"
#include "fat32.h"
#include "string.h"
#include "vm.h"

static struct device devices[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      memset(f, 0, sizeof(*f));
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_ENTRY){
    eput(ff.ep);
  } else if(ff.type == FD_DEVICE){
    if(ff.ops && ff.ops->close)
      ff.ops->close(&ff);
  }
}

int
device_register(int major, const char *name,
                const struct file_operations *ops)
{
  if(major <= 0 || major >= NDEV || name == 0 || ops == 0)
    return -1;
  if(devices[major].ops != 0)
    return -1;
  devices[major].name = name;
  devices[major].ops = ops;
  return 0;
}

const struct device *
device_get(int major)
{
  if(major <= 0 || major >= NDEV || devices[major].ops == 0)
    return 0;
  return &devices[major];
}

int
file_parse_access_mode(int flags, char *readable, char *writable)
{
  switch(flags & O_ACCMODE) {
  case O_RDONLY:
    *readable = 1;
    *writable = 0;
    return 0;
  case O_WRONLY:
    *readable = 0;
    *writable = 1;
    return 0;
  case O_RDWR:
    *readable = 1;
    *writable = 1;
    return 0;
  default:
    return -1;
  }
}

int
fileopen_device(struct file *f, int major, int minor, int flags)
{
  const struct device *dev;
  char readable;
  char writable;

  if(f == 0 || (flags & ~O_ACCMODE) != 0)
    return -1;
  if(file_parse_access_mode(flags, &readable, &writable) < 0)
    return -1;
  if((dev = device_get(major)) == 0)
    return -1;

  f->type = FD_DEVICE;
  f->readable = readable;
  f->writable = writable;
  f->major = major;
  f->minor = minor;
  f->ops = dev->ops;
  f->private_data = 0;
  if(f->ops->open && f->ops->open(f) < 0) {
    f->type = FD_NONE;
    f->ops = 0;
    f->private_data = 0;
    return -1;
  }
  return 0;
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;

  if(f->type == FD_ENTRY){
    elock(f->ep);
    estat(f->ep, &st);
    eunlock(f->ep);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  } else if(f->type == FD_DEVICE){
    // device stat not implemented for K210
    return -1;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(n < 0 || f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->ops == 0 || f->ops->read == 0)
      return -1;
    r = f->ops->read(f, addr, n);
  } else if(f->type == FD_ENTRY){
    elock(f->ep);
    if((r = eread(f->ep, 1, addr, f->off, n)) > 0)
      f->off += r;
    eunlock(f->ep);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(n < 0 || f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->ops == 0 || f->ops->write == 0)
      return -1;
    ret = f->ops->write(f, addr, n);
  } else if(f->type == FD_ENTRY){
    int i = 0;
    while(i < n){
      int n1 = n - i;
      elock(f->ep);
      if ((r = ewrite(f->ep, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      eunlock(f->ep);

      if(r < 0)
        break;
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

int
fileioctl(struct file *f, uint64 cmd, uint64 arg)
{
  if(f->type != FD_DEVICE || f->ops == 0 || f->ops->ioctl == 0)
    return -1;
  return f->ops->ioctl(f, cmd, arg);
}

int
dirnext(struct file *f, uint64 addr)
{
  if(f->readable == 0 || !(f->ep->attribute & ATTR_DIRECTORY))
    return -1;

  struct dirent de;
  struct stat st;
  int count = 0;
  int ret;
  elock(f->ep);
  while ((ret = enext(f->ep, &de, f->off, &count)) == 0) {  // skip empty entry
    f->off += count * 32;
  }
  eunlock(f->ep);
  if (ret == -1)
    return 0;

  f->off += count * 32;
  estat(&de, &st);
  if(copyout(myproc()->pagetable, addr, (char *)&st, sizeof(st)) < 0)
    return -1;

  return 1;
}
