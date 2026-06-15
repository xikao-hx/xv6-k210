
# To compile and run with a lab solution, set the lab name in lab.mk
# (e.g., LB=util).  Run make grade to test solution with the lab's
# grade script (e.g., grade-lab-util).

# -include conf/lab.mk

platform ?= qemu

K=kernel
U=user
T=target
FS_SIZE_MB ?= 64

BUILD = build
KBUILD = $(BUILD)/kernel
UBUILD = $(BUILD)/user

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
  $K/driver/uarths.o \
  $K/driver/plic.o \
  $K/fs/disk.o \
  $K/fs/bio.o \
  $K/fs/fat32.o \
  $K/fs/file.o \
  $K/ipc/pipe.o \
  $K/libc/printf.o \
  $K/libc/sprintf.o \
  $K/libc/string.o \
  $K/libc/logo.o \
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
  $K/driver/i2c.o \
  $K/devsw/spidev.o \
  $K/devsw/i2cdev.o \
  $K/devsw/sdcarddev.o \
  $K/devsw/uartdev.o \
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

# Map kernel objects to build directory
OBJS := $(patsubst $K/%.o,$(KBUILD)/%.o,$(OBJS))

# riscv64-unknown-elf- or riscv64-linux-gnu-
TOOLPREFIX = riscv64-unknown-elf-
# TOOLPREFIX = riscv64-unknown-linux-gnu-
QEMU = qemu-system-riscv64

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

CFLAGS = -Wall  -O -fno-omit-frame-pointer -ggdb -g
CFLAGS += -MD
# CFLAGS += -D TEST
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -I.
# CFLAGS += -D DEBUG
CFLAGS += -I$K/include
CFLAGS += -I$U/include
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

ifeq ($(platform), k210)
RUSTSBI = ./bootloader/sbi-k210
else
RUSTSBI = ./bootloader/sbi-qemu
endif

# -------- kernel compile --------
$(KBUILD)/%.o: $K/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(KBUILD)/%.o: $K/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$T/kernel: $(OBJS) $(LINKER_SCRIPT) $(UBUILD)/initcode
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $T/kernel $(OBJS)
	$(OBJDUMP) -S $T/kernel > $T/kernel.asm
	$(OBJDUMP) -t $T/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $T/kernel.sym

$(UBUILD)/initcode: $U/initcode.S
	@mkdir -p $(UBUILD)
	$(CC) $(CFLAGS) -march=rv64g -nostdinc -I. -I$K/include -c $U/initcode.S -o $(UBUILD)/initcode.o
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $(UBUILD)/initcode.out $(UBUILD)/initcode.o
	$(OBJCOPY) -S -O binary $(UBUILD)/initcode.out $(UBUILD)/initcode
	$(OBJDUMP) -S $(UBUILD)/initcode.o > $(UBUILD)/initcode.asm

tags: $(OBJS) $(UBUILD)/_init
	etags *.S *.c

# -------- user compiler --------
# Compile user sources into subdirectories matching kernel/ layout
$(UBUILD)/sh/%.o: $U/sh/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(UBUILD)/app/%.o: $U/app/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(UBUILD)/test/%.o: $U/test/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(UBUILD)/libc/%.o: $U/libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(UBUILD)/%.o: $U/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# user lib
ULIB = $(UBUILD)/libc/ulib.o $(UBUILD)/usys.o $(UBUILD)/libc/printf.o $(UBUILD)/libc/umalloc.o $(UBUILD)/libc/oled.o

define LINK_USER
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $(dir $@)$*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(dir $@)$*.sym
endef

# user programe link rules — _* outputs to same subdir as its main .o
$(UBUILD)/sh/_%: $(UBUILD)/sh/%.o $(ULIB)
	$(LINK_USER)

$(UBUILD)/app/_%: $(UBUILD)/app/%.o $(ULIB)
	$(LINK_USER)

$(UBUILD)/test/_%: $(UBUILD)/test/%.o $(ULIB)
	$(LINK_USER)

$(UBUILD)/_%: $(UBUILD)/%.o $(ULIB)
	$(LINK_USER)

# system call
$(UBUILD)/usys.S : $U/usys.pl
	@mkdir -p $(UBUILD)
	perl $U/usys.pl > $@

$(UBUILD)/usys.o : $(UBUILD)/usys.S
	$(CC) $(CFLAGS) -c -o $@ $<

# Prevent deletion of intermediate files
.PRECIOUS: $(UBUILD)/sh/%.o $(UBUILD)/app/%.o $(UBUILD)/test/%.o $(UBUILD)/libc/%.o $(UBUILD)/%.o $(KBUILD)/%.o

