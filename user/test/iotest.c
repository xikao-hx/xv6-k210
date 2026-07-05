#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

static char wbuf[512];
static char rbuf[512];

static void
fill_buf(int round, int block)
{
  for(int i = 0; i < sizeof(wbuf); i++)
    wbuf[i] = 'A' + ((round + block + i) % 26);
}

static int
run_io_round(int round, int blocks)
{
  int fd;
  int t0;
  int t1;
  int n;

  t0 = uptime();

  fd = open("iotest.tmp", O_CREATE | O_TRUNC | O_RDWR);
  if(fd < 0) {
    printf("iotest: open write failed\n");
    return -1;
  }
  for(int i = 0; i < blocks; i++) {
    fill_buf(round, i);
    if(write(fd, wbuf, sizeof(wbuf)) != sizeof(wbuf)) {
      printf("iotest: write failed block=%d\n", i);
      close(fd);
      return -1;
    }
  }
  close(fd);

  fd = open("iotest.tmp", O_RDONLY);
  if(fd < 0) {
    printf("iotest: open read failed\n");
    return -1;
  }
  for(int i = 0; i < blocks; i++) {
    n = read(fd, rbuf, sizeof(rbuf));
    if(n != sizeof(rbuf)) {
      printf("iotest: read failed block=%d n=%d\n", i, n);
      close(fd);
      return -1;
    }
  }
  close(fd);

  t1 = uptime();
  return t1 - t0;
}

int
main(int argc, char *argv[])
{
  int rounds = 20;
  int interval = 10;
  int blocks = 8;
  int total = 0;
  int max = 0;

  if(argc > 1)
    rounds = atoi(argv[1]);
  if(argc > 2)
    interval = atoi(argv[2]);
  if(argc > 3)
    blocks = atoi(argv[3]);
  if(rounds <= 0)
    rounds = 20;
  if(interval < 0)
    interval = 0;
  if(blocks <= 0)
    blocks = 8;

  printf("iotest: pid=%d rounds=%d interval=%d blocks=%d\n",
         getpid(), rounds, interval, blocks);

  for(int i = 0; i < rounds; i++) {
    int before;
    int after;
    int io_ticks;
    int wake_delay;

    before = uptime();
    if(interval)
      sleep(interval);
    after = uptime();
    wake_delay = after - before - interval;

    io_ticks = run_io_round(i, blocks);
    if(io_ticks < 0)
      exit(1);

    total += io_ticks;
    if(io_ticks > max)
      max = io_ticks;

    printf("iotest: round=%d wake_delay=%d io_ticks=%d\n",
           i, wake_delay, io_ticks);
  }

  remove("iotest.tmp");
  printf("iotest: done avg_io=%d max_io=%d blocks=%d\n",
         total / rounds, max, blocks);
  exit(0);
}
