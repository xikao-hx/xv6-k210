#ifndef __DISK_H
#define __DISK_H

struct buf;

void disk_init(void);
void disk_read(struct buf *b);
void disk_write(struct buf *b);
void disk_intr(void);

#endif
