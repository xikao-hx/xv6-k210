#include "types.h"
#include "file.h"
#include "sdcarddev.h"
#include "user.h"
#include "fcntl.h"

static uint32
checksum(uint8 *buf, int n)
{
  uint32 sum = 0;

  for (int i = 0; i < n; i++)
    sum = (sum << 5) - sum + buf[i];
  return sum;
}

static int
read_sector(int fd, uint32 sector, uint8 *buf)
{
  if (ioctl(fd, SDCARD_IOCTL_SEEK, sector) < 0)
    return -1;
  if (read(fd, buf, 512) != 512)
    return -1;
  return 0;
}

int
main(void)
{
  uint8 buf[512];
  uint32 nsectors = 0;
  uint32 pos = 0;
  int fd;
  int fails = 0;

  printf("SD card dev test\n");
  printf("================\n");

  fd = dev(O_RDWR, SDCARD_DEV, 0);
  if (fd < 0) {
    printf("FAIL: dev(SDCARD_DEV)\n");
    exit(1);
  }

  if (ioctl(fd, SDCARD_IOCTL_TELL, (uint64)&pos) < 0) {
    printf("FAIL: TELL\n");
    fails++;
  }

  if (ioctl(fd, SDCARD_IOCTL_NSECTORS, (uint64)&nsectors) < 0 || nsectors == 0) {
    printf("FAIL: NSECTORS\n");
    fails++;
  } else {
    printf("sectors: %u\n", nsectors);
  }

  if (read_sector(fd, 0, buf) < 0) {
    printf("FAIL: read sector 0\n");
    fails++;
  } else {
    printf("sector 0 checksum: 0x%x\n", checksum(buf, 512));
  }

  if (nsectors > 33) {
    if (read_sector(fd, 32, buf) < 0) {
      printf("FAIL: read sector 32\n");
      fails++;
    } else {
      printf("sector 32 checksum: 0x%x\n", checksum(buf, 512));
    }
  }

  if (ioctl(fd, SDCARD_IOCTL_SEEK, pos) < 0) {
    printf("FAIL: restore sector position\n");
    fails++;
  }

  close(fd);
  printf("SD card test %s, failures=%d\n", fails ? "FAILED" : "PASSED", fails);
  exit(fails ? 1 : 0);
}
