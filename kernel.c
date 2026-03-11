/* kernel.c */
#include <stdint.h>

/* ===========================
   VGA TEXT MODE
   =========================== */
static volatile uint16_t* const VGA_BUFFER = (uint16_t*)0xB8000;
static const int VGA_COLS = 80;
static const int VGA_ROWS = 25;

static int vga_row = 0;
static int vga_col = 0;

static uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void vga_putc(char c, uint8_t color) {
    if (c == '\n') {
        vga_row++;
        vga_col = 0;
    } else {
        VGA_BUFFER[vga_row * VGA_COLS + vga_col] = vga_entry(c, color);
        vga_col++;
        if (vga_col >= VGA_COLS) {
            vga_col = 0;
            vga_row++;
        }
    }
    if (vga_row >= VGA_ROWS) {
        vga_row = 0; // 超シンプルなスクロール
    }
}

/* ===========================
   SERIAL PORT (COM1)
   =========================== */
#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void serial_init(void) {
    outb(COM1_PORT + 1, 0x00); // 割り込み無効
    outb(COM1_PORT + 3, 0x80); // DLAB=1
    outb(COM1_PORT + 0, 0x03); // 38400 baud
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03); // 8N1
    outb(COM1_PORT + 2, 0xC7); // FIFO 有効
    outb(COM1_PORT + 4, 0x0B); // DTR/RTS 有効
}

static int serial_is_transmit_empty(void) {
    return inb(COM1_PORT + 5) & 0x20;
}

static void serial_putc(char c) {
    while (!serial_is_transmit_empty());
    outb(COM1_PORT, c);
}

/* ===========================
   TTY: VGA + SERIAL 同時出力
   =========================== */
static void tty_putc(char c) {
    vga_putc(c, 0x0F);  // VGA: 黒地に白
    serial_putc(c);    // SERIAL: COM1
}

static void tty_print(const char* s) {
    while (*s) tty_putc(*s++);
}

/* ===========================
   ENTRY POINT
   =========================== */

void kmain(void) {
    uint8_t color = 0x0F; // 黒地に白

    // 画面クリア
    for (int y = 0; y < VGA_ROWS; y++) {
        for (int x = 0; x < VGA_COLS; x++) {
            VGA_BUFFER[y * VGA_COLS + x] = vga_entry(' ', color);
        }
    }

    serial_init();

    tty_print("Hello from kernel!\n");
    

    // ここから先は multiboot_header.s の hang に戻って停止
}
