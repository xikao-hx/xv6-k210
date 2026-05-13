//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

#define MAX_SYMLINK_DEPTH 10

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

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if(argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

uint64
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], path[MAXPATH];
  uint off;

  if(argstr(0, path, MAXPATH) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  if((n = argstr(0, path, MAXPATH)) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if(ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)){
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_SYMLINK && !(omode & O_NOFOLLOW)) {
    for (int i = 0; i < MAX_SYMLINK_DEPTH; i ++) {
      if (readi(ip, 0, (uint64)path, 0, MAXPATH) != MAXPATH) {
        iunlock(ip);
        end_op();
        return -1;
      }

      iunlock(ip);
      ip = namei(path);
      if (ip == 0) {
        end_op();
        return -1;
      }

      ilock(ip);
      if (ip->type != T_SYMLINK) 
        break;
    }

    if (ip->type == T_SYMLINK) {
      iunlock(ip);
      end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if(ip->type == T_DEVICE){
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if((omode & O_TRUNC) && ip->type == T_FILE){
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  if((argstr(0, path, MAXPATH)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEVICE, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();
  
  begin_op();
  if(argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  if(argstr(0, path, MAXPATH) < 0 || argaddr(1, &uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv)){
      goto bad;
    }
    if(fetchaddr(uargv+sizeof(uint64)*i, (uint64*)&uarg) < 0){
      goto bad;
    }
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if(argv[i] == 0)
      goto bad;
    if(fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = exec(path, argv);

  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

 bad:
  for(i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
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

uint64 sys_symlink(void) 
{
  char path[MAXPATH], target[MAXPATH];
  struct inode *ip;

  if (argstr(0, target, MAXPATH) < 0 || argstr(1, path, MAXPATH) < 0) 
    return -1;

  begin_op();

  // 分配一个inode节点，文件类型是T_SYMLINK，create返回被锁定的inode
  ip = create(path, T_SYMLINK, 0, 0);
  if(ip == 0){
    end_op();
    return -1;
  }

  // 将inode数据块中写入target路径
  if(writei(ip, 0, (uint64)target, 0, MAXPATH) < MAXPATH) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  iunlockput(ip);
  end_op();
  return 0;
}

uint64 sys_mmap(void)
{
  uint64 addr;
  int length;
  int prot;
  int flags;
  int vfd;
  int offset;
  struct file *vfile;
  uint64 err = 0xffffffffffffffff;
  struct proc *p = myproc();
  
  // 获取系统调用参数
  if (argaddr(0, &addr) < 0 || argint(1, &length) < 0 || argint(2, &prot) < 0 || 
      argint(3, &flags) < 0 || argfd(4, &vfd, &vfile) < 0 || argint(5, &offset) < 0) {
    return err;
  }

  // 实验中提示addr和offset为0，简化程序可能发生的情况
  if (addr != 0 || offset != 0 || length < 0) {
    return err;
  }

  // 文件不可写则不允许用于PROT_WRITE权限时映射为MAP_SHARED
  if (vfile->writable == 0 && (prot & PROT_WRITE) != 0 && flags == MAP_SHARED) {
    return err;
  }

  if (p->sz + length > MAXVA) {
    return err;
  }

  // 遍历查找未使用的vma_area结构体
  for (int i = 0; i < NVMA; i++) {
    if (p->vmas[i].used == 0) {
      p->vmas[i].used = 1;
      p->vmas[i].addr = p->sz;  // 这个地方很重要
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

int
mmap_handler(uint64 va, uint64 scause)
{
  struct proc *p = myproc();
  pagetable_t pagetable = p->pagetable;
  int i = 0;
  struct vma_area *vma;

  // 根据地址查找属于哪一个vma
  for (; i < NVMA; i ++) {
    vma = &p->vmas[i];
    if (vma->used && vma->addr <= va && va <= (vma->addr + vma->length - 1)) {
      break;
    }
  }

  if (i == NVMA) {
    return -1;
  }

  // 分配物理地址，并读取文件内容
  struct file *vfile = vma->vfile;
  if (scause == 13 && vfile->readable == 0) return -1;
  if (scause == 15 && vfile->writable == 0) return -1;

  void * pa = kalloc();
  if (pa == 0) {
    printf("mmap_hander: kalloc err\n");
    return -1;
  }
  memset(pa, 0, PGSIZE);

  ilock(vfile->ip);
  int offset = vma->offset + PGROUNDDOWN(va - vma->addr);
  int readbytes = readi(vfile->ip, 0, (uint64)pa, offset, PGSIZE);
  if (readbytes == 0) {
    iunlock(vfile->ip);
    kfree(pa);
    return -1;
  }
  iunlock(vfile->ip);

  // 添加页面映射
  int pte_flags = PTE_U;
  if (vma->prot & PROT_READ) pte_flags |= PTE_R;
  if (vma->prot & PROT_WRITE) pte_flags |= PTE_W;
  if (vma->prot & PROT_EXEC) pte_flags |= PTE_X;
  // panic出现
  // if (mappages(pagetable, PGROUNDDOWN(va), (uint64)pa, PGSIZE, pte_flags) != 0)  
  
  if (mappages(pagetable, PGROUNDDOWN(va), (uint64)pa, PGSIZE, pte_flags) != 0) {
    kfree(pa);
    return -1;
  }

  // 同步修改进程的内核页表，这里要加上，否则有错误出现
  upg2ukpg(pagetable, p->kpagetable, va, va + PGSIZE);

  return 0;
}

extern uint64 sys_munmap(void)
{
  uint64 addr;
  int length;
  struct proc *p = myproc();
  struct vma_area *vma;
  
  int i = 0;

  if (argaddr(0, &addr) < 0 || argint(1, &length) < 0) {
    return -1;
  }

  // 根据提示：munmap的地址只能是起始和结束位置
  for (; i < NVMA; i ++) {
    vma = &p->vmas[i];
    //uint64 addr = vma->addr;
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

  // 写回文件系统
  if (vma->vfile->writable && (vma->prot & PROT_WRITE) && vma->flags == MAP_SHARED) {
    filewrite(vma->vfile, addr, length);
  }

  // 取消映射
  uvmunmap(p->pagetable, PGROUNDDOWN(addr), length / PGSIZE, 1);   
  ukvmdealloc(p->kpagetable, addr, vma->addr, 0);
  
  // 关闭文件
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
