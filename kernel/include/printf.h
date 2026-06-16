#ifndef __PRINTF_H
#define __PRINTF_H

#include "riscv.h"

void printf(char*, ...);
void panic(char*) __attribute__((noreturn));
void printfinit(void);
void vmprint(pagetable_t pagetable);
void print_logo(void);
int  snprintf(char*, int, char*, ...);

#endif
