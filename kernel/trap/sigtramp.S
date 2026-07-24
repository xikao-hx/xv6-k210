#include "syscall.h"

        .section sigtrampsec
        .globl sigtramp
sigtramp:
        li a7, SYS_sigreturn
        ecall
1:
        j 1b
