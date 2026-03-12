/* boot/boot.S  –  Multiboot2 compliant entry point, sets up stack + protected mode,
   then calls kmain(multiboot_info).  Runs entirely in physical addresses < 3 GB
   (the kernel is linked at KERNEL_VIRT_BASE = 0xC0000000 but loaded at 1 MB).   */

.set KERNEL_VIRT_BASE,  0xC0000000
.set KERNEL_PHYS_BASE,  0x00100000
.set BOOT_STACK_SIZE,   0x4000          /* 16 KiB temporary stack */

/* ── Multiboot2 header ─────────────────────────────────────────────────── */
.set MB2_MAGIC,     0xE85250D6
.set MB2_ARCH,      0               /* i386 protected mode */

.section .multiboot, "a"
.align 8
mb2_header:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long (mb2_header_end - mb2_header)
    .long -(MB2_MAGIC + MB2_ARCH + (mb2_header_end - mb2_header))

    /* tag: framebuffer request (text mode) */
    .align 8
    .word 5             /* type = framebuffer */
    .word 0             /* flags */
    .long 20            /* size */
    .long 0             /* width  (0 = don't care) */
    .long 0             /* height */
    .long 0             /* depth  */

    /* tag: end */
    .align 8
    .word 0
    .word 0
    .long 8
mb2_header_end:

/* ── Temporary (identity-mapped) page tables ──────────────────────────── */
.section .bss, "aw", @nobits
.align 4096
boot_pd:     .space 4096       /* page directory */
boot_pt0:    .space 4096       /* identity maps 0..4 MB (low) */
boot_pt768:  .space 4096       /* maps 0xC0000000..0xC0400000 to phys 0..4 MB */
boot_stack:  .space BOOT_STACK_SIZE
boot_stack_top:

/* ── _start: entered in 32-bit protected mode by GRUB ────────────────────*/
.section .text.boot
.global _start
_start:
    cli
    /* eax = 0x36D76289 (MB2 magic), ebx = phys addr of multiboot info */

    /* set up page tables at physical addresses */
    /* --- PT for identity 0..4 MB --- */
    lea (boot_pt0 - KERNEL_VIRT_BASE), %edi
    xor %esi, %esi
    mov $1024, %ecx
1:  mov %esi, %eax
    or  $0x003, %eax        /* present + writable */
    mov %eax, (%edi)
    add $4, %edi
    add $4096, %esi
    loop 1b

    /* --- PT for 0xC0000000..0xC0400000 -> 0..4 MB --- */
    lea (boot_pt768 - KERNEL_VIRT_BASE), %edi
    xor %esi, %esi
    mov $1024, %ecx
2:  mov %esi, %eax
    or  $0x003, %eax
    mov %eax, (%edi)
    add $4, %edi
    add $4096, %esi
    loop 2b

    /* --- fill PD --- */
    lea (boot_pd - KERNEL_VIRT_BASE), %edi
    /* entry 0: identity */
    lea (boot_pt0 - KERNEL_VIRT_BASE), %eax
    or  $0x003, %eax
    mov %eax, (%edi)
    /* entry 768 (= 0xC0000000 >> 22): higher-half */
    lea (boot_pt768 - KERNEL_VIRT_BASE), %eax
    or  $0x003, %eax
    mov %eax, (768*4)(%edi)

    /* load CR3 */
    lea (boot_pd - KERNEL_VIRT_BASE), %eax
    mov %eax, %cr3

    /* enable paging */
    mov %cr0, %eax
    or  $0x80000000, %eax
    mov %eax, %cr0

    /* long jump into higher half */
    lea higher_half, %eax
    jmp *%eax

.section .text
higher_half:
    /* now executing at virtual 0xC0xxxxxx */
    /* remove identity mapping */
    mov $boot_pd, %edi
    movl $0, (%edi)
    mov %cr3, %eax; mov %eax, %cr3   /* flush TLB */

    /* set up proper stack */
    mov $boot_stack_top, %esp

    /* save multiboot info pointer (physical) */
    push %ebx           /* multiboot info phys ptr */
    push %eax           /* should still be 0x36D76289? no – just placeholder */

    /* clear BSS */
    lea _bss_start, %edi
    lea _bss_end,   %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb

    /* call kmain(u32 mb_magic, u32 mb_info_phys) */
    pop %eax            /* mb magic (corrupted by paging, fix: re-push correctly) */
    pop %ebx            /* mb info  */
    push %ebx
    push $0x36D76289    /* MB2 magic constant */
    call kmain

    /* should never return */
    cli
