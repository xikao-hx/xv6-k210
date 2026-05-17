//
// File-system system calls (K210/FAT32 version).
//

#include "types.h"
#include "riscv.h"
#include "param.h"
#include "memlayout.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "fat32.h"
#include "string.h"
#include "printf.h"

// Forward declarations (replaces defs.h which conflicts with fat32's dirlookup)
int             argint(int, int*);
int             argstr(int, char*, int);
int             argaddr(int, uint64 *);
int             fetchstr(uint64, char*, int);
struct proc*    myproc(void);
void            fileclose(struct file*);
struct file*    filealloc(void);
struct file*    filedup(struct file*);
int             fileread(struct file*, uint64, int n);
int             filestat(struct file*, uint64 addr);
int             filewrite(struct file*, uint64, int n);
int             dirnext(struct file *f, uint64 addr);
int             pipealloc(struct file**, struct file**);
int             copyout(pagetable_t, uint64, char *, uint64);
uint64          walkaddr(pagetable_t, uint64);
int             strlen(const char*);
struct dirent*  enameparent(char *path, char *name);
struct dirent*  ename(char *path);
struct dirent*  ealloc(struct dirent *dp, char *name, int attr);
int             exec(char *path, char **argv);
void*           kalloc(void);
void            kfree(void *);
int             fetchaddr(uint64, uint64*);
int             fetchstr(uint64, char*, int);
void            etrunc(struct dirent *entry);
int             eread(struct dirent *entry, int user_dst, uint64 dst, uint off, uint n);
int             ewrite(struct dirent *entry, int user_src, uint64 src, uint off, uint n);
void *          memset(void *, int, uint);
int             mappages(pagetable_t, uint64, uint64, uint64, int);
void            upg2ukpg(pagetable_t, pagetable_t, uint64, uint64);
void            uvmunmap(pagetable_t, uint64, uint64, int);
int             argint(int, int*);

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd] == 0){
      p->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argaddr(1, &p) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  if(argfd(0, 0, &f) < 0 || argaddr(1, &st) < 0)
    return -1;
  return filestat(f, st);
}

static struct dirent*
create(char *path, short type, int mode)
{
  struct dirent *ep, *dp;
  char name[FAT32_MAX_FILENAME + 1];

  if((dp = enameparent(path, name)) == 0)
    return 0;

  if (type == T_DIR) {
    mode = ATTR_DIRECTORY;
  } else if (mode & O_RDONLY) {
    mode = ATTR_READ_ONLY;
  } else {
    mode = 0;
  }

  elock(dp);
  if ((ep = ealloc(dp, name, mode)) == 0) {
    eunlock(dp);
    eput(dp);
    return 0;
  }

  if ((type == T_DIR && !(ep->attribute & ATTR_DIRECTORY)) ||
      (type == T_FILE && (ep->attribute & ATTR_DIRECTORY))) {
    eunlock(dp);
    eput(ep);
    eput(dp);
    return 0;
  }

  eunlock(dp);
  eput(dp);

  elock(ep);
  return ep;
}

