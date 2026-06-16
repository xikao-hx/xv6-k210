#ifndef __FILE_H
#define __FILE_H

// Device ABI constants are shared by kernel and user programs.
#include "types.h"
#include "dev.h"

struct proc;
struct pipe;
struct dirent;

struct file {
  enum { FD_NONE, FD_PIPE, FD_ENTRY, FD_DEVICE } type;
  int ref; // reference count
  char readable;
  char writable;
  struct pipe *pipe; // FD_PIPE
  struct dirent *ep; // FD_ENTRY
  uint off;          // FD_ENTRY
  short major;       // FD_DEVICE
  short minor;       // FD_DEVICE
};

// map major device number to device functions.
struct devsw {
  int (*read)(int, uint64, int);
  int (*write)(int, uint64, int);
  int (*ioctl)(int, uint64, uint64); // minor, cmd, arg
};

extern struct devsw devsw[];

struct file*    filealloc(void);
void            fileclose(struct file*);
struct file*    filedup(struct file*);
void            fileinit(void);
int             fileread(struct file*, uint64, int n);
int             filestat(struct file*, uint64 addr);
int             filewrite(struct file*, uint64, int n);
int             mmap_handler(uint64 va, uint64 scause);
int             find_vma(struct proc *p, uint64 va);
int             dirnext(struct file *f, uint64 addr);
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);

#endif
