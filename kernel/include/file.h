#ifndef __FILE_H
#define __FILE_H

// Device ABI constants are shared by kernel and user programs.
#include "types.h"

struct proc;
struct pipe;
struct dirent;
struct file;

// map major device number to device functions.
struct file_operations {
  int (*open)(struct file *);
  int (*close)(struct file *);
  int (*read)(struct file *, uint64, int);
  int (*write)(struct file *, uint64, int);
  int (*ioctl)(struct file *, uint64, uint64);
};

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
  const struct file_operations *ops;  // FD_DEVICE
  void *private_data;                 // FD_DEVICE
};

struct file*    filealloc(void);
int             fileopen(struct file *f);
int             fileclose(struct file*);
struct file*    filedup(struct file*);
void            fileinit(void);
int             fileread(struct file*, uint64, int n);
int             filestat(struct file*, uint64 addr);
int             filewrite(struct file*, uint64, int n);
int             fileioctl(struct file *f, uint64 cmd, uint64 arg);
int             mmap_handler(uint64 va, uint64 scause);
int             find_vma(struct proc *p, uint64 va);
int             dirnext(struct file *f, uint64 addr);
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);

#endif
