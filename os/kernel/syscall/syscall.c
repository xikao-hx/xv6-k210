#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "syscall.h"
#include "defs.h"

// Fetch the uint64 at addr from the current process.
int
fetchaddr(uint64 addr, uint64 *ip)
{
  struct proc *p = myproc();
  if(addr >= p->sz || addr+sizeof(uint64) > p->sz)
    return -1;
  if(copyin(p->pagetable, (char *)ip, addr, sizeof(*ip)) != 0)
    return -1;
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Returns length of string, not including nul, or -1 for error.
int
fetchstr(uint64 addr, char *buf, int max)
{
  struct proc *p = myproc();
  int err = copyinstr(p->pagetable, buf, addr, max);
  if(err < 0)
    return err;
  return strlen(buf);
}

static uint64
argraw(int n)
{
  struct proc *p = myproc();
  switch (n) {
  case 0:
    return p->trapframe->a0;
  case 1:
    return p->trapframe->a1;
  case 2:
    return p->trapframe->a2;
  case 3:
    return p->trapframe->a3;
  case 4:
    return p->trapframe->a4;
  case 5:
    return p->trapframe->a5;
  }
  panic("argraw");
  return -1;
}

// Fetch the nth 32-bit system call argument.
int
argint(int n, int *ip)
{
  *ip = argraw(n);
  return 0;
}

// Retrieve an argument as a pointer.
// Doesn't check for legality, since
// copyin/copyout will do that.
int
argaddr(int n, uint64 *ip)
{
  *ip = argraw(n);
  uint64 va = *ip;
  struct proc *p = myproc();
  pagetable_t pagetable = p->pagetable;

  if (walkaddr(pagetable, va) == 0) {
    if (PGROUNDUP(p->trapframe->sp) - 1 < va && va < p->sz) {
      if (uvmlazymalloc(pagetable, va) != 0) {
        // printf("argaddr: uvlazymalloc fail\n");
        return -1;
      }
    }
  }

  return 0;
}

// Fetch the nth word-sized system call argument as a null-terminated string.
// Copies into buf, at most max.
// Returns string length if OK (including nul), -1 if error.
int
argstr(int n, char *buf, int max)
{
  uint64 addr;
  if(argaddr(n, &addr) < 0)
    return -1;
  return fetchstr(addr, buf, max);
}

extern uint64 sys_chdir(void);
extern uint64 sys_close(void);
extern uint64 sys_dup(void);
extern uint64 sys_exec(void);
extern uint64 sys_exit(void);
extern uint64 sys_fork(void);
extern uint64 sys_fstat(void);
extern uint64 sys_getpid(void);
extern uint64 sys_kill(void);
extern uint64 sys_link(void);
extern uint64 sys_mkdir(void);
extern uint64 sys_mknod(void);
extern uint64 sys_open(void);
extern uint64 sys_pipe(void);
extern uint64 sys_read(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_sleep(void);
extern uint64 sys_unlink(void);
extern uint64 sys_wait(void);
extern uint64 sys_write(void);
extern uint64 sys_uptime(void);
extern uint64 sys_trace(void);
extern uint64 sys_sysinfo(void);
extern uint64 sys_symlink(void);
extern uint64 sys_mmap(void);
extern uint64 sys_munmap(void);
extern uint64 sys_dev(void);
extern uint64 sys_readdir(void);
extern uint64 sys_getcwd(void);
extern uint64 sys_remove(void);

static char *syscalls_name[] = {
[SYS_fork]    "fork",
[SYS_exit]    "exit",
[SYS_wait]    "wait",
[SYS_pipe]    "pipe",
[SYS_read]    "read",
[SYS_kill]    "kill",
[SYS_exec]    "exec",
[SYS_fstat]   "fstat",
[SYS_chdir]   "chdir",
[SYS_dup]     "dup",
[SYS_getpid]  "getpid",
[SYS_sbrk]    "sbrk",
[SYS_sleep]   "sleep",
[SYS_uptime]  "uptime",
[SYS_open]    "open",
[SYS_write]   "write",
[SYS_mknod]   "mknod",
[SYS_unlink]  "unlink",
[SYS_link]    "link",
[SYS_mkdir]   "mkdir",
[SYS_close]   "close",
[SYS_trace]   "trace",
[SYS_sysinfo] "sysinfo",
[SYS_symlink] "sys_symlink",
[SYS_mmap]    "mmap",
[SYS_munmap]  "munmap",
[SYS_dev]     "dev",
[SYS_readdir] "readdir",
[SYS_getcwd]  "getcwd",
[SYS_remove]  "remove",
};

static char syscalls_argc[] = {
[SYS_fork]    0,
[SYS_exit]    1,
[SYS_wait]    1,
[SYS_pipe]    1,
[SYS_read]    3,
[SYS_kill]    1,
[SYS_exec]    2,
[SYS_fstat]   2,
[SYS_chdir]   1,
[SYS_dup]     1,
[SYS_getpid]  0,
[SYS_sbrk]    1,
[SYS_sleep]   1,
[SYS_uptime]  0,
[SYS_open]    2,
[SYS_write]   3,
[SYS_mknod]   3,
[SYS_unlink]  1,
[SYS_link]    2,
[SYS_mkdir]   1,
[SYS_close]   1,
[SYS_trace]   1,
[SYS_sysinfo] 1,
[SYS_mmap]    2,
[SYS_munmap]  2,
[SYS_dev]     3,
[SYS_readdir] 2,
[SYS_getcwd]  1,
[SYS_remove]  1,
};

static uint64 (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_sleep]   sys_sleep,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
[SYS_trace]   sys_trace,
[SYS_sysinfo]   sys_sysinfo,
[SYS_symlink]   sys_symlink,
[SYS_mmap]      sys_mmap,
[SYS_munmap]    sys_munmap,
[SYS_dev]       sys_dev,
[SYS_readdir]   sys_readdir,
[SYS_getcwd]    sys_getcwd,
[SYS_remove]    sys_remove,
};

// 尝试获取字符串参数，如果成功返回1，否则返回0
int
getstrarg(int n, char *buf, int max)
{
  uint64 addr;
  
  if(argaddr(n, &addr) < 0)
    return 0;
  
  return fetchstr(addr, buf, max) >= 0;
}

void syscall_trace(struct proc *p, int num, uint64 *args);
void
syscall(void)
{
  int num;
  struct proc *p = myproc();
  // 获取系统调用参数
  uint64 args[6];
  args[0] = p->trapframe->a0;
  args[1] = p->trapframe->a1;
  args[2] = p->trapframe->a2;
  args[3] = p->trapframe->a3;
  args[4] = p->trapframe->a4;
  args[5] = p->trapframe->a5;

  num = p->trapframe->a7;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    if((p->trace_mask & (1 << num)) != 0) {
      syscall_trace(p, num, args);
    }
    p->trapframe->a0 = syscalls[num]();
  } else {
    printf("%d %s: unknown sys call %d\n",
           p->pid, p->name, num);
    p->trapframe->a0 = -1;
  }
}

