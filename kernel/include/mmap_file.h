#ifndef __MMAP_FILE_H
#define __MMAP_FILE_H

#include "types.h"

struct file;

void mmap_file_cache_init(void);
void *mmap_file_page_get(struct file *, uint64, uint64);
int mmap_file_page_hold(struct file *, uint64, uint64, uint64);
int mmap_file_page_writeback(struct file *, uint64, uint64, uint64);
int mmap_file_page_put(struct file *, uint64, uint64);

#endif
