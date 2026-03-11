#include "idt.h"

#define IDT_SIZE 256

static struct idt_entry idt[IDT_SIZE];
static struct idt_ptr idtp;

extern void isr0();  // タイマー割り込み（IRQ0）
static void idt_clear_gate(int n) {
    idt[n].base_low  = 0;
    idt[n].sel       = 0;
    idt[n].always0   = 0;
    idt[n].flags     = 0;   // present=0 → 無効
    idt[n].base_high = 0;
}

static void idt_set_gate(int n, uint32_t handler) {
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    idt[n].base_low  = handler & 0xFFFF;
    idt[n].sel       = cs;        // ★ 0x08 固定ではなく、今の CS を使う
    idt[n].always0   = 0;
    idt[n].flags     = 0x8E;      // present, ring0, 32bit interrupt gate
    idt[n].base_high = (handler >> 16) & 0xFFFF;
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    for (int i = 0; i < IDT_SIZE; i++) {
        idt_clear_gate(i);
    }

    idt_set_gate(32, (uint32_t)isr0); // IRQ0

    __asm__ volatile ("lidt %0" : : "m"(idtp));
}

