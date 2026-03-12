#ifndef _ARCH_X86_H
#define _ARCH_X86_H

#include "../kernel/kernel.h"

/* ─── CR registers ────────────────────────────────────────────────────────── */
static inline u32 read_cr0(void) { u32 v; __asm__("mov %%cr0,%0":"=r"(v)); return v; }
static inline u32 read_cr2(void) { u32 v; __asm__("mov %%cr2,%0":"=r"(v)); return v; }
static inline u32 read_cr3(void) { u32 v; __asm__("mov %%cr3,%0":"=r"(v)); return v; }
static inline void write_cr0(u32 v) { __asm__("mov %0,%%cr0"::"r"(v):"memory"); }
static inline void write_cr3(u32 v) { __asm__("mov %0,%%cr3"::"r"(v):"memory"); }
static inline void write_cr4(u32 v) { __asm__("mov %0,%%cr4"::"r"(v):"memory"); }
static inline u32 read_cr4(void)    { u32 v; __asm__("mov %%cr4,%0":"=r"(v)); return v; }
static inline void tlb_flush_all(void) { write_cr3(read_cr3()); }
static inline void tlb_flush_page(uptr va) {
    __asm__ volatile("invlpg (%0)"::"r"(va):"memory");
}

/* ─── GDT ─────────────────────────────────────────────────────────────────── */
#define GDT_NULL      0
#define GDT_KCODE     1
#define GDT_KDATA     2
#define GDT_UCODE     3
#define GDT_UDATA     4
#define GDT_TSS       5
#define GDT_ENTRIES   6

#define SEG_KCODE     (GDT_KCODE << 3)
#define SEG_KDATA     (GDT_KDATA << 3)
#define SEG_UCODE     ((GDT_UCODE << 3) | 3)
#define SEG_UDATA     ((GDT_UDATA << 3) | 3)
#define SEG_TSS_SEL   (GDT_TSS << 3)

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  gran;
    u8  base_high;
} PACKED;

struct gdt_ptr {
    u16 limit;
    u32 base;
} PACKED;

/* ─── IDT ─────────────────────────────────────────────────────────────────── */
#define IDT_ENTRIES   256

struct idt_entry {
    u16 offset_low;
    u16 selector;
    u8  zero;
    u8  type_attr;
    u16 offset_high;
} PACKED;

struct idt_ptr {
    u16 limit;
    u32 base;
} PACKED;

/* ─── TSS ─────────────────────────────────────────────────────────────────── */
struct tss {
    u32 prev_tss;
    u32 esp0;
    u32 ss0;
    u32 esp1, ss1, esp2, ss2;
    u32 cr3;
    u32 eip, eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs;
    u32 ldt;
    u16 trap, iomap_base;
} PACKED;

/* ─── CPU register frame (pushed by ISR stubs) ────────────────────────────── */
struct regs {
    /* pushed by pusha */
    u32 edi, esi, ebp, esp_dummy;
    u32 ebx, edx, ecx, eax;
    /* pushed explicitly */
    u32 ds, es, fs, gs;
    /* pushed by isr stub */
    u32 int_no, err_code;
    /* pushed by CPU */
    u32 eip, cs, eflags;
    u32 user_esp, user_ss;   /* only on privilege change */
} PACKED;

/* ─── Paging ──────────────────────────────────────────────────────────────── */
#define PDE_PRESENT    (1 << 0)
#define PDE_WRITE      (1 << 1)
#define PDE_USER       (1 << 2)
#define PDE_PWT        (1 << 3)
#define PDE_PCD        (1 << 4)
#define PDE_ACCESSED   (1 << 5)
#define PDE_LARGE      (1 << 7)

#define PTE_PRESENT    (1 << 0)
#define PTE_WRITE      (1 << 1)
#define PTE_USER       (1 << 2)
#define PTE_PWT        (1 << 3)
#define PTE_PCD        (1 << 4)
#define PTE_ACCESSED   (1 << 5)
#define PTE_DIRTY      (1 << 6)
#define PTE_GLOBAL     (1 << 8)

#define PD_INDEX(va)   (((uptr)(va)) >> 22)
#define PT_INDEX(va)   ((((uptr)(va)) >> 12) & 0x3FF)
#define PTE_ADDR(pte)  ((pte) & PAGE_MASK)

typedef u32 pde_t;
typedef u32 pte_t;

/* ─── CPUID ───────────────────────────────────────────────────────────────── */
static inline void cpuid(u32 leaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax),"=b"(*ebx),"=c"(*ecx),"=d"(*edx)
        : "a"(leaf) : "memory");
}

/* ─── MSR ─────────────────────────────────────────────────────────────────── */
static inline u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo),"=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}
static inline void wrmsr(u32 msr, u64 val) {
    __asm__ volatile("wrmsr" :: "c"(msr),"a"((u32)val),"d"((u32)(val>>32)));
}

/* ─── Halt / NOP ──────────────────────────────────────────────────────────── */
static inline void hlt(void)   { __asm__ volatile("hlt"); }
static inline void nop(void)   { __asm__ volatile("nop"); }

/* ─── Function declarations ──────────────────────────────────────────────── */
void gdt_init(void);
void idt_init(void);
void tss_set_kernel_stack(u32 esp0);
void gdt_set_entry(int idx, u32 base, u32 limit, u8 access, u8 gran);

/* IRQ numbers */
#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1
#define IRQ_CASCADE  2
#define IRQ_COM2     3
#define IRQ_COM1     4
#define IRQ_LPT2     5
#define IRQ_FLOPPY   6
#define IRQ_LPT1     7
#define IRQ_RTC      8
#define IRQ_MOUSE    12
#define IRQ_FPU      13
#define IRQ_ATA1     14
#define IRQ_ATA2     15

#define IRQ_OFFSET    32   /* remapped PIC base */

typedef void (*irq_handler_t)(struct regs *r);
void irq_register(int irq, irq_handler_t h);
void irq_unregister(int irq);
void irq_handler_dispatch(struct regs *r);

/* exception handler */
void exception_handler(struct regs *r);
void syscall_handler(struct regs *r);

#endif /* _ARCH_X86_H */