# user programe list
UPROGS=\
	$(UBUILD)/sh/_cat\
	$(UBUILD)/sh/_echo\
	$(UBUILD)/sh/_grep\
	$(UBUILD)/sh/_kill\
	$(UBUILD)/sh/_ln\
	$(UBUILD)/sh/_ls\
	$(UBUILD)/sh/_mkdir\
	$(UBUILD)/sh/_rm\
	$(UBUILD)/sh/_sh\
	$(UBUILD)/sh/_find\
	$(UBUILD)/app/_burn\
	$(UBUILD)/app/_mpu6050\
	$(UBUILD)/app/_w25q64\
	$(UBUILD)/test/_uarttest\
	$(UBUILD)/test/_sdtest\
	$(UBUILD)/test/_spitest\
	$(UBUILD)/test/_i2ctest\
	$(UBUILD)/test/_dmactest\
	$(UBUILD)/test/_devtest\
	$(UBUILD)/_init

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

# -------- build --------
# Compile kernel
build: $T/kernel $(UPROGS)

# Compile RustSBI
RUSTSBI:
ifeq ($(platform), k210)
	@cd ./bootloader/rustsbi-k210 && cargo build && cp ./target/riscv64gc-unknown-none-elf/debug/rustsbi-k210 ../sbi-k210
	@cp ../sbi-k210 $T
	@$(OBJDUMP) -S ./bootloader/sbi-k210 > $T/rustsbi-k210.asm
else
	@cd ./bootloader/rustsbi-qemu && cargo build && cp ./target/riscv64gc-unknown-none-elf/debug/rustsbi-qemu ../sbi-qemu
	@cp ../sbi-qemu $T
	@$(OBJDUMP) -S ./bootloader/sbi-qemu > $T/rustsbi-qemu.asm
endif

rustsbi-clean:
	@cd ./bootloader/rustsbi-k210 && cargo clean
	@cd ./bootloader/rustsbi-qemu && cargo clean

clean: # rustsbi-clean
	rm -rf $(BUILD)
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	fs.img .gdbinit
	rm -f $T/kernel $T/kernel.asm $T/kernel.sym $T/kernel.bin $T/k210.bin $T/fs.img
# rm -f bootloader/sbi-k210 bootloader/sbi-qemu

# -------- run --------
# qemu
ifndef CPUS
CPUS := 2
endif

QEMUOPTS = -machine virt -kernel $T/kernel -m 8M -nographic
QEMUOPTS += -smp $(CPUS)
QEMUOPTS += -bios $(RUSTSBI)
QEMUOPTS += -drive file=$T/fs.img,if=none,format=raw,id=x0 
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0

# k210
image = $T/kernel.bin
k210 = $T/k210.bin
k210-serialport := /dev/ttyUSB0

boot:
	@sudo chmod 777 $(k210-serialport)
	@python3 -m serial.tools.miniterm --raw --dtr 0 --rts 0 $(k210-serialport) 115200
	
run: build fs
ifeq ($(platform), k210)
	@$(OBJCOPY) $T/kernel --strip-all -O binary $(image)
	@$(OBJCOPY) $(RUSTSBI) --strip-all -O binary $(k210)
	@dd if=$(image) of=$(k210) bs=128k seek=1
# @$(OBJDUMP) -D -b binary -m riscv $(k210) > $T/k210.asm
	@sudo chmod 777 $(k210-serialport)
	@python3 ./tools/kflash.py -p $(k210-serialport) -b 115200 -t $(k210)
else
	@$(QEMU) $(QEMUOPTS)
endif

# -------- file system --------
# Create FAT32 filesystem image populated with all user programs.
# Uses pyfatfs (no sudo required).
fs: $(UPROGS)
	@echo "populating fs.img with user programs..."
	@dd if=/dev/zero of=$T/fs.img bs=1M count=$(FS_SIZE_MB) 2>/dev/null
	@mkfs.vfat -F 32 $T/fs.img 2>/dev/null
	@python3 tools/mkfs.py "$(UBUILD)"
	@echo "done"

.PHONY: xv6_image handin tarball tarball-pref clean grade handin-check

dev-sd := /dev/sdb
sdcard: fs
	@test -b "$(dev-sd)" || (echo "$(dev-sd) is not a block device; refusing to write fs.img"; exit 1)
	@sudo dd if=target/fs.img of=$(dev-sd) bs=1M status=progress
	@sudo eject $(dev-sd)

# BUG: The baud rate of K210 must be increased.
download: fs
	@python3 tools/burn.py --baud 460800 --board-baud 500000 $(k210-serialport) target/fs.img