void syscall_trace(struct proc *p, int num, uint64 *args)
{
  char buf[MAXPATH];    // 用于存储字符串参数

  printf("%d: syscall %s", p->pid, syscalls_name[num]);
  printf("(");
  // 根据系统调用类型特殊处理参数
  switch(num) {
    case SYS_exec: {
      // exec(path, argv[])
      // 第一个参数：路径字符串
      if(getstrarg(0, buf, sizeof(buf))) {
        printf("\"%s\", ", buf);
      } else {
        printf("%p, ", args[0]);
      }
      
      // 第二个参数：参数数组（指针数组）
      printf("[");
      uint64 argv_addr;
      if(argaddr(1, &argv_addr) >= 0) {
        uint64 arg_ptr;
        int arg_count = 0;
        // 最多显示5个参数
        for(int i = 0; i < 5 && arg_count < 10; i++) {
          if(copyin(p->pagetable, (char*)&arg_ptr, argv_addr + i*sizeof(uint64), sizeof(uint64)) >= 0) {   // 找到指针数组中某个指针存储的地址
            if(arg_ptr == 0) break;  // NULL终止
            if(fetchstr(arg_ptr, buf, sizeof(buf)) >= 0) {   // 然后用这个地址，找到对应的字符串
              if(arg_count > 0) printf(", ");
              printf("\"%s\"", buf);
              arg_count++;
            }
          }
        }
        if(arg_count >= 5) printf(", ...");
      }
      printf("]");
      break;
    }

    case SYS_open: {
      // open(path, mode)
      if(getstrarg(0, buf, sizeof(buf))) {
        printf("\"%s\", %d", buf, (int)args[1]);
      } else {
        printf("%p, %d", args[0], (int)args[1]);
      }
      break;
    }

    case SYS_mknod: {
      // mknod(path, major, minor)
      if(getstrarg(0, buf, sizeof(buf))) {
        printf("\"%s\", %d, %d", buf, (int)args[1], (int)args[2]);
      } else {
        printf("%p, %d, %d", args[0], (int)args[1], (int)args[2]);
      }
      break;
    }

    case SYS_unlink:
    case SYS_mkdir:
    case SYS_chdir: {
      // 这些系统调用只有一个字符串参数
      if(getstrarg(0, buf, sizeof(buf))) {
        printf("\"%s\"", buf);
      } else {
        printf("%p", args[0]);
      }
      break;
    }

    case SYS_link: {
      // link(old, new)
      if(getstrarg(0, buf, sizeof(buf))) {
        printf("\"%s\", ", buf);
        if(getstrarg(1, buf, sizeof(buf))) {
          printf("\"%s\"", buf);
        } else {
          printf("%p", args[1]);
        }
      } else {
        printf("%p, %p", args[0], args[1]);
      }
      break;
    }

    case SYS_write:
    case SYS_read: {
      // read/write(fd, buf, n)
      // 对于缓冲区，通常只显示地址而不打印内容
      printf("%d, %p, %d", (int)args[0], args[1], (int)args[2]);
      break;
    }

    default: {
      // 默认情况：按参数个数打印数字参数
      int arg_count = syscalls_argc[num];
      for(int i = 0; i < arg_count; i++) {
        printf("%d", (int)args[i]);
        if(i < arg_count - 1) {
          printf(", ");
        }
      }
    }
  }
  printf(")");
  printf(" -> %d\n", p->trapframe->a0);
}