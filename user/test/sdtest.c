#include "types.h"
#include "dev.h"
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
  uint32 shared_pos = 0;
  int fd;
  int independent_fd;
  int shared_fd;
  int child;
  int status;
  int fails = 0;

  printf("SD card dev test\n");
  printf("================\n");

  fd = open("/dev/sdcard", O_RDWR);
  if (fd < 0) {
    printf("FAIL: dev(DEV_SDCARD)\n");
    exit(1);
  }

  if (ioctl(fd, SDCARD_IOCTL_TELL, (uint64)&pos) < 0) {
    printf("FAIL: TELL\n");
    fails++;
  }

  independent_fd = open("/dev/sdcard", O_RDWR);
  if (independent_fd < 0 ||
      ioctl(fd, SDCARD_IOCTL_SEEK, 7) < 0 ||
      ioctl(independent_fd, SDCARD_IOCTL_TELL, (uint64)&shared_pos) < 0 ||
      shared_pos != 0) {
    printf("FAIL: independent open sector state\n");
    fails++;
  }
  if (independent_fd >= 0)
    close(independent_fd);

  shared_fd = dup(fd);
  if (shared_fd < 0 ||
      ioctl(shared_fd, SDCARD_IOCTL_SEEK, 9) < 0 ||
      ioctl(fd, SDCARD_IOCTL_TELL, (uint64)&shared_pos) < 0 ||
      shared_pos != 9) {
    printf("FAIL: dup shared sector state\n");
    fails++;
  }
  if (shared_fd >= 0)
    close(shared_fd);

  if (ioctl(fd, SDCARD_IOCTL_SEEK, 11) < 0) {
    printf("FAIL: prepare fork shared state\n");
    fails++;
  } else {
    child = fork();
    if (child < 0) {
      printf("FAIL: fork\n");
      fails++;
    } else if (child == 0) {
      exit(ioctl(fd, SDCARD_IOCTL_SEEK, 12) < 0);
    } else {
      if (wait(&status) < 0 || status != 0 ||
          ioctl(fd, SDCARD_IOCTL_TELL, (uint64)&shared_pos) < 0 ||
          shared_pos != 12) {
        printf("FAIL: fork shared sector state\n");
        fails++;
      }
    }
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
