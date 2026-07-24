#include "types.h"
#include "user.h"
#include "console.h"

static int failures;
static volatile int handler_seen;

static void
fail(char *name)
{
  printf("signaltest: %s FAILED\n", name);
  failures++;
}

static void __attribute__((section(".text.signal_handler")))
test_handler(int signum)
{
  handler_seen = signum;
}

static int
wait_status(int pid)
{
  int status = 0x7fffffff;
  int got = wait(&status);

  if(got != pid) {
    fail("wait pid");
    return 0x7fffffff;
  }
  return status;
}

static void
test_handler_and_ignore(void)
{
  sighandler_t old;
  int pid;

  handler_seen = 0;
  old = signal(SIGTERM, test_handler);
  if(old != SIG_DFL)
    fail("handler old default");
  if(sigsend(getpid(), SIGTERM) != 0)
    fail("handler send");
  if(handler_seen != SIGTERM)
    fail("handler delivery");
  if(signal(SIGTERM, SIG_DFL) != test_handler)
    fail("handler restore");

  pid = fork();
  if(pid == 0) {
    if(signal(SIGTERM, SIG_IGN) == SIG_ERR)
      exit(81);
    if(sigsend(getpid(), SIGTERM) != 0)
      exit(82);
    exit(23);
  }
  if(pid < 0 || wait_status(pid) != 23)
    fail("ignore");
}

static void
test_default_and_kill(void)
{
  int pid;

  pid = fork();
  if(pid == 0) {
    signal(SIGTERM, SIG_DFL);
    sigsend(getpid(), SIGTERM);
    exit(91);
  }
  if(pid < 0 || wait_status(pid) != -SIGTERM)
    fail("default terminate");

  pid = fork();
  if(pid == 0) {
    if(signal(SIGKILL, SIG_IGN) != SIG_ERR)
      exit(92);
    sigsend(getpid(), SIGKILL);
    exit(93);
  }
  if(pid < 0 || wait_status(pid) != -SIGKILL)
    fail("sigkill");
}

static void
test_invalid_inputs(void)
{
  if(signal(0, test_handler) != SIG_ERR)
    fail("signal zero");
  if(signal(NSIG, test_handler) != SIG_ERR)
    fail("signal range");
  if(signal(SIGTERM, (sighandler_t)0x4000000000UL) != SIG_ERR)
    fail("handler address");
  if(sigsend(999999, SIGTERM) != -1)
    fail("missing pid");
  if(sigsend(getpid(), 0) != -1)
    fail("send zero");
  if(sigsend(-getpid(), SIGTERM) != -1)
    fail("pgrp before support");
  if(sigreturn() != -1)
    fail("sigreturn context");
}

static void
test_fork_inherits_handler(void)
{
  int pid;

  if(signal(SIGINT, test_handler) == SIG_ERR) {
    fail("fork register");
    return;
  }
  pid = fork();
  if(pid == 0) {
    handler_seen = 0;
    if(sigsend(getpid(), SIGINT) != 0)
      exit(94);
    exit(handler_seen == SIGINT ? 0 : 95);
  }
  if(pid < 0 || wait_status(pid) != 0)
    fail("fork inherit");
  signal(SIGINT, SIG_DFL);
}

static void
test_exec_semantics(void)
{
  char *default_argv[] = { "signaltest", "exec-default", 0 };
  char *ignore_argv[] = { "signaltest", "exec-ignore", 0 };
  int pid;

  signal(SIGTERM, test_handler);
  pid = fork();
  if(pid == 0) {
    exec("signaltest", default_argv);
    exit(96);
  }
  if(pid < 0 || wait_status(pid) != -SIGTERM)
    fail("exec resets handler");

  signal(SIGTERM, SIG_IGN);
  pid = fork();
  if(pid == 0) {
    exec("signaltest", ignore_argv);
    exit(97);
  }
  if(pid < 0 || wait_status(pid) != 37)
    fail("exec preserves ignore");
  signal(SIGTERM, SIG_DFL);
}

static void
test_interrupt_sleep(void)
{
  int ready[2];
  int pid;
  char byte;

  if(pipe(ready) < 0) {
    fail("sleep ready pipe");
    return;
  }
  pid = fork();
  if(pid == 0) {
    close(ready[0]);
    signal(SIGTERM, test_handler);
    handler_seen = 0;
    write(ready[1], "r", 1);
    close(ready[1]);
    int rc = sleep(1000);
    exit(rc == -1 && handler_seen == SIGTERM ? 0 : 60);
  }
  close(ready[1]);
  if(pid < 0 || read(ready[0], &byte, 1) != 1 ||
     sigsend(pid, SIGTERM) < 0 || wait_status(pid) != 0)
    fail("interrupt sleep");
  close(ready[0]);
}