hang:
    hlt
    jmp hang

/* ── Context switch ───────────────────────────────────────────────────────
   void __switch_to(struct context *old, struct context *new)
   Saves callee-saved regs into *old, restores from *new.              */
.global __switch_to
__switch_to:
    mov 4(%esp), %eax       /* old */
    mov 8(%esp), %edx       /* new */

    /* save old */
    mov %edi,  0(%eax)
    mov %esi,  4(%eax)
    mov %ebx,  8(%eax)
    mov %ebp, 12(%eax)
    mov %esp, 16(%eax)
    /* save return address (eip) */
    mov (%esp), %ecx
    mov %ecx,  20(%eax)
    pushfl
    pop %ecx
    mov %ecx, 24(%eax)

    /* restore new */
    mov  0(%edx), %edi
    mov  4(%edx), %esi
    mov  8(%edx), %ebx
    mov 12(%edx), %ebp
    mov 16(%edx), %esp
    mov 20(%edx), %ecx     /* new eip */
    mov 24(%edx), %eax
    push %eax
    popfl
    jmp *%ecx

/* ── ISR / IRQ stubs (macro-generated) ──────────────────────────────── */
.extern exception_handler
.extern irq_handler_dispatch
.extern syscall_handler

.macro ISR_NOERR num
.global isr\num
isr\num:
    cli
    push $0          /* dummy error code */
    push $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    cli
    push $\num
    jmp isr_common
.endm

.macro IRQ_STUB num irqnum
.global irq\num
irq\num:
    cli
    push $0
    push $(\irqnum + 32)
    jmp irq_common
.endm

ISR_NOERR 0;  ISR_NOERR 1;  ISR_NOERR 2;  ISR_NOERR 3
ISR_NOERR 4;  ISR_NOERR 5;  ISR_NOERR 6;  ISR_NOERR 7
ISR_ERR   8;  ISR_NOERR 9;  ISR_ERR   10; ISR_ERR   11
ISR_ERR   12; ISR_ERR   13; ISR_ERR   14; ISR_NOERR 15
ISR_NOERR 16; ISR_ERR   17; ISR_NOERR 18; ISR_NOERR 19
ISR_NOERR 20; ISR_NOERR 21; ISR_NOERR 22; ISR_NOERR 23
ISR_NOERR 24; ISR_NOERR 25; ISR_NOERR 26; ISR_NOERR 27
ISR_NOERR 28; ISR_NOERR 29; ISR_ERR   30; ISR_NOERR 31

IRQ_STUB  0, 0;  IRQ_STUB  1, 1;  IRQ_STUB  2, 2;  IRQ_STUB  3, 3
IRQ_STUB  4, 4;  IRQ_STUB  5, 5;  IRQ_STUB  6, 6;  IRQ_STUB  7, 7
IRQ_STUB  8, 8;  IRQ_STUB  9, 9;  IRQ_STUB 10,10;  IRQ_STUB 11,11
IRQ_STUB 12,12;  IRQ_STUB 13,13;  IRQ_STUB 14,14;  IRQ_STUB 15,15

/* syscall gate: int 0x80 */
.global syscall_entry
syscall_entry:
    push $0
    push $0x80
    jmp syscall_common

isr_common:
    pusha
    push %gs; push %fs; push %es; push %ds
    mov $0x10, %ax; mov %ax, %ds; mov %ax, %es
    mov %ax, %fs;  mov %ax, %gs
    push %esp
    call exception_handler
    add $4, %esp
    jmp restore_regs

irq_common:
    pusha
    push %gs; push %fs; push %es; push %ds
    mov $0x10, %ax; mov %ax, %ds; mov %ax, %es
    mov %ax, %fs;  mov %ax, %gs
    push %esp
    call irq_handler_dispatch
    add $4, %esp
    jmp restore_regs

syscall_common:
    pusha
    push %gs; push %fs; push %es; push %ds
    mov $0x10, %ax; mov %ax, %ds; mov %ax, %es
    mov %ax, %fs;  mov %ax, %gs
    push %esp
    call syscall_handler
    add $4, %esp
    /* fall through */

restore_regs:
    pop %ds; pop %es; pop %fs; pop %gs
    popa
    add $8, %esp   /* pop int_no + err_code */
    iret
