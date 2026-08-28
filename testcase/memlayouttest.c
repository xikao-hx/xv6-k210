#include "param.h"
#include "fcntl.h"
#include "types.h"
#include "riscv.h"
#include "memlayout.h"
#include "user.h"

#define MAP_FAILED ((char *)-1)

static void
fail(char *test, char *why)
{
  printf("memlayouttest: %s failed: %s\n", test, why);
  exit(1);
}

static int __attribute__((noinline))
grow_stack(int depth, int seed)
{
  volatile char page_half[PGSIZE / 2];

  page_half[0] = seed;
  page_half[sizeof(page_half) - 1] = seed + depth;
  if(depth > 0 && grow_stack(depth - 1, seed + 1) < 0)
    return -1;
  if(page_half[0] != (char)seed ||
     page_half[sizeof(page_half) - 1] != (char)(seed + depth))
    return -1;
  return 0;
}

static void
top_down_test(void)
{
  char *high;
  char *low;
  char *reuse;

  high = mmap(0, PGSIZE, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  low = mmap(0, 2 * PGSIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(high == MAP_FAILED || low == MAP_FAILED)
    fail("top-down", "mmap");
  if((uint64)high != USER_STACK_BOTTOM - PGSIZE ||
     low != high - 2 * PGSIZE)
    fail("top-down", "address order");

  high[0] = 1;
  low[0] = 2;
  if(munmap(high, PGSIZE) < 0)
    fail("top-down", "unmap high");
  reuse = mmap(0, PGSIZE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(reuse != high)
    fail("top-down", "highest hole not reused");
  if(munmap(reuse, PGSIZE) < 0 || munmap(low, 2 * PGSIZE) < 0)
    fail("top-down", "cleanup");
}

static void
collision_and_boundary_test(void)
{
  char *brk;
  char *map;
  uint64 delta;
  uint64 oversized;

  brk = sbrk(0);
  map = mmap(0, PGSIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(map == MAP_FAILED)
    fail("collision", "mmap");

  delta = (uint64)map - (uint64)brk + 1;
  if(delta > 0x7fffffff || sbrk((int)delta) != (char *)-1)
    fail("collision", "heap crossed VMA");
  if(sbrk(0) != brk)
    fail("collision", "failed growth changed break");
  if(munmap(map, PGSIZE) < 0)
    fail("collision", "cleanup");

  oversized = USER_STACK_BOTTOM - PGROUNDUP((uint64)brk) + PGSIZE;
  if(oversized > 0x7fffffff)
    fail("collision", "test length overflow");
  map = mmap(0, (int)oversized, PROT_READ,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(map != MAP_FAILED)
    fail("collision", "mmap crossed heap");

  if(write(1, (void *)MAXUVA, 1) != -1)
    fail("boundary", "MAXUVA accepted by write");
  if(munmap((void *)MAXUVA, PGSIZE) != -1)
    fail("boundary", "MAXUVA accepted by munmap");
}

static void
stack_test(void)
{
  int pid;
  int status;

  if(grow_stack(12, 7) < 0)
    fail("stack", "growth corrupted data");

  pid = fork();
  if(pid < 0)
    fail("stack", "guard fork");
  if(pid == 0){
    *(volatile char *)USER_STACK_GUARD = 1;
    exit(0);
  }
  if(wait(&status) != pid || status == 0)
    fail("stack", "guard access survived");
}

static void
lifecycle_test(void)
{
  for(int i = 0; i < 16; i++){
    volatile int stack_value = i + 10;
    char *oldbrk;
    char *map;
    int pid;
    int status;

    oldbrk = sbrk(3 * PGSIZE);
    if(oldbrk == (char *)-1)
      fail("lifecycle", "grow heap");
    oldbrk[2 * PGSIZE] = i;
    if(sbrk(-3 * PGSIZE) == (char *)-1)
      fail("lifecycle", "shrink heap");

    map = mmap(0, 2 * PGSIZE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(map == MAP_FAILED)
      fail("lifecycle", "mmap");
    map[0] = i + 1;

    pid = fork();
    if(pid < 0)
      fail("lifecycle", "fork");
    if(pid == 0){
      map[0] = 99;
      stack_value = 99;
      if(grow_stack(4, i) < 0)
        exit(2);
      exit(0);
    }
    if(wait(&status) != pid || status != 0)
      fail("lifecycle", "child exit");
    if(map[0] != i + 1 || stack_value != i + 10)
      fail("lifecycle", "fork isolation");
    if(munmap(map, 2 * PGSIZE) < 0)
      fail("lifecycle", "munmap");
  }
}

static void
exec_test(void)
{
  char *argv[] = { "echo", "memlayout-exec", 0 };
  int pid;
  int status;

  pid = fork();
  if(pid < 0)
    fail("exec", "fork");
  if(pid == 0){
    exec("echo", argv);
    exit(1);
  }
  if(wait(&status) != pid || status != 0)
    fail("exec", "address-space replacement");
}

int
main(void)
{
  top_down_test();
  collision_and_boundary_test();
  stack_test();
  lifecycle_test();
  exec_test();
  printf("memlayouttest: all tests succeeded\n");
  exit(0);
}
