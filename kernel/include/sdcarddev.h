#ifndef __SDCARDDEV_H
#define __SDCARDDEV_H

#define SDCARD_IOCTL_SEEK      0x01  // arg: sector number (uint32)
#define SDCARD_IOCTL_TELL      0x02  // arg: pointer to uint32 for result
#define SDCARD_IOCTL_NSECTORS  0x03  // return total sector count (uint32)
#define SDCARD_IOCTL_INVALIDATE_CACHE 0x04  // drop FAT/buffer cache after raw writes

#endif
