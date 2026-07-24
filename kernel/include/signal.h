#ifndef __SIGNAL_H
#define __SIGNAL_H

#include "types.h"

#define NSIG       32

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGKILL     9
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGCHLD    17

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#ifndef __ASSEMBLER__

struct proc;

void signal_proc_init(struct proc *);
void signal_proc_fork(struct proc *, struct proc *);
void signal_reset_on_exec(struct proc *);
uint64 signal_set_handler(struct proc *, int, uint64);
int signal_send_pid(int, int);
int signal_send_pgrp(int, int);
int signal_send_locked(struct proc *, int);
int signal_pgrp_exists(int);
int signal_pending(struct proc *);
int signal_pending_locked(struct proc *);
void signal_deliver(struct proc *);
uint64 signal_sigreturn(struct proc *);
int signal_exit_status(struct proc *);

#endif

#endif
