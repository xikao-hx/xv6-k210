#include "types.h"
#include "user.h"

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
