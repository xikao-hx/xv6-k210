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