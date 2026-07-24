#include "param.h"
#include "fcntl.h"
#include "dev.h"
#include "kbufdev.h"
#include "types.h"
#include "stat.h"
#include "riscv.h"
#include "fs.h"
#include "sysinfo.h"
#include "user.h"

void mmap_test();
void fork_test();
void fork_semantics_test();
void offset_unmap_test();
void copy_fault_test();
void anonymous_private_test();
void anonymous_shared_test();
void kbuf_device_test();
void file_shared_cache_test();
void permission_test();
void exec_test();
char buf[BSIZE];

#define MAP_FAILED ((char *) -1)

int
main(int argc, char *argv[])
{
  // sbrk(100);
  mmap_test();
  fork_test();
  fork_semantics_test();
  offset_unmap_test();
  copy_fault_test();
  anonymous_private_test();
  anonymous_shared_test();
  kbuf_device_test();
  file_shared_cache_test();
  permission_test();
  exec_test();
  printf("mmaptest: all tests succeeded\n");
  exit(0);
}

char *testname = "???";

void
err(char *why)
{
  printf("mmaptest: %s failed: %s, pid=%d\n", testname, why, getpid());
  exit(1);
}

//
// check the content of the two mapped pages.
//
void
_v1(char *p)
{
  int i;
  for (i = 0; i < PGSIZE*2; i++) {
    if (i < PGSIZE + (PGSIZE/2)) {
      if (p[i] != 'A') {
        printf("mismatch at %d, wanted 'A', got 0x%x\n", i, p[i]);
        err("v1 mismatch (1)");
      }
    } else {
      if (p[i] != 0) {
        printf("mismatch at %d, wanted zero, got 0x%x\n", i, p[i]);
        err("v1 mismatch (2)");
      }
    }
  }
}

//
// create a file to be mapped, containing
// 1.5 pages of 'A' and half a page of zeros.
//
void
makefile(const char *f)
{
  int i;
  int n = PGSIZE/BSIZE;

  unlink(f);
  int fd = open(f, O_WRONLY | O_CREATE);
  if (fd == -1)
    err("open");
  memset(buf, 'A', BSIZE);
  // write 1.5 page
  for (i = 0; i < n + n/2; i++) {
    if (write(fd, buf, BSIZE) != BSIZE)
      err("write 0 makefile");
  }
  if (close(fd) == -1)
    err("close");
}

