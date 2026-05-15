
# To compile and run with a lab solution, set the lab name in lab.mk
# (e.g., LB=util).  Run make grade to test solution with the lab's
# grade script (e.g., grade-lab-util).

# -include conf/lab.mk

platform ?= qemu

K=kernel
U=user

# Entry point must be the first object in link order
# (OpenSBI jumps to the beginning of the kernel image)
ifeq ($(platform), k210)
ENTRY_OBJ = $K/entry_k210.o
else
ENTRY_OBJ = $K/entry_qemu.o
endif

# Common objects (all platforms)
OBJS = \
  $(ENTRY_OBJ) \
  $K/main.o \
  $K/devsw/console.o \
  $K/devsw/stats.o \
  $K/driver/uart.o \
  $K/driver/plic.o \
  $K/driver/disk.o \
  $K/fs/bio.o \
  $K/fs/fat32.o \
  $K/fs/file.o \
  $K/ipc/pipe.o \
  $K/libc/printf.o \
  $K/libc/sprintf.o \
  $K/libc/string.o \
  $K/lock/sleeplock.o \
  $K/lock/spinlock.o \
  $K/proc/exec.o \
  $K/proc/proc.o \
  $K/syscall/syscall.o \
  $K/syscall/sysfile.o \
  $K/syscall/sysproc.o \
  $K/vm/kalloc.o \
  $K/vm/vm.o \
  $K/vm/vmcopyin.o \
  $K/proc/swtch.o \
  $K/trap/trap.o \
  $K/trap/trampoline.o \
  $K/trap/kernelvec.o

# Platform-specific objects
ifeq ($(platform), k210)
OBJS += \
  $K/driver/spi.o \
  $K/driver/gpiohs.o \
  $K/driver/fpioa.o \
  $K/driver/utils.o \
  $K/driver/sdcard.o \
  $K/driver/dmac.o \
  $K/driver/sysctl.o
else
OBJS += \
  $K/driver/virtio_disk.o
endif

# riscv64-unknown-elf- or riscv64-linux-gnu-

TOOLPREFIX = riscv64-unknown-linux-gnu-
QEMU = qemu-system-riscv64

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

CFLAGS = -Wall -Werror -O -fno-omit-frame-pointer -ggdb -g

# ifdef LAB
# LABUPPER = $(shell echo $(LAB) | tr a-z A-Z)
# CFLAGS += -DSOL_$(LABUPPER) -DLAB_$(LABUPPER)
# endif

CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -I.
CFLAGS += -I$K/include
ifeq ($(platform), qemu)
CFLAGS += -D QEMU
endif
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

LDFLAGS = -z max-page-size=4096

ifeq ($(platform), k210)
LINKER_SCRIPT = linker/k210.ld
else
LINKER_SCRIPT = linker/qemu.ld
endif

$K/kernel: $(OBJS) $(LINKER_SCRIPT) $U/initcode
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $K/kernel $(OBJS)
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$U/initcode: $U/initcode.S
	$(CC) $(CFLAGS) -march=rv64g -nostdinc -I. -I$K/include -c $U/initcode.S -o $U/initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $U/initcode.out $U/initcode.o
	$(OBJCOPY) -S -O binary $U/initcode.out $U/initcode
	$(OBJDUMP) -S $U/initcode.o > $U/initcode.asm

tags: $(OBJS) _init
	etags *.S *.c

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

# ifneq ($(filter pgtbl lock,$(LAB)),)
# ULIB += $U/statistics.o
# endif

_%: %.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/usys.S : $U/usys.pl
	perl $U/usys.pl > $U/usys.S

$U/usys.o : $U/usys.S
	$(CC) $(CFLAGS) -c -o $U/usys.o $U/usys.S

$U/_forktest: $U/forktest.o $(ULIB)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/_forktest $U/forktest.o $U/ulib.o $U/usys.o
	$(OBJDUMP) -S $U/_forktest > $U/forktest.asm

# user/_find: user/find.o user/grep.o $(ULIB)
# 	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
# 	$(OBJDUMP) -S $@ > $*.asm
# 	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_ls\
	$U/_mkdir\
	$U/_rm\
	$U/_sh\
	$U/_grind\
	$U/_wc\
	$U/_zombie

# ifneq ($(filter pgtbl lock,$(LAB)),)
# UPROGS += \
# 	$U/_stats
# endif

# ifeq ($(LAB),syscall)
# UPROGS += \
# 	$U/_trace\
# 	$U/_sysinfotest
# endif

# ifeq ($(LAB),trap)
# UPROGS += \
# 	$U/_call\
# 	$U/_alarmtest
# endif

# ifeq ($(LAB),lazy)
# UPROGS += \
# 	$U/_lazytests
# endif

# ifeq ($(LAB),cow)
# UPROGS += \
# 	$U/_cowtest
# endif

# UEXTRA=
# ifeq ($(LAB),util)
# 	UEXTRA += user/xargstest.sh
# endif

dst := /mnt

# Create FAT32 filesystem image populated with all user programs.
# Uses pyfatfs (no sudo required).
fs: $(UPROGS)
	@echo "populating fs.img with user programs..."
	@dd if=/dev/zero of=fs.img bs=512k count=512 2>/dev/null
	@mkfs.vfat -F 32 fs.img 2>/dev/null
	@python3 scripts/mkfs.py "$U"
	@echo "done"

-include kernel/*.d kernel/**/*.d user/*.d

clean:
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym \
	*/*/*.o */*/*.d */*/*.asm */*/*.sym \
	$U/initcode $U/initcode.out $K/kernel $K/kernel.bin $K/kernel.elf fs.img \
	.gdbinit \
	$U/usys.S \
	$(UPROGS)

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
		then echo "-gdb tcp::$(GDBPORT)"; \
		else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 2
endif

QEMUOPTS = -machine virt -bios none -kernel $K/kernel -m 128M -smp $(CPUS) -nographic
QEMUOPTS += -drive file=fs.img,if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

qemu: $K/kernel fs
#	$(QEMU) $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/" < $^ > $@

qemu-gdb: $K/kernel .gdbinit fs.img
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB)

.PHONY: xv6_image handin tarball tarball-pref clean grade handin-check
