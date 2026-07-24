#ifndef __FILE_H
#define __FILE_H

// Device ABI constants are shared by kernel and user programs.
#include "types.h"
#include "dev.h"

struct proc;
struct pipe;
struct dirent;
struct file;
struct kbuf;

struct file_operations {
  int (*open)(struct file *);
  int (*read)(struct file *, uint64, int);
  int (*write)(struct file *, uint64, int);
  int (*ioctl)(struct file *, uint64, uint64);
  struct kbuf *(*mmap)(struct file *, uint64, uint64, int, int);
  int (*close)(struct file *);
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
  const struct file_operations *ops; // FD_DEVICE
  void *private_data;                // FD_DEVICE
};

struct device {
  const char *name;
  const struct file_operations *ops;
};

struct file*    filealloc(void);
void            fileclose(struct file*);
struct file*    filedup(struct file*);
void            fileinit(void);
int             file_parse_access_mode(int, char*, char*);
int             fileopen_device(struct file*, int, int, int);
int             fileread(struct file*, uint64, int n);
int             filestat(struct file*, uint64 addr);
int             fileioctl(struct file*, uint64, uint64);
int             filewrite(struct file*, uint64, int n);
int             fileread_at(struct file*, uint64, uint64, int);
int             filewrite_at(struct file*, uint64, uint64, int);
int             device_register(int, const char*,
                                const struct file_operations*);
const struct device* device_get(int);
int             dirnext(struct file *f, uint64 addr);
int             pipealloc(struct file**, struct file**);
void            pipeclose(struct pipe*, int);
int             piperead(struct pipe*, uint64, int);
int             pipewrite(struct pipe*, uint64, int);

#endif
