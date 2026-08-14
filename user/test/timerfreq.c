// timerfreq: measure the real duration of one kernel timer tick on this board.
//
// The kernel programs the timer interrupt every INTERVAL `time`-CSR cycles
// (param.h), assuming a 390MHz time base -> 5ms/tick.  On real K210 hardware
// the time base differs, so a "tick" can be far longer than 5ms, silently
// distorting every tick-based delay/timeout (sleep(), uptime(), burn timeouts).
//
// Usage:
//   timerfreq [n]     sleep n ticks, print the uptime() delta around it.
//
// Run it on the board and time the wall-clock between the "begin" and "end"
// markers with a host-side stopwatch / miniterm timestamp:
//   tick_duration = wall_seconds / n
// User space cannot read the `time` CSR directly (scounteren is not set), so
// the host stopwatch is the reference clock.
#include "user.h"

int
main(int argc, char *argv[])
{
  int n = 100;
  int t0, t1;

  if (argc > 1)
    n = atoi(argv[1]);
  if (n < 1)
    n = 1;

  t0 = uptime();
  printf("timerfreq: begin n=%d t=%d\n", n, t0);
  sleep(n);
  t1 = uptime();
  printf("timerfreq: end   t=%d delta=%d ticks\n", t1, t1 - t0);
  printf("timerfreq: tick_duration_s = host_wall_time_s / %d\n", n);
  exit(0);
}