void
mmap_test(void)
{
  int fd;
  int i;
  const char * const f = "mmap.dur";
  printf("mmap_test starting\n");
  testname = "mmap_test";

  //
  // create a file with known content, map it into memory, check that
  // the mapped memory has the same bytes as originally written to the
  // file.
  //
  makefile(f);
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open");

  printf("test mmap f\n");
  //
  // this call to mmap() asks the kernel to map the content
  // of open file fd into the address space. the first
  // 0 argument indicates that the kernel should choose the
  // virtual address. the second argument indicates how many
  // bytes to map. the third argument indicates that the
  // mapped memory should be read-only. the fourth argument
  // indicates that, if the process modifies the mapped memory,
  // that the modifications should not be written back to
  // the file nor shared with other processes mapping the
  // same file (of course in this case updates are prohibited
  // due to PROT_READ). the fifth argument is the file descriptor
  // of the file to be mapped. the last argument is the starting
  // offset in the file.
  //
  char *p = mmap(0, PGSIZE*2, PROT_READ, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (1)");
  _v1(p);
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (1)");

  printf("test mmap f: OK\n");
    
  printf("test mmap private\n");
  // should be able to map file opened read-only with private writable
  // mapping
  p = mmap(0, PGSIZE*2, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (2)");
  if (close(fd) == -1)
    err("close");
  _v1(p);
  for (i = 0; i < PGSIZE*2; i++)
    p[i] = 'Z';
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (2)");

  printf("test mmap private: OK\n");
    
  printf("test mmap read-only\n");
    
  // check that mmap doesn't allow read/write mapping of a
  // file opened read-only.
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open");
  p = mmap(0, PGSIZE*3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p != MAP_FAILED)
    err("mmap call should have failed");
  if (close(fd) == -1)
    err("close");

  printf("test mmap read-only: OK\n");
    
  printf("test mmap read/write\n");
  
  // check that mmap does allow read/write mapping of a
  // file opened read/write.
  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  p = mmap(0, PGSIZE*3, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED)
    err("mmap (3)");
  if (close(fd) == -1)
    err("close");

  // check that the mapping still works after close(fd).
  _v1(p);

  // write the mapped memory.
  for (i = 0; i < PGSIZE*2; i++)
    p[i] = 'Z';

  // unmap just the first two of three pages of mapped memory.
  if (munmap(p, PGSIZE*2) == -1)
    err("munmap (3)");
  
  printf("test mmap read/write: OK\n");
  
  printf("test mmap dirty\n");
  
  // check that the writes to the mapped memory were
  // written to the file.
  if ((fd = open(f, O_RDWR)) == -1)
    err("open");
  for (i = 0; i < PGSIZE + (PGSIZE/2); i++){
    char b;
    if (read(fd, &b, 1) != 1)
      err("read (1)");
    if (b != 'Z')
      err("file does not contain modifications");
  }
  if (close(fd) == -1)
    err("close");

  printf("test mmap dirty: OK\n");

  printf("test not-mapped unmap\n");
  
  // unmap the rest of the mapped memory.
  if (munmap(p+PGSIZE*2, PGSIZE) == -1)
    err("munmap (4)");

  printf("test not-mapped unmap: OK\n");
    
  printf("test mmap two files\n");
  
  //
  // mmap two files at the same time.
  //
  int fd1;
  if((fd1 = open("mmap1", O_RDWR|O_CREATE)) < 0)
    err("open mmap1");
  if(write(fd1, "12345", 5) != 5)
    err("write mmap1");
  char *p1 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd1, 0);
  if(p1 == MAP_FAILED)
    err("mmap mmap1");
  close(fd1);
  unlink("mmap1");

  int fd2;
  if((fd2 = open("mmap2", O_RDWR|O_CREATE)) < 0)
    err("open mmap2");
  if(write(fd2, "67890", 5) != 5)
    err("write mmap2");
  char *p2 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd2, 0);
  if(p2 == MAP_FAILED)
    err("mmap mmap2");
  close(fd2);
  unlink("mmap2");

  if(memcmp(p1, "12345", 5) != 0)
    err("mmap1 mismatch");
  if(memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch");

  munmap(p1, PGSIZE);
  if(memcmp(p2, "67890", 5) != 0)
    err("mmap2 mismatch (2)");
  munmap(p2, PGSIZE);
  
  printf("test mmap two files: OK\n");
  
  printf("mmap_test: ALL OK\n");
}

//
// mmap a file, then fork.
// check that the child sees the mapped file.
//
void
fork_test(void)
{
  int fd;
  int pid;
  const char * const f = "mmap.dur";
  
  printf("fork_test starting\n");
  testname = "fork_test";
  
  // mmap the file twice.
  makefile(f);
  if ((fd = open(f, O_RDONLY)) == -1)
    err("open");
  unlink(f);
  char *p1 = mmap(0, PGSIZE*2, PROT_READ, MAP_SHARED, fd, 0);
  if (p1 == MAP_FAILED)
    err("mmap (4)");
  char *p2 = mmap(0, PGSIZE*2, PROT_READ, MAP_SHARED, fd, 0);
  if (p2 == MAP_FAILED)
    err("mmap (5)");

  // read just 2nd page.
  if(*(p1+PGSIZE) != 'A')
    err("fork mismatch (1)");

  if((pid = fork()) < 0)
    err("fork");
  if (pid == 0) {
    _v1(p1);
    munmap(p1, PGSIZE); // just the first page
    exit(0); // tell the parent that the mapping looks OK.
  }

  int status = -1;
  wait(&status);

  if(status != 0){
    printf("fork_test failed\n");
    exit(1);
  }

  // check that the parent's mappings are still there.
  _v1(p1);
  _v1(p2);

  printf("fork_test OK\n");
}

void
fork_semantics_test(void)
{
  char *private;
  char *shared;
  int fd;
  int pid;
  int status;

  printf("fork_semantics_test starting\n");
  testname = "fork_semantics_test";
  unlink("mmap.fork");
  fd = open("mmap.fork", O_RDWR | O_CREATE);
  if(fd < 0 || write(fd, "A", 1) != 1)
    err("fork semantics file");
  private = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  shared = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if(private == MAP_FAILED || shared == MAP_FAILED)
    err("fork semantics mmap");
  if(private[0] != 'A' || shared[0] != 'A')
    err("fork semantics fault-in");

  pid = fork();
  if(pid < 0)
    err("fork semantics fork");
  if(pid == 0){
    private[0] = 'P';
    shared[0] = 'S';
    if(private[0] != 'P' || shared[0] != 'S')
      exit(2);
    exit(0);
  }
  wait(&status);
  if(status != 0 || private[0] != 'A' || shared[0] != 'S')
    err("fork private/shared behavior");
  munmap(private, PGSIZE);
  munmap(shared, PGSIZE);
  unlink("mmap.fork");
  printf("fork_semantics_test OK\n");
}

void
make_pattern_file(const char *path)
{
  unlink(path);
  int fd = open(path, O_WRONLY | O_CREATE);
  if(fd < 0)
    err("open pattern file");
  for(int page = 0; page < 3; page++){
    memset(buf, 'A' + page, sizeof(buf));
    for(int i = 0; i < PGSIZE / BSIZE; i++){
      if(write(fd, buf, sizeof(buf)) != sizeof(buf))
        err("write pattern file");
    }
  }
  close(fd);
}

void
offset_unmap_test(void)
{
  char *p;
  char *p1;
  char *p2;
  char *low;
  int fd;

  printf("offset_unmap_test starting\n");
  testname = "offset_unmap_test";
  make_pattern_file("mmap.pattern");

  fd = open("mmap.pattern", O_RDONLY);
  if(fd < 0)
    err("open offset file");
  p = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, PGSIZE);
  close(fd);
  if(p == MAP_FAILED || p[0] != 'B' || p[PGSIZE - 1] != 'B')
    err("non-zero offset");
  if(munmap(p, PGSIZE) < 0)
    err("offset munmap");

  fd = open("mmap.pattern", O_RDONLY);
  p = mmap(0, PGSIZE * 3, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if(p == MAP_FAILED || p[0] != 'A' || p[PGSIZE * 2] != 'C')
    err("middle map");
  if(munmap(p + PGSIZE, PGSIZE) < 0)
    err("middle munmap");
  if(p[0] != 'A' || p[PGSIZE * 2] != 'C')
    err("middle split content");
  if(munmap(p, PGSIZE) < 0 || munmap(p + PGSIZE * 2, PGSIZE) < 0)
    err("split tail cleanup");

  fd = open("mmap.pattern", O_RDONLY);
  p1 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  p2 = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, PGSIZE);
  close(fd);
  if(p1 == MAP_FAILED || p2 == MAP_FAILED)
    err("cross map");
  low = p1 < p2 ? p1 : p2;
  if(low + PGSIZE != (p1 < p2 ? p2 : p1))
    err("cross maps not adjacent");
  if(munmap(low, PGSIZE * 2) < 0)
    err("cross-vma munmap");

  unlink("mmap.pattern");
  printf("offset_unmap_test OK\n");
}

