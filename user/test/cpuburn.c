#include "types.h"
#include "stat.h"
#include "user.h"

static volatile uint sink;

static void
burn_one_round(void)
{
  uint x = sink + 1;

  for(int i = 0; i < 200000; i++) {
    x = x * 1103515245 + 12345;
    x ^= x >> 7;
  }
  sink = x;
}

int
main(int argc, char *argv[])
{
  int ticks = 400;
  int verbose = 0;
  int start;
  int last;

  if(argc > 1)
    ticks = atoi(argv[1]);
  if(argc > 2)
    verbose = atoi(argv[2]);
  if(ticks <= 0)
    ticks = 400;

  printf("cpuburn: pid=%d ticks=%d verbose=%d\n", getpid(), ticks, verbose);
  start = uptime();
  last = start;

  while(uptime() - start < ticks) {
    burn_one_round();
    if(verbose && uptime() - last >= 50) {
      last = uptime();
      printf("cpuburn: pid=%d elapsed=%d\n", getpid(), last - start);
    }
  }

  printf("cpuburn: pid=%d done elapsed=%d sink=%x\n",
         getpid(), uptime() - start, sink);
  exit(0);
}