uint64
sys_open(void)
{
  char path[FAT32_MAX_PATH];
  int fd, omode;
  struct file *f;
  struct dirent *ep;

  if(argstr(0, path, FAT32_MAX_PATH) < 0 || argint(1, &omode) < 0)
    return -1;

  if(omode & O_CREATE){
    ep = create(path, T_FILE, omode);
    if(ep == 0){
      return -1;
    }
  } else {
    if((ep = ename(path)) == 0){
      return -1;
    }
    elock(ep);
    if((ep->attribute & ATTR_DIRECTORY) && omode != O_RDONLY){
      eunlock(ep);
      eput(ep);
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if (f) {
      fileclose(f);
    }
    eunlock(ep);
    eput(ep);
    return -1;
  }

  if(!(ep->attribute & ATTR_DIRECTORY) && (omode & O_TRUNC)){
    etrunc(ep);
  }

  f->type = FD_ENTRY;
  f->off = (omode & O_APPEND) ? ep->file_size : 0;
  f->ep = ep;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  eunlock(ep);

  return fd;
}

uint64
sys_exec(void)
{
  char path[FAT32_MAX_PATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, FAT32_MAX_PATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0; i<MAXARG; i++){
    if(fetchaddr(uargv+sizeof(uint64)*i, &uarg) < 0)
      return -1;
    if(uarg == 0)
      break;
    argv[i] = kalloc();
    if(argv[i] == 0)
      panic("sys_exec kalloc");
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      return -1;
  }

  int ret = exec(path, argv);

  for(i=0; i<MAXARG && argv[i]; i++)
    kfree(argv[i]);
  return ret;
}

uint64
sys_mkdir(void)
{
  char path[FAT32_MAX_PATH];
  struct dirent *ep;

  if(argstr(0, path, FAT32_MAX_PATH) < 0 || (ep = create(path, T_DIR, 0)) == 0){
    return -1;
  }
  eunlock(ep);
  eput(ep);
  return 0;
}

uint64
sys_chdir(void)
{
  char path[FAT32_MAX_PATH];
  struct dirent *ep;
  struct proc *p = myproc();

  if(argstr(0, path, FAT32_MAX_PATH) < 0 || (ep = ename(path)) == 0){
    return -1;
  }
  elock(ep);
  if(!(ep->attribute & ATTR_DIRECTORY)){
    eunlock(ep);
    eput(ep);
    return -1;
  }
  eunlock(ep);
  eput(p->cwd);
  p->cwd = ep;
  return 0;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  if(argaddr(0, &fdarray) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if(copyout(p->pagetable, fdarray, (char*)&fd0, sizeof(fd0)) < 0 ||
     copyout(p->pagetable, fdarray+sizeof(fd0), (char *)&fd1, sizeof(fd1)) < 0){
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

// To open console/device.
uint64
sys_dev(void)
{
  int fd, omode;
  int major, minor;
  struct file *f;

  if(argint(0, &omode) < 0 || argint(1, &major) < 0 || argint(2, &minor) < 0){
    return -1;
  }

  if(omode & O_CREATE){
    panic("dev file on FAT");
  }

  if(major < 0 || major >= NDEV)
    return -1;

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    return -1;
  }

  f->type = FD_DEVICE;
  f->off = 0;
  f->ep = 0;
  f->major = major;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  return fd;
}

// To support ls command
uint64
sys_readdir(void)
{
  struct file *f;
  uint64 p;

  if(argfd(0, 0, &f) < 0 || argaddr(1, &p) < 0)
    return -1;
  // dirnext reads directory entries; used by ls
  // In K210, we use the FAT32 way
  if(f->type == FD_ENTRY){
    return dirnext(f, p);
  }
  return -1;
}

// get absolute cwd string
uint64
sys_getcwd(void)
{
  uint64 addr;
  if (argaddr(0, &addr) < 0)
    return -1;

  struct dirent *de = myproc()->cwd;
  char path[FAT32_MAX_PATH];
  char *s;
  int len;

  if (de->parent == 0) {
    s = "/";
  } else {
    s = path + FAT32_MAX_PATH - 1;
    *s = '\0';
    while (de->parent) {
      len = strlen(de->filename);
      s -= len;
      if (s <= path)
        return -1;
      strncpy(s, de->filename, len);
      *--s = '/';
      de = de->parent;
    }
  }

  if (copyout(myproc()->pagetable, addr, s, strlen(s) + 1) < 0)
    return -1;

  return 0;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct dirent *dp)
{
  struct dirent ep;
  int count;
  int ret;
  ep.valid = 0;
  ret = enext(dp, &ep, 2 * 32, &count);   // skip the "." and ".."
  return ret == -1;
}

uint64
sys_remove(void)
{
  char path[FAT32_MAX_PATH];
  struct dirent *ep;
  int len;
  if((len = argstr(0, path, FAT32_MAX_PATH)) <= 0)
    return -1;

  char *s = path + len - 1;
  while (s >= path && *s == '/') {
    s--;
  }
  if (s >= path && *s == '.' && (s == path || *--s == '/')) {
    return -1;
  }

  if((ep = ename(path)) == 0){
    return -1;
  }
  elock(ep);
  if((ep->attribute & ATTR_DIRECTORY) && !isdirempty(ep)){
      eunlock(ep);
      eput(ep);
      return -1;
  }
  elock(ep->parent);
  eremove(ep);
  eunlock(ep->parent);
  eunlock(ep);
  eput(ep);

  return 0;
}

uint64
sys_mmap(void)
{
  uint64 addr;
  int length;
  int prot;
  int flags;
  int vfd;
  struct file *vfile;
  uint64 err = 0xffffffffffffffff;
  struct proc *p = myproc();
  int offset = 0;

  if (argaddr(0, &addr) < 0 || argint(1, &length) < 0 || argint(2, &prot) < 0 ||
      argint(3, &flags) < 0 || argfd(4, &vfd, &vfile) < 0 || argint(5, &offset) < 0) {
    return err;
  }

  if (addr != 0 || offset != 0 || length < 0)
    return err;

  if (vfile->writable == 0 && (prot & PROT_WRITE) != 0 && flags == MAP_SHARED)
    return err;

  if (p->sz + length > MAXVA)
    return err;

  for (int i = 0; i < NVMA; i++) {
    if (p->vmas[i].used == 0) {
      p->vmas[i].used = 1;
      p->vmas[i].addr = p->sz;
      p->vmas[i].flags = flags;
      p->vmas[i].length = length;
      p->vmas[i].offset = offset;
      p->vmas[i].prot = prot;
      p->vmas[i].vfd = vfd;
      p->vmas[i].vfile = vfile;
      filedup(vfile);
      p->sz += length;

      return p->vmas[i].addr;
    }
  }

  return err;
}

// FAT32 stubs for unsupported syscalls

uint64
sys_mknod(void)
{
  // FAT32 has no device nodes; use sys_dev instead
  return -1;
}

uint64
sys_link(void)
{
  // FAT32 has no hard links
  return -1;
}

uint64
sys_symlink(void)
{
  // FAT32 has no symbolic links
  return -1;
}

uint64
sys_unlink(void)
{
  // unlink is an alias for remove in FAT32
  return sys_remove();
}

// mmap page fault handler (FAT32 version using eread)
int
mmap_handler(uint64 va, uint64 scause)
{
  struct proc *p = myproc();
  pagetable_t pagetable = p->pagetable;
  int i = 0;
  struct vma_area *vma;

  for (; i < NVMA; i ++) {
    vma = &p->vmas[i];
    if (vma->used && vma->addr <= va && va <= (vma->addr + vma->length - 1)) {
      break;
    }
  }

  if (i == NVMA) {
    return -1;
  }

  struct file *vfile = vma->vfile;
  if (scause == 13 && vfile->readable == 0) return -1;
  if (scause == 15 && vfile->writable == 0) return -1;

  void * pa = kalloc();
  if (pa == 0) {
    printf("mmap_handler: kalloc err\n");
    return -1;
  }
  memset(pa, 0, PGSIZE);

  elock(vfile->ep);
  int offset = vma->offset + PGROUNDDOWN(va - vma->addr);
  int readbytes = eread(vfile->ep, 0, (uint64)pa, offset, PGSIZE);
  if (readbytes == 0) {
    eunlock(vfile->ep);
    kfree(pa);
    return -1;
  }
  eunlock(vfile->ep);

  int pte_flags = PTE_U;
  if (vma->prot & PROT_READ) pte_flags |= PTE_R;
  if (vma->prot & PROT_WRITE) pte_flags |= PTE_W;
  if (vma->prot & PROT_EXEC) pte_flags |= PTE_X;

  if (mappages(pagetable, PGROUNDDOWN(va), (uint64)pa, PGSIZE, pte_flags) != 0) {
    kfree(pa);
    return -1;
  }

  upg2ukpg(pagetable, p->kpagetable, va, va + PGSIZE);
  return 0;
}

uint64
sys_munmap(void)
{
  uint64 addr;
  int length;
  struct proc *p = myproc();
  struct vma_area *vma;
  int i = 0;

  if (argaddr(0, &addr) < 0 || argint(1, &length) < 0) {
    return -1;
  }

  for (; i < NVMA; i ++) {
    vma = &p->vmas[i];
    if (vma->used && vma->length >= length) {
      if (vma->addr == addr) {
        vma->addr += length;
        vma->length -= length;
        break;
      } else if ((vma->addr + vma->length) == (addr + length)) {
        vma->length -= length;
        break;
      }
    }
  }

  if (i == NVMA) {
    return -1;
  }

  if (vma->vfile->writable && (vma->prot & PROT_WRITE) && vma->flags == MAP_SHARED) {
    filewrite(vma->vfile, addr, length);
  }

  uvmunmap(p->pagetable, PGROUNDDOWN(addr), length / PGSIZE, 1);
  // Note: ukvmdealloc not available, skip for now

  if (vma->length == 0) {
    fileclose(vma->vfile);
    vma->used = 0;
  }

  return 0;
}

int
find_vma(struct proc *p, uint64 va)
{
  for (int i = 0; i < NVMA; i ++) {
    struct vma_area *vma = &p->vmas[i];
    if (vma->used) {
      if (vma->addr <= va && va <= (vma->addr + vma->length - 1)) {
        return 0;
      }
    }
  }
  return -1;
}
