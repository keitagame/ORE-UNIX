# Makefile – MyOS kernel build system
# Usage: make            – build kernel.elf + iso
#        make iso        – build bootable ISO
#        make qemu       – run in QEMU (serial console)
#        make qemu-gdb   – run with GDB stub
#        make clean

ARCH       := i686
TARGET     := $(ARCH)-elf
CC         := $(TARGET)-gcc
AS         := $(TARGET)-as
LD         := $(TARGET)-ld
OBJCOPY    := $(TARGET)-objcopy
NASM       := nasm

# Fall back to native gcc if cross-compiler not found
ifeq ($(shell which $(CC) 2>/dev/null),)
  CC  := gcc
  AS  := as
  LD  := ld
  OBJCOPY := objcopy
  CROSS_WARN := 1
endif

CFLAGS  := -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter \
           -ffreestanding -fno-builtin -fno-stack-protector   \
           -fno-omit-frame-pointer -fno-pie -fno-pic           \
           -m32 -march=i686                                     \
           -Iinclude -Iinclude/kernel -Iinclude/arch/x86       \
           -DKERNEL

ASFLAGS := --32

LDFLAGS := -T kernel.ld -m elf_i386 --build-id=none \
           -nostdlib -static

# ── Source files ──────────────────────────────────────────────────────────── #
BOOT_SRC  := boot/boot.S boot/gdt_idt.c
KERNEL_SRC := kernel/kmain.c kernel/process.c kernel/elf.c   \
              kernel/syscall.c kernel/kprintf.c
MM_SRC    := mm/pmm.c mm/vmm.c mm/kmalloc.c
FS_SRC    := fs/vfs.c fs/ramfs.c fs/pipe.c
DRIVER_SRC := drivers/serial.c drivers/timer.c \
              drivers/ata.c drivers/dev.c

ALL_SRC := $(BOOT_SRC) $(KERNEL_SRC) $(MM_SRC) $(FS_SRC) $(DRIVER_SRC)

# ── Object files ─────────────────────────────────────────────────────────── #
OBJ_DIR  := build/obj
OBJS     := $(patsubst %,$(OBJ_DIR)/%.o,$(ALL_SRC))

KERNEL_ELF := build/kernel.elf
ISO_DIR    := build/iso
ISO_FILE   := build/myos.iso

# ── GRUB config ──────────────────────────────────────────────────────────── #
GRUB_CFG := $(ISO_DIR)/boot/grub/grub.cfg

.PHONY: all iso qemu qemu-gdb clean dirs check-tools

all: dirs $(KERNEL_ELF)
	@echo ""
	@echo "Build complete: $(KERNEL_ELF)"
	@echo "  Size: $$(du -h $(KERNEL_ELF) | cut -f1)"

# ── Compilation rules ─────────────────────────────────────────────────────── #
$(OBJ_DIR)/%.S.o: %.S | dirs
	@mkdir -p $(@D)
	@echo "  AS   $<"
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.c.o: %.c | dirs
	@mkdir -p $(@D)
	@echo "  CC   $<"
	$(CC) $(CFLAGS) -c $< -o $@

# ── Link ──────────────────────────────────────────────────────────────────── #
$(KERNEL_ELF): $(OBJS) kernel.ld
	@echo "  LD   $@"
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "  Sections:"
	@$(TARGET)-size $@ 2>/dev/null || size $@

dirs:
	@mkdir -p $(OBJ_DIR)/boot $(OBJ_DIR)/kernel \
	          $(OBJ_DIR)/mm   $(OBJ_DIR)/fs     \
	          $(OBJ_DIR)/drivers

# ── ISO (requires grub-mkrescue + xorriso) ────────────────────────────────── #
iso: all
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/kernel.elf
	@cat > $(GRUB_CFG) << 'EOF'
	set timeout=3
	set default=0

	menuentry "MyOS" {
	multiboot2 /boot/kernel.elf
	boot
	}

	menuentry "MyOS (with initrd)" {
	multiboot2 /boot/kernel.elf
	module2    /boot/initrd.cpio initrd
	boot
	}
	EOF
	@echo "  ISO  $(ISO_FILE)"
	grub-mkrescue -o $(ISO_FILE) $(ISO_DIR) 2>/dev/null || \
	grub2-mkrescue -o $(ISO_FILE) $(ISO_DIR)
	@echo "ISO built: $(ISO_FILE)"

# ── QEMU ─────────────────────────────────────────────────────────────────── #
QEMU     := qemu-system-i386
QEMU_MEM := 256M
QEMU_FLAGS := -m $(QEMU_MEM) -no-reboot -no-shutdown  \
              -serial stdio -display none               \
              -drive id=disk0,file=build/disk.img,format=raw,if=ide \
              -drive id=disk1,file=/dev/null,format=raw,if=ide,media=cdrom

qemu: all build/disk.img
	$(QEMU) $(QEMU_FLAGS) \
	    -kernel $(KERNEL_ELF) \
	    -append "console=ttyS0 init=/sbin/init"

qemu-initrd: all build/disk.img build/initrd.cpio
	$(QEMU) $(QEMU_FLAGS) \
	    -kernel $(KERNEL_ELF) \
	    -initrd build/initrd.cpio \
	    -append "console=ttyS0 init=/sbin/init"

qemu-iso: iso build/disk.img
	$(QEMU) -m $(QEMU_MEM) -no-reboot -serial stdio \
	    -cdrom $(ISO_FILE) -boot d

qemu-gdb: all
	$(QEMU) $(QEMU_FLAGS) \
	    -kernel $(KERNEL_ELF) \
	    -append "console=ttyS0" \
	    -s -S &
	@echo "QEMU waiting for GDB. Run: gdb $(KERNEL_ELF)"
	@echo "  (gdb) target remote :1234"

# ── Create blank disk image ───────────────────────────────────────────────── #
build/disk.img:
	@mkdir -p build
	dd if=/dev/zero of=$@ bs=1M count=64 2>/dev/null
	@echo "Created blank disk image: $@"

# ── initrd: build a minimal CPIO initrd ───────────────────────────────────── #
INITRD_DIR := build/initrd_staging

initrd: build/initrd.cpio

build/initrd.cpio: $(INITRD_DIR)/sbin/init
	@echo "  CPIO build/initrd.cpio"
	@cd $(INITRD_DIR) && find . | cpio -H newc -o > ../../$@ 2>/dev/null
	@echo "  initrd size: $$(du -h build/initrd.cpio | cut -f1)"

$(INITRD_DIR)/sbin/init: scripts/build_initrd.sh
	@bash scripts/build_initrd.sh $(INITRD_DIR)

# ── Line count ────────────────────────────────────────────────────────────── #
loc:
	@find . -name '*.c' -o -name '*.h' -o -name '*.S' | \
	    grep -v build | xargs wc -l | tail -1

# ── Clean ─────────────────────────────────────────────────────────────────── #
clean:
	rm -rf build/obj build/kernel.elf build/myos.iso build/initrd.cpio
	@echo "Clean done"

distclean: clean
	rm -rf build/

# ── Check tools ───────────────────────────────────────────────────────────── #
check-tools:
	@echo "Checking build tools..."
	@which $(CC)      && echo "  CC: $$($(CC) --version | head -1)"
	@which $(LD)      && echo "  LD: $$($(LD) --version | head -1)"
	@which qemu-system-i386 && echo "  QEMU: $$(qemu-system-i386 --version | head -1)"
	@which grub-mkrescue && echo "  GRUB: $$(grub-mkrescue --version)"