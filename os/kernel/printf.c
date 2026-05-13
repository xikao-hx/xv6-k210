//
// formatted console output -- printf, panic.
//

#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "symtab.h"

volatile int panicked = 0;

// lock to avoid interleaving concurrent printf's.
static struct {
  struct spinlock lock;
  int locking;
} pr;

static char digits[] = "0123456789abcdef";

static void
printint(int xx, int base, int sign)
{
  char buf[16];
  int i;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  i = 0;
  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}

static void
printptr(uint64 x)
{
  int i;
  consputc('0');
  consputc('x');
  for (i = 0; i < (sizeof(uint64) * 2); i++, x <<= 4)
    consputc(digits[x >> (sizeof(uint64) * 8 - 4)]);
}

// Print to the console. only understands %d, %x, %p, %s.
void
printf(char *fmt, ...)
{
  va_list ap;
  int i, c, locking;
  char *s;

  locking = pr.locking;
  if(locking)
    acquire(&pr.lock);

  if (fmt == 0)
    panic("null fmt");

  va_start(ap, fmt);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 1);
      break;
    case 'p':
      printptr(va_arg(ap, uint64));
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&pr.lock);
}

void
printpage(pagetable_t pagetable, int layer)
{
  if (pagetable == 0)
    return;

  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    if (pte & PTE_V) {
      uint64 pa = PTE2PA(pte);  

      for (int j = 0; j < layer + 1; j++) {
        printf("..");
      }
      printf("%d: pte %p pa %p\n", i, (void*)pte, (void*)pa);


      if ((pte & (PTE_R | PTE_W | PTE_X)) == 0) {          
        printpage((pagetable_t)PTE2PA(pte), layer + 1);   
      }
    }
  }
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", (void*)pagetable);
  printpage(pagetable, 0);
}

/*
void
backtrace(void) 
{
  uint64 fp = r_fp();;
  printf("backtrace:\n");

  while((PGROUNDUP(fp) - PGROUNDDOWN(fp)) == PGSIZE) {
    uint64 ret_addr = *(uint64*)(fp - 8);
    printf("%p\n", ret_addr);
    fp = *(uint64*)(fp - 16);   // fp-16是指针的地址，*是要取出地址对应的值
  }
}
*/

// void backtrace(void) 
// {
//   uint64 fp = r_fp();
//   printf("backtrace:\n");

//   while ((PGROUNDUP(fp) - PGROUNDDOWN(fp)) == PGSIZE) {
//     uint64 ret_addr = *(uint64*)(fp - 8);
//     struct symtab_entry* sym = lookup_sym(ret_addr);
    
//     if (sym) {
//       // 无法获得行号
//       printf("函数名 + 偏移量：%s + %p\n", sym->name, ret_addr - sym->addr);
//     } else {
//       printf("  0x%p\n", ret_addr);
//     }

//     fp = *(uint64*)(fp - 16);
//     if (fp == 0) break;
//   }
// }

void
backtrace(void) 
{
  uint64 fp = r_fp();;
  printf("backtrace:\n");

  while((PGROUNDUP(fp) - PGROUNDDOWN(fp)) == PGSIZE) {
    uint64 ret_addr = *(uint64*)(fp - 8);
    printf("%p\n", ret_addr);
    fp = *(uint64*)(fp - 16);   // fp-16是指针的地址，*是要取出地址对应的值
  }
}

void
panic(char *s)
{
  pr.locking = 0;
  printf("panic: ");
  printf(s);
  printf("\n");
  backtrace(); 
  panicked = 1; // freeze uart output from other CPUs
  for(;;)
    ;
}

void
printfinit(void)
{
  initlock(&pr.lock, "pr");
  pr.locking = 1;
}
