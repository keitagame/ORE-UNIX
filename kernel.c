/* kernel.c */
#include <stdint.h>

static volatile uint16_t* const VGA_BUFFER = (uint16_t*)0xB8000;
static const int VGA_COLS = 80;
static const int VGA_ROWS = 25;

static uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

void kmain(void) {
    uint8_t color = 0x0F; // 黒地に白

    // 画面クリア
    for (int y = 0; y < VGA_ROWS; y++) {
        for (int x = 0; x < VGA_COLS; x++) {
            VGA_BUFFER[y * VGA_COLS + x] = vga_entry(' ', color);
        }
    }

    const char* msg = "Hello from minimal C kernel via GRUB!";
    int x = 0;
    while (*msg) {
        VGA_BUFFER[x++] = vga_entry(*msg++, color);
    }

    // ここから先は multiboot_header.s の hang に戻って停止
}
