#ifndef __MMAP_INTERNAL_H
#define __MMAP_INTERNAL_H

#include "mmap.h"

#define MAP_FAILED ((uint64)-1)

struct vma_area *mmap_vma_find(struct proc *, uint64);
uint64 mmap_map_create(struct proc *, uint64, uint64, int, int,
                       enum vma_type, struct mmap_object *,
                       const struct vma_ops *, uint64);
int mmap_populate(struct proc *, struct vma_area *);

struct mmap_object *mmap_object_create(enum vma_type, struct file *,
                                       int, uint64);
void mmap_object_put(struct mmap_object *);

int mmap_file_object_init(struct mmap_object *, struct file *);
void mmap_file_object_destroy(struct mmap_object *);
int mmap_file_writeback_range(struct proc *, struct vma_area *,
                              uint64, uint64);

int mmap_anon_object_init(struct mmap_object *, int);
void mmap_anon_object_destroy(struct mmap_object *);

int mmap_kbuf_object_init(struct mmap_object *, struct file *, uint64);
void mmap_kbuf_object_destroy(struct mmap_object *);

#endif