static void
test_interrupt_pipe_read(void)
{
  int ready[2];
  int data[2];
  int pid;
  char byte;

  if(pipe(ready) < 0 || pipe(data) < 0) {
    fail("pipe read setup");
    return;
  }
  pid = fork();
  if(pid == 0) {
    close(ready[0]);
    close(data[1]);
    signal(SIGTERM, test_handler);
    handler_seen = 0;
    write(ready[1], "r", 1);
    close(ready[1]);
    int rc = read(data[0], &byte, 1);
    exit(rc == -1 && handler_seen == SIGTERM ? 0 : 61);
  }
  close(ready[1]);
  close(data[0]);
  if(pid < 0 || read(ready[0], &byte, 1) != 1 ||
     sigsend(pid, SIGTERM) < 0 || wait_status(pid) != 0)
    fail("interrupt pipe read");
  close(ready[0]);
  close(data[1]);
}

static void
test_interrupt_pipe_write(void)
{
  int ready[2];
  int data[2];
  int pid;
  char byte;

  if(pipe(ready) < 0 || pipe(data) < 0) {
    fail("pipe write setup");
    return;
  }
  pid = fork();
  if(pid == 0) {
    char buf[1024];
    int rc;

    close(ready[0]);
    close(data[0]);
    memset(buf, 'x', sizeof(buf));
    signal(SIGTERM, test_handler);
    handler_seen = 0;
    write(ready[1], "r", 1);
    close(ready[1]);
    rc = write(data[1], buf, sizeof(buf));
    exit(rc > 0 && rc < (int)sizeof(buf) && handler_seen == SIGTERM ? 0 : 62);
  }
  close(ready[1]);
  close(data[1]);
  if(pid < 0 || read(ready[0], &byte, 1) != 1) {
    fail("interrupt pipe write ready");
  } else {
    sleep(5);
    if(sigsend(pid, SIGTERM) < 0 || wait_status(pid) != 0)
      fail("interrupt pipe write");
  }
  close(ready[0]);
  close(data[0]);
}

static void
test_interrupt_console_read(void)
{
  int ready[2];
  int pid;
  char byte;

  if(pipe(ready) < 0) {
    fail("console read setup");
    return;
  }
  pid = fork();
  if(pid == 0) {
    close(ready[0]);
    signal(SIGTERM, test_handler);
    handler_seen = 0;
    write(ready[1], "r", 1);
    close(ready[1]);
    int rc = read(0, &byte, 1);
    exit(rc == -1 && handler_seen == SIGTERM ? 0 : 63);
  }
  close(ready[1]);
  if(pid < 0 || read(ready[0], &byte, 1) != 1 ||
     sigsend(pid, SIGTERM) < 0 || wait_status(pid) != 0)
    fail("interrupt console read");
  close(ready[0]);
}

static void
test_interrupt_wait(void)
{
  int parent = getpid();
  int pid;
  int status;

  handler_seen = 0;
  signal(SIGINT, test_handler);
  pid = fork();
  if(pid == 0) {
    sleep(2);
    sigsend(parent, SIGINT);
    sleep(2);
    exit(25);
  }
  if(pid < 0) {
    fail("interrupt wait fork");
  } else {
    if(wait(&status) != -1 || handler_seen != SIGINT)
      fail("interrupt wait");
    if(wait(&status) != pid || status != 25)
      fail("wait retry");
  }
  signal(SIGINT, SIG_DFL);
}

static void
run_sleep_tests(void)
{
  printf("signaltest sleep: starting\n");
  test_interrupt_sleep();
  test_interrupt_pipe_read();
  test_interrupt_pipe_write();
  test_interrupt_console_read();
  test_interrupt_wait();
  if(failures == 0)
    printf("signaltest sleep: ALL PASSED\n");
  else
    printf("signaltest sleep: FAILED failures=%d\n", failures);
}

