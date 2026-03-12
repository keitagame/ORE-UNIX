/* boot/gdt.c – GDT, IDT, TSS initialization */
#include "../include/arch.h"
struct task_struct;
/* page fault handler (somewhere in mm/ or so) */
void page_fault_handler(u32 addr, u32 err);

/* signal / kill (somewhere in kernel/) */
int sys_kill(int pid, int sig);

/* temporary, until you have proper signal.h */
#define SIGSEGV 11
/* ─── GDT ─────────────────────────────────────────────────────────────────── */
static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdtp;
static struct tss       ktss;

void gdt_set_entry(int idx, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[idx].base_low  = (base & 0xFFFF);
    gdt[idx].base_mid  = (base >> 16) & 0xFF;
    gdt[idx].base_high = (base >> 24) & 0xFF;
    gdt[idx].limit_low = (limit & 0xFFFF);
    gdt[idx].gran      = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[idx].access    = access;
}

static void gdt_load(void) {
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (u32)gdt;
    __asm__ volatile(
        "lgdt (%0)\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "ljmp $0x08, $1f\n\t"
        "1:\n\t"
        :: "r"(&gdtp) : "memory", "eax");
}

void gdt_init(void) {
    gdt_set_entry(GDT_NULL,  0, 0,          0,    0);
    /* kernel code: ring 0, execute/read */
    gdt_set_entry(GDT_KCODE, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    /* kernel data: ring 0, read/write */
    gdt_set_entry(GDT_KDATA, 0, 0xFFFFFFFF, 0x92, 0xCF);
    /* user code: ring 3, execute/read */
    gdt_set_entry(GDT_UCODE, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    /* user data: ring 3, read/write */
    gdt_set_entry(GDT_UDATA, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* TSS */
    u32 tss_base  = (u32)&ktss;
    u32 tss_limit = sizeof(ktss) - 1;
    memset(&ktss, 0, sizeof(ktss));
    ktss.ss0       = SEG_KDATA;
    ktss.iomap_base = sizeof(ktss);
    gdt_set_entry(GDT_TSS, tss_base, tss_limit, 0x89, 0x00);

    gdt_load();

    /* load TSS */
    __asm__ volatile("ltr %%ax" :: "a"((u16)SEG_TSS_SEL));
}

void tss_set_kernel_stack(u32 esp0) {
    ktss.esp0 = esp0;
}

/* ─── IDT ─────────────────────────────────────────────────────────────────── */
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

/* ISR prototypes (generated in boot.S) */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

extern void syscall_entry(void);

static void idt_set_gate(int num, u32 base, u16 sel, u8 flags) {
    idt[num].offset_low  = base & 0xFFFF;
    idt[num].selector    = sel;
    idt[num].zero        = 0;
    idt[num].type_attr   = flags;
    idt[num].offset_high = (base >> 16) & 0xFFFF;
}

/* ── PIC (8259) remapping ───────────────────────────────────────────────── */
#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20
#define ICW1_INIT  0x11
#define ICW4_8086  0x01

static void pic_remap(u8 offset1, u8 offset2) {
    u8 mask1 = inb(PIC1_DATA);
    u8 mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD,  ICW1_INIT); io_wait();
    outb(PIC2_CMD,  ICW1_INIT); io_wait();
    outb(PIC1_DATA, offset1);   io_wait();
    outb(PIC2_DATA, offset2);   io_wait();
    outb(PIC1_DATA, 4);         io_wait(); /* cascade IRQ2 */
    outb(PIC2_DATA, 2);         io_wait();
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(int irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask_irq(int irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 bit   = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) | (1 << bit));
}

void pic_unmask_irq(int irq) {
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    u8 bit   = (irq < 8) ? irq : (irq - 8);
    outb(port, inb(port) & ~(1 << bit));
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));

    /* exceptions */
    idt_set_gate(0,  (u32)isr0,  SEG_KCODE, 0x8E);
    idt_set_gate(1,  (u32)isr1,  SEG_KCODE, 0x8E);
    idt_set_gate(2,  (u32)isr2,  SEG_KCODE, 0x8E);
    idt_set_gate(3,  (u32)isr3,  SEG_KCODE, 0x8E);
    idt_set_gate(4,  (u32)isr4,  SEG_KCODE, 0x8E);
    idt_set_gate(5,  (u32)isr5,  SEG_KCODE, 0x8E);
    idt_set_gate(6,  (u32)isr6,  SEG_KCODE, 0x8E);
    idt_set_gate(7,  (u32)isr7,  SEG_KCODE, 0x8E);
    idt_set_gate(8,  (u32)isr8,  SEG_KCODE, 0x8E);
    idt_set_gate(9,  (u32)isr9,  SEG_KCODE, 0x8E);
    idt_set_gate(10, (u32)isr10, SEG_KCODE, 0x8E);
    idt_set_gate(11, (u32)isr11, SEG_KCODE, 0x8E);
    idt_set_gate(12, (u32)isr12, SEG_KCODE, 0x8E);
    idt_set_gate(13, (u32)isr13, SEG_KCODE, 0x8E);
    idt_set_gate(14, (u32)isr14, SEG_KCODE, 0x8E);
    idt_set_gate(15, (u32)isr15, SEG_KCODE, 0x8E);
    idt_set_gate(16, (u32)isr16, SEG_KCODE, 0x8E);
    idt_set_gate(17, (u32)isr17, SEG_KCODE, 0x8E);
    idt_set_gate(18, (u32)isr18, SEG_KCODE, 0x8E);
    idt_set_gate(19, (u32)isr19, SEG_KCODE, 0x8E);
    idt_set_gate(20, (u32)isr20, SEG_KCODE, 0x8E);
    idt_set_gate(21, (u32)isr21, SEG_KCODE, 0x8E);
    idt_set_gate(22, (u32)isr22, SEG_KCODE, 0x8E);
    idt_set_gate(23, (u32)isr23, SEG_KCODE, 0x8E);
    idt_set_gate(24, (u32)isr24, SEG_KCODE, 0x8E);
    idt_set_gate(25, (u32)isr25, SEG_KCODE, 0x8E);
    idt_set_gate(26, (u32)isr26, SEG_KCODE, 0x8E);
    idt_set_gate(27, (u32)isr27, SEG_KCODE, 0x8E);
    idt_set_gate(28, (u32)isr28, SEG_KCODE, 0x8E);
    idt_set_gate(29, (u32)isr29, SEG_KCODE, 0x8E);
    idt_set_gate(30, (u32)isr30, SEG_KCODE, 0x8E);
    idt_set_gate(31, (u32)isr31, SEG_KCODE, 0x8E);

    /* remap PIC to IRQ_OFFSET (32) */
    pic_remap(IRQ_OFFSET, IRQ_OFFSET + 8);

    /* IRQ handlers at 32..47 */
    idt_set_gate(32, (u32)irq0,  SEG_KCODE, 0x8E);
    idt_set_gate(33, (u32)irq1,  SEG_KCODE, 0x8E);
    idt_set_gate(34, (u32)irq2,  SEG_KCODE, 0x8E);
    idt_set_gate(35, (u32)irq3,  SEG_KCODE, 0x8E);
    idt_set_gate(36, (u32)irq4,  SEG_KCODE, 0x8E);
    idt_set_gate(37, (u32)irq5,  SEG_KCODE, 0x8E);
    idt_set_gate(38, (u32)irq6,  SEG_KCODE, 0x8E);
    idt_set_gate(39, (u32)irq7,  SEG_KCODE, 0x8E);
    idt_set_gate(40, (u32)irq8,  SEG_KCODE, 0x8E);
    idt_set_gate(41, (u32)irq9,  SEG_KCODE, 0x8E);
    idt_set_gate(42, (u32)irq10, SEG_KCODE, 0x8E);
    idt_set_gate(43, (u32)irq11, SEG_KCODE, 0x8E);
    idt_set_gate(44, (u32)irq12, SEG_KCODE, 0x8E);
    idt_set_gate(45, (u32)irq13, SEG_KCODE, 0x8E);
    idt_set_gate(46, (u32)irq14, SEG_KCODE, 0x8E);
    idt_set_gate(47, (u32)irq15, SEG_KCODE, 0x8E);

    /* syscall: int 0x80, DPL=3 so user can call */
    idt_set_gate(0x80, (u32)syscall_entry, SEG_KCODE, 0xEE);

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (u32)idt;
    __asm__ volatile("lidt (%0)" :: "r"(&idtp) : "memory");
}

