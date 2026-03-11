TARGET = kernel.elf
ISO    = minimal.iso

CC     = gcc
AS     = as
LD     = ld

CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -fno-stack-protector -fno-pic -fno-builtin
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

all: $(ISO)

$(TARGET): multiboot_header.o kernel.o memory.o
	$(LD) $(LDFLAGS) -o $@ $^

multiboot_header.o: multiboot_header.s
	$(AS) --32 -o $@ $<

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c -o $@ $<
memory.o: memory.c memory.h
	$(CC) $(CFLAGS) -c -o $@ memory.c
$(ISO): $(TARGET) grub.cfg
	mkdir -p iso/boot/grub
	cp $(TARGET) iso/boot/kernel.elf
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) iso

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -nographic

clean:
	rm -rf *.o $(TARGET) $(ISO) iso