static void
run_pgrp_tests(void)
{
  int ready[2];
  int first;
  int second;
  int status1;
  int status2;
  char bytes[2];

  printf("signaltest pgrp: starting\n");
  if(setpgid(0, 0) < 0 || getpgrp() != getpid())
    fail("self process group");
  if(setpgid(-1, 1) != -1 || setpgid(999999, 1) != -1)
    fail("setpgid invalid target");
  if(pipe(ready) < 0) {
    fail("pgrp ready pipe");
    goto done;
  }

  first = fork();
  if(first == 0) {
    close(ready[0]);
    signal(SIGTERM, test_handler);
    handler_seen = 0;
    write(ready[1], "a", 1);
    sleep(1000);
    exit(handler_seen == SIGTERM ? 0 : 70);
  }
  if(first < 0 || setpgid(first, first) < 0) {
    fail("pgrp leader");
    goto close_ready;
  }

  second = fork();
  if(second == 0) {
    close(ready[0]);
    signal(SIGTERM, test_handler);
    handler_seen = 0;
    write(ready[1], "b", 1);
    sleep(1000);
    exit(handler_seen == SIGTERM ? 0 : 71);
  }
  if(second < 0 || setpgid(second, first) < 0) {
    fail("pgrp member");
    goto close_ready;
  }

  close(ready[1]);
  if(read(ready[0], &bytes[0], 1) != 1 ||
     read(ready[0], &bytes[1], 1) != 1)
    fail("pgrp children ready");
  if(sigsend(-first, SIGTERM) < 0)
    fail("pgrp send");
  if(wait(&status1) < 0 || wait(&status2) < 0 ||
     status1 != 0 || status2 != 0)
    fail("pgrp delivery");
  close(ready[0]);
  goto done;

close_ready:
  close(ready[0]);
  close(ready[1]);
done:
  if(failures == 0)
    printf("signaltest pgrp: ALL PASSED\n");
  else
    printf("signaltest pgrp: FAILED failures=%d\n", failures);
}

static void
run_tty_test(void)
{
  int old_foreground = ioctl(0, CONSOLE_IOCTL_GET_FG_PGRP, 0);

  printf("signaltest tty: starting\n");
  if(setpgid(0, 0) < 0 ||
     ioctl(0, CONSOLE_IOCTL_SET_FG_PGRP, getpgrp()) < 0) {
    printf("signaltest tty: setup FAILED\n");
    exit(1);
  }
  signal(SIGINT, test_handler);
  handler_seen = 0;
  printf("signaltest tty: READY\n");
  while(handler_seen == 0)
    ;
  if(handler_seen != SIGINT ||
     ioctl(0, CONSOLE_IOCTL_SET_FG_PGRP, old_foreground) < 0) {
    printf("signaltest tty: FAILED\n");
    failures++;
  } else {
    printf("signaltest tty: ALL PASSED\n");
  }
}

static void
run_raw_test(void)
{
  int old_mode = ioctl(0, CONSOLE_IOCTL_GET_MODE, 0);
  char input = 0;
  int got;

  printf("signaltest raw: starting\n");
  signal(SIGINT, test_handler);
  handler_seen = 0;
  if(ioctl(0, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_RAW) < 0) {
    printf("signaltest raw: setup FAILED\n");
    exit(1);
  }
  printf("signaltest raw: READY\n");
  got = read(0, &input, 1);
  ioctl(0, CONSOLE_IOCTL_SET_MODE, old_mode);
  if(got == 1 && input == 0x03 && handler_seen == 0)
    printf("signaltest raw: ALL PASSED\n");
  else {
    printf("signaltest raw: FAILED got=%d byte=%d signal=%d\n",
           got, input, handler_seen);
    failures++;
  }
}

static int
run_exec_mode(char *mode)
{
  if(strcmp(mode, "exec-default") == 0) {
    sigsend(getpid(), SIGTERM);
    return 98;
  }
  if(strcmp(mode, "exec-ignore") == 0) {
    sigsend(getpid(), SIGTERM);
    return 37;
  }
  return -1;
}

int
main(int argc, char **argv)
{
  int status;

  if(argc == 2) {
    status = run_exec_mode(argv[1]);
    if(status >= 0)
      exit(status);
    if(strcmp(argv[1], "sleep") == 0) {
      run_sleep_tests();
      exit(failures ? 1 : 0);
    }
    if(strcmp(argv[1], "pgrp") == 0) {
      run_pgrp_tests();
      exit(failures ? 1 : 0);
    }
    if(strcmp(argv[1], "tty") == 0) {
      run_tty_test();
      exit(failures ? 1 : 0);
    }
    if(strcmp(argv[1], "raw") == 0) {
      run_raw_test();
      exit(failures ? 1 : 0);
    }
  }

  printf("signaltest basic: starting\n");
  printf("signaltest: handler/ignore\n");
  test_handler_and_ignore();
  printf("signaltest: default/kill\n");
  test_default_and_kill();
  printf("signaltest: invalid inputs\n");
  test_invalid_inputs();
  printf("signaltest: fork inheritance\n");
  test_fork_inherits_handler();
  printf("signaltest: exec semantics\n");
  test_exec_semantics();

  if(failures == 0)
    printf("signaltest basic: ALL PASSED\n");
  else
    printf("signaltest basic: FAILED failures=%d\n", failures);
  exit(failures ? 1 : 0);
}
