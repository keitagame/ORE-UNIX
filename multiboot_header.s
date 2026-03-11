/* multiboot_header.s */
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

    .section .multiboot
    .align 4
    .long MAGIC
    .long FLAGS
    .long CHECKSUM

    .section .text
    .global _start
_start:
    /* GRUB からここに飛んでくる（32bit, protected mode） */
    call kmain

hang:
    cli
    hlt
    jmp hang