void
copy_fault_test(void)
{
  char *p;
  char *name;
  int fd;
  int out;

  printf("copy_fault_test starting\n");
  testname = "copy_fault_test";

  unlink("copy.src");
  fd = open("copy.src", O_WRONLY | O_CREATE);
  if(fd < 0 || write(fd, "copy", 4) != 4)
    err("create copy source");
  close(fd);

  unlink("copy.backing");
  fd = open("copy.backing", O_RDWR | O_CREATE);
  if(fd < 0 || write(fd, "0000", 4) != 4)
    err("create copy backing");
  p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  close(fd);
  if(p == MAP_FAILED)
    err("copy mmap");

  fd = open("copy.src", O_RDONLY);
  if(fd < 0 || read(fd, p, 4) != 4)
    err("copyout fault-in");
  close(fd);
  unlink("copy.out");
  out = open("copy.out", O_WRONLY | O_CREATE);
  if(out < 0 || write(out, p, 4) != 4)
    err("copyin mapped buffer");
  close(out);
  if(munmap(p, PGSIZE) < 0)
    err("copy munmap");

  unlink("copy.name");
  fd = open("copy.name", O_RDWR | O_CREATE);
  if(fd < 0 || write(fd, "copy.out", 9) != 9)
    err("create mapped name");
  name = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if(name == MAP_FAILED)
    err("name mmap");
  fd = open(name, O_RDONLY);
  if(fd < 0)
    err("copyinstr fault-in");
  if(read(fd, buf, 4) != 4 || memcmp(buf, "copy", 4) != 0)
    err("copy output content");
  close(fd);
  munmap(name, PGSIZE);

  unlink("copy.src");
  unlink("copy.backing");
  unlink("copy.name");
  unlink("copy.out");
  printf("copy_fault_test OK\n");
}

