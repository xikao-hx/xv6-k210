#include "types.h"
#include "console.h"
#include "user.h"
#include "fcntl.h"

static int
parse_number(const char *s)
{
  int v = 0;

  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    for (s += 2; *s; s++) {
      char c = *s;
      if (c >= '0' && c <= '9')
        v = v * 16 + (c - '0');
      else if (c >= 'a' && c <= 'f')
        v = v * 16 + (c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v = v * 16 + (c - 'A' + 10);
      else
        break;
    }
    return v;
  }
  return atoi(s);
}


// useage: uartbaud <baud> [pattern] [count]
int
main(int argc, char *argv[])
{
  int baud = 500000;
  int pattern = 0x55;
  int count = 0;
  int fd, sent = 0;
  char buf[64];
  struct console_baud_info info;

  if (argc >= 2)
    baud = parse_number(argv[1]);
  if (argc >= 3)
    pattern = parse_number(argv[2]);
  if (argc >= 4)
    count = parse_number(argv[3]);

  if (baud < 9600 || baud > 5000000) {
    printf("uartbaud: bad baud %d\n", baud);
    exit(1);
  }

  fd = open("/dev/console", O_RDWR);
  if (fd < 0) {
    printf("uartbaud: open /dev/console failed\n");
    exit(1);
  }

  if (ioctl(fd, CONSOLE_IOCTL_GET_BAUD_INFO, (uint64)&info) < 0) {
    printf("uartbaud: get baud info failed\n");
    close(fd);
    exit(1);
  }
  printf("uartbaud: switch to %d baud, pattern 0x%x%x, count=%d\n",
         baud, (uint8)pattern >> 4, (uint8)pattern & 0xf, count);
  printf("uartbaud: clock=%u expected_div=%u\n",
         info.clock, info.clock / (uint32)baud - 1);
  printf("uartbaud: GO\n");

  if (ioctl(fd, CONSOLE_IOCTL_SET_BAUD, (uint64)baud) < 0) {
    printf("uartbaud: set baud failed\n");
    close(fd);
    exit(1);
  }

  if (ioctl(fd, CONSOLE_IOCTL_GET_BAUD_INFO, (uint64)&info) == 0)
    printf("uartbaud: div=%u actual=%u\n", info.div, info.actual);

  for (int i = 0; i < (int)sizeof(buf); i++)
    buf[i] = (char)pattern;

  while (count == 0 || sent < count) {
    int n = write(fd, buf, sizeof(buf));
    if (n <= 0)
      break;
    sent += n;
  }

  if (count != 0) {
    ioctl(fd, CONSOLE_IOCTL_SET_BAUD, (uint64)115200);
    printf("uartbaud: done, sent %d bytes, back to 115200\n", sent);
  }
  close(fd);
  return 0;
}