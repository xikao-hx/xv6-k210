//
// exec_k210.c - K210 (FAT32) version of exec
//

#include "elf.h"
#include "fat32.h"
#include "printf.h"
#include "proc.h"
#include "string.h"
#include "vm.h"

static int
loadseg(pagetable_t pagetable, uint64 va, struct dirent *ep, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;
  if((va % PGSIZE) != 0)
    panic("loadseg: va must be page aligned");

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(eread(ep, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }

  return 0;
}

int
exec(char *path, char **argv)
{
  int i, off;
  uint64 argc, sz, sp, ustack[MAXARG+1], stackbase;
  struct elfhdr elf;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct dirent *ep;

  struct proc *p = myproc();

  sz = 0;
  if((ep = ename(path)) == 0){
    return -1;
  }
  elock(ep);

  // Check ELF header
  if(eread(ep, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program segments
  for(i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)){
    if(eread(ep, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    uint64 sz2;
    if((sz2 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) < 0)
      goto bad;
    sz = sz2;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    if(loadseg(pagetable, ph.vaddr, ep, ph.off, ph.filesz) < 0)
      goto bad;
  }

  // Allocate a user page for the stack
  sz = PGROUNDUP(sz);
  uint64 sz3;
  if((sz3 = uvmalloc(pagetable, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  sz = sz3;
  uvmclear(pagetable, sz-2*PGSIZE);
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16;
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  p->trapframe->a1 = sp;

  // Save process state
  oldpagetable = p->pagetable;
  uint64 old_sz = p->sz;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;
  p->trapframe->sp = sp;

  // Update kpagetable for the new user address space.
  // Unmap old user mappings, then copy new ones from the new pagetable.
  uvmunmap(p->kpagetable, 0, PGROUNDUP(old_sz) / PGSIZE, 0);
  upg2ukpg(p->pagetable, p->kpagetable, 0, p->sz);

  proc_freepagetable(oldpagetable, old_sz);

  eunlock(ep);
  eput(ep);

  return argc;

 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ep){
    eunlock(ep);
    eput(ep);
  }
  return -1;
}