/* ─── IRQ dispatch table ─────────────────────────────────────────────────── */
static irq_handler_t irq_handlers[16];

void irq_register(int irq, irq_handler_t h) {
    if (irq < 0 || irq >= 16) return;
    irq_handlers[irq] = h;
    pic_unmask_irq(irq);
}

void irq_unregister(int irq) {
    if (irq < 0 || irq >= 16) return;
    irq_handlers[irq] = NULL;
    pic_mask_irq(irq);
}

void irq_handler_dispatch(struct regs *r) {
    int irq = r->int_no - IRQ_OFFSET;
    if (irq >= 0 && irq < 16 && irq_handlers[irq])
        irq_handlers[irq](r);
    pic_send_eoi(irq);
}

/* ─── Exception handler ─────────────────────────────────────────────────── */
static const char *exception_names[] = {
    "Divide Error",          "Debug",                  "NMI",
    "Breakpoint",            "Overflow",               "Bound Range Exceeded",
    "Invalid Opcode",        "Device Not Available",   "Double Fault",
    "Coprocessor Overrun",   "Invalid TSS",            "Segment Not Present",
    "Stack Fault",           "General Protection",     "Page Fault",
    "Reserved",              "x87 FPU Error",          "Alignment Check",
    "Machine Check",         "SIMD FP Exception",
};

void exception_handler(struct regs *r) {
    if (r->int_no == 14) {
        /* page fault – delegate */
        page_fault_handler(read_cr2(), r->err_code);
        return;
    }

    if (r->cs & 3) {
        /* user-mode fault: send SIGSEGV */
        extern struct task_struct *current;
        if (current && current->pid != 0) {
            printk(KERN_WARN "Process %d (%s) faulted: %s (err=%u eip=%08x)\n",
                   current->pid, current->name,
                   r->int_no < 20 ? exception_names[r->int_no] : "?",
                   r->err_code, r->eip);
            sys_kill(current->pid, SIGSEGV);
            return;
        }
    }

    panic("Exception %u (%s) at EIP=%08x ESP=%08x ERR=%08x\n"
          "EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n"
          "ESI=%08x EDI=%08x EBP=%08x\n",
          r->int_no,
          r->int_no < 20 ? exception_names[r->int_no] : "unknown",
          r->eip, r->esp_dummy, r->err_code,
          r->eax, r->ebx, r->ecx, r->edx,
          r->esi, r->edi, r->ebp);
}