void
anonymous_private_test(void)
{
  const int length = 16 * 1024 * 1024;
  char *p;
  char *bad;
  int fd;
  int pid;
  int status;

  printf("anonymous_private_test starting\n");
  testname = "anonymous_private_test";

  bad = mmap(0, PGSIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
  if(bad != MAP_FAILED)
    err("anonymous fd accepted");
  bad = mmap(0, PGSIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, PGSIZE);
  if(bad != MAP_FAILED)
    err("anonymous offset accepted");

  // This VMA is larger than available physical memory. Creation can only
  // succeed when mmap reserves virtual addresses without allocating data pages.
  p = mmap(0, length, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(p == MAP_FAILED)
    err("large private anonymous mmap");
  if(p[0] != 0 || p[17 * PGSIZE] != 0 || p[length - 1] != 0)
    err("anonymous page not zero");
  p[0] = 'A';
  p[17 * PGSIZE] = 'Q';
  p[length - 1] = 'Z';

  unlink("anon.src");
  fd = open("anon.src", O_WRONLY | O_CREATE);
  if(fd < 0 || write(fd, "anon", 4) != 4)
    err("anonymous copy source");
  close(fd);
  fd = open("anon.src", O_RDONLY);
  if(fd < 0 || read(fd, p + 2 * PGSIZE, 4) != 4)
    err("anonymous copyout fault-in");
  close(fd);
  if(memcmp(p + 2 * PGSIZE, "anon", 4) != 0)
    err("anonymous copy content");
  unlink("anon.src");

  pid = fork();
  if(pid < 0)
    err("anonymous fork");
  if(pid == 0){
    if(p[0] != 'A' || p[PGSIZE] != 0)
      exit(2);
    p[0] = 'C';
    p[PGSIZE] = 'D';
    if(p[0] != 'C' || p[PGSIZE] != 'D')
      exit(3);
    exit(0);
  }
  wait(&status);
  if(status != 0 || p[0] != 'A' || p[PGSIZE] != 0)
    err("anonymous private COW");
  if(munmap(p, length) < 0)
    err("anonymous munmap");
  printf("anonymous_private_test OK\n");
}

void
anonymous_shared_test(void)
{
  char *p;
  int pid;
  int status;

  printf("anonymous_shared_test starting\n");
  testname = "anonymous_shared_test";
  p = mmap(0, PGSIZE * 3, PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if(p == MAP_FAILED)
    err("shared anonymous mmap");

  // Page 0 is resident before fork. Page 1 is first faulted by the child,
  // and page 2 is first faulted by the parent after the child exits.
  p[0] = 'A';
  pid = fork();
  if(pid < 0)
    err("shared anonymous fork");
  if(pid == 0){
    if(p[0] != 'A' || p[PGSIZE] != 0)
      exit(2);
    p[0] = 'B';
    p[PGSIZE] = 'C';
    exit(0);
  }
  wait(&status);
  if(status != 0 || p[0] != 'B' || p[PGSIZE] != 'C')
    err("shared anonymous visibility");
  if(p[2 * PGSIZE] != 0)
    err("shared anonymous late zero page");
  p[2 * PGSIZE] = 'D';

  // A second child inherits all resident pages. Its exit must not invalidate
  // the parent's mappings or the anonymous object's page ownership.
  pid = fork();
  if(pid < 0)
    err("shared anonymous second fork");
  if(pid == 0){
    if(p[0] != 'B' || p[PGSIZE] != 'C' || p[2 * PGSIZE] != 'D')
      exit(3);
    p[2 * PGSIZE] = 'E';
    exit(0);
  }
  wait(&status);
  if(status != 0 || p[2 * PGSIZE] != 'E')
    err("shared anonymous exit lifetime");
  if(munmap(p, PGSIZE * 3) < 0)
    err("shared anonymous munmap");
  printf("anonymous_shared_test OK\n");
}

void
kbuf_device_test(void)
{
  struct kbuf_ioctl_region region;
  char *bad;
  char *p;
  int fd;
  int rofd;
  int pid;
  int status;

  printf("kbuf_device_test starting\n");
  testname = "kbuf_device_test";
  fd = dev(O_RDWR, DEV_KBUF, 0);
  if(fd < 0)
    err("kbuf dev open");

  bad = mmap(0, 3 * PGSIZE, PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0);
  if(bad != MAP_FAILED)
    err("kbuf oversized mmap accepted");
  bad = mmap(0, PGSIZE, PROT_READ, MAP_SHARED, fd, 1);
  if(bad != MAP_FAILED)
    err("kbuf unaligned offset accepted");
  bad = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
  if(bad != MAP_FAILED)
    err("kbuf private mmap accepted");

  rofd = dev(O_RDONLY, DEV_KBUF, 0);
  if(rofd < 0)
    err("kbuf read-only open");
  bad = mmap(0, PGSIZE, PROT_READ | PROT_WRITE,
             MAP_SHARED, rofd, 0);
  if(bad != MAP_FAILED)
    err("kbuf fd permission ignored");
  close(rofd);

  p = mmap(0, 2 * PGSIZE, PROT_READ | PROT_WRITE,
           MAP_SHARED, fd, 0);
  if(p == MAP_FAILED)
    err("kbuf mmap");
  region.offset = PGSIZE - 8;
  region.length = 16;
  region.value = 0x5a;
  if(ioctl(fd, KBUF_IOCTL_FILL, (uint64)&region) < 0)
    err("kbuf driver fill");
  for(int i = 0; i < 16; i++){
    if((uchar)p[PGSIZE - 8 + i] != region.value)
      err("kbuf driver to user visibility");
  }

  region.offset = PGSIZE + 64;
  region.length = 32;
  region.value = 0x36;
  for(int i = 0; i < 32; i++)
    p[region.offset + i] = region.value;
  if(ioctl(fd, KBUF_IOCTL_CHECK, (uint64)&region) < 0)
    err("kbuf user to driver visibility");

  close(fd);
  if((uchar)p[PGSIZE - 1] != 0x5a)
    err("kbuf mapping after close");
  pid = fork();
  if(pid < 0)
    err("kbuf fork");
  if(pid == 0){
    p[0] = 'K';
    if(munmap(p, 2 * PGSIZE) < 0)
      exit(2);
    exit(0);
  }
  wait(&status);
  if(status != 0 || p[0] != 'K')
    err("kbuf fork sharing and release");
  if(munmap(p, 2 * PGSIZE) < 0)
    err("kbuf munmap");
  printf("kbuf_device_test OK\n");
}

void
file_shared_cache_test(void)
{
  char *p;
  char *q;
  char token = 'x';
  char observed;
  int ready[2];
  int done[2];
  int fd;
  int child_fd;
  int pid;
  int status;
  struct sysinfo before;
  struct sysinfo after;
  char *name = "mmap-cache";

  printf("file_shared_cache_test starting\n");
  testname = "file_shared_cache_test";
  unlink(name);
  fd = open(name, O_CREATE | O_TRUNC | O_RDWR);
  if(fd < 0)
    err("cache test create");
  memset(buf, 0, sizeof(buf));
  for(int written = 0; written < PGSIZE; written += sizeof(buf)){
    int count = PGSIZE - written;
    if(count > sizeof(buf))
      count = sizeof(buf);
    if(write(fd, buf, count) != count)
      err("cache test initialize");
  }
  p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(p == MAP_FAILED)
    err("cache parent mmap");
  if(pipe(ready) < 0 || pipe(done) < 0)
    err("cache test pipe");

  pid = fork();
  if(pid < 0)
    err("cache test fork");
  if(pid == 0){
    close(ready[0]);
    close(done[1]);
    close(fd);
    child_fd = open(name, O_RDWR);
    if(child_fd < 0)
      exit(2);
    q = mmap(0, PGSIZE, PROT_READ | PROT_WRITE,
             MAP_SHARED, child_fd, 0);
    if(q == MAP_FAILED)
      exit(3);
    q[0] = 'S';
    q[17] = 'C';
    if(write(ready[1], &token, 1) != 1 ||
       read(done[0], &token, 1) != 1)
      exit(4);
    if(q[1] != 'P')
      exit(5);
    if(munmap(q, PGSIZE) < 0)
      exit(6);
    close(child_fd);
    close(ready[1]);
    close(done[0]);
    exit(0);
  }

  close(ready[1]);
  close(done[0]);
  if(read(ready[0], &token, 1) != 1)
    err("cache child ready");
  // The child used an independent open and mmap object. The parent faults
  // only now, while the child's mapping is still resident.
  if(p[0] != 'S' || p[17] != 'C')
    err("cache child to parent visibility");
  p[1] = 'P';
  if(write(done[1], &token, 1) != 1)
    err("cache parent ready");
  wait(&status);
  if(status != 0 || p[0] != 'S' || p[1] != 'P')
    err("cache parent after child release");
  close(ready[0]);
  close(done[1]);
  if(munmap(p, PGSIZE) < 0)
    err("cache parent munmap");

  // Warm allocator/page-table metadata, then ensure repeated cache eviction
  // does not consume one physical page per iteration.
  p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(p == MAP_FAILED)
    err("cache reclaim warm mmap");
  observed = p[0];
  if(munmap(p, PGSIZE) < 0 || observed != 'S')
    err("cache reclaim warm unmap");
  if(sysinfo(&before) < 0)
    err("cache reclaim sysinfo before");
  for(int i = 0; i < 8; i++){
    p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(p == MAP_FAILED)
      err("cache reclaim mmap");
    observed = p[0];
    if(observed != 'S' || munmap(p, PGSIZE) < 0)
      err("cache reclaim loop");
  }
  if(sysinfo(&after) < 0 ||
     before.freemem > after.freemem + PGSIZE)
    err("cache page leak");

  close(fd);
  fd = open(name, O_RDONLY);
  if(fd < 0 || read(fd, buf, 18) != 18 ||
     buf[0] != 'S' || buf[1] != 'P' || buf[17] != 'C')
    err("cache writeback");
  close(fd);
  unlink(name);
  printf("file_shared_cache_test OK\n");
}

void
permission_test(void)
{
  int fd;
  int pid;
  int status;

  printf("permission_test starting\n");
  testname = "permission_test";
  make_pattern_file("mmap.permission");
  fd = open("mmap.permission", O_RDONLY);
  if(fd < 0)
    err("permission open");

  pid = fork();
  if(pid < 0)
    err("permission fork");
  if(pid == 0){
    char *p = mmap(0, PGSIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    if(p == MAP_FAILED)
      exit(2);
    p[0] = 'X';
    exit(0);
  }
  wait(&status);
  close(fd);
  unlink("mmap.permission");
  if(status == 0)
    err("read-only mapping accepted write");
  printf("permission_test OK\n");
}

void
exec_test(void)
{
  char *p;
  char c;
  char *args[] = { "echo", "mmap-exec", 0 };
  int fd;
  int pid;
  int status;

  printf("exec_test starting\n");
  testname = "exec_test";
  unlink("mmap.exec");
  fd = open("mmap.exec", O_RDWR | O_CREATE);
  if(fd < 0 || write(fd, "A", 1) != 1)
    err("exec file");
  p = mmap(0, PGSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if(p == MAP_FAILED)
    err("exec mmap");

  pid = fork();
  if(pid < 0)
    err("exec fork");
  if(pid == 0){
    p[0] = 'X';
    exec("echo", args);
    exit(2);
  }
  wait(&status);
  if(status != 0)
    err("exec child");
  fd = open("mmap.exec", O_RDONLY);
  if(fd < 0 || read(fd, &c, 1) != 1 || c != 'X')
    err("exec writeback");
  close(fd);
  munmap(p, PGSIZE);
  unlink("mmap.exec");
  printf("exec_test OK\n");
}
