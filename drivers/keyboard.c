/* drivers/keyboard.c – PS/2 keyboard driver */
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <arch/x86/arch.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_CMD    0x64

/* US QWERTY scancode set 1 */
static const char scancode_lower[128] = {
    0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']', '\n',
    0,   'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,   '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,  ' ', 0,
    0,0,0,0,0,0,0,0,0,0,   /* F1-F10 */
    0,0,                    /* NumLock, ScrollLock */
    '7','8','9','-','4','5','6','+','1','2','3','0','.', 0,0,0,
    0,0,                    /* F11, F12 */
};

static const char scancode_upper[128] = {
    0,   27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}', '\n',
    0,   'A','S','D','F','G','H','J','K','L',':','"','~',
    0,   '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,  ' ', 0,
};

static bool shift_pressed = false;
static bool ctrl_pressed  = false;
static bool alt_pressed   = false;
static bool caps_lock     = false;

#define KBD_BUF 256
static struct {
    char       buf[KBD_BUF];
    u32        head, tail, size;
    spinlock_t lock;
    wait_queue_head_t wait;
} kbd_buf;

static void kbd_irq(struct regs *r) {
    (void)r;
    u8 sc = inb(KBD_DATA);
    bool released = sc & 0x80;
    sc &= 0x7F;

    switch (sc) {
    case 0x2A: case 0x36: shift_pressed = !released; return;
    case 0x1D: ctrl_pressed  = !released; return;
    case 0x38: alt_pressed   = !released; return;
    case 0x3A: if (!released) caps_lock = !caps_lock; return;
    }
    if (released) return;

    /* CTRL+C -> SIGINT to foreground process group */
    if (ctrl_pressed && sc == 0x2E) {
        extern struct task_struct *current;
        if (current) sys_kill(current->pgid, SIGINT);
        return;
    }

    char c = 0;
    bool upper = shift_pressed ^ caps_lock;
    if (sc < 128) {
        c = upper ? scancode_upper[sc] : scancode_lower[sc];
    }
    if (!c) return;

    irq_flags_t f;
    spin_lock_irqsave(&kbd_buf.lock, &f);
    if (kbd_buf.size < KBD_BUF) {
        kbd_buf.buf[kbd_buf.head] = c;
        kbd_buf.head = (kbd_buf.head + 1) % KBD_BUF;
        kbd_buf.size++;
    }
    spin_unlock_irqrestore(&kbd_buf.lock, &f);
    wake_up(&kbd_buf.wait);
}

void keyboard_init(void) {
    spin_init(&kbd_buf.lock);
    init_waitqueue_head(&kbd_buf.wait);
    irq_register(IRQ_KEYBOARD, kbd_irq);
    printk(KERN_INFO "Keyboard: PS/2 initialized\n");
}

/* ─── VGA text mode driver ───────────────────────────────────────────────── */
#define VGA_BASE    0xC00B8000UL
#define VGA_COLS    80
#define VGA_ROWS    25
#define VGA_TAB     8

static u16 *vga_buf = (u16*)VGA_BASE;
static int  vga_row = 0, vga_col = 0;
static u8   vga_attr = 0x07;  /* white on black */

#define VGA_ENTRY(ch, attr) ((u16)(ch) | ((u16)(attr) << 8))

static void vga_scroll(void) {
    for (int r = 1; r < VGA_ROWS; r++)
        memcpy(&vga_buf[(r-1)*VGA_COLS], &vga_buf[r*VGA_COLS],
               VGA_COLS * 2);
    for (int c = 0; c < VGA_COLS; c++)
        vga_buf[(VGA_ROWS-1)*VGA_COLS + c] = VGA_ENTRY(' ', vga_attr);
    vga_row = VGA_ROWS - 1;
}

static void vga_update_cursor(void) {
    u16 pos = vga_row * VGA_COLS + vga_col;
    outb(0x3D4, 0x0F); outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E); outb(0x3D5, (pos >> 8) & 0xFF);
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_col = 0; vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\b') {
        if (vga_col > 0) vga_col--;
        vga_buf[vga_row * VGA_COLS + vga_col] = VGA_ENTRY(' ', vga_attr);
    } else if (c == '\t') {
        vga_col = (vga_col + VGA_TAB) & ~(VGA_TAB - 1);
    } else {
        vga_buf[vga_row * VGA_COLS + vga_col] = VGA_ENTRY(c, vga_attr);
        vga_col++;
    }
    if (vga_col >= VGA_COLS) { vga_col = 0; vga_row++; }
    if (vga_row >= VGA_ROWS)  vga_scroll();
    vga_update_cursor();
}

void vga_puts(const char *s) {
    while (*s) vga_putchar(*s++);
}

void vga_clear(void) {
    for (int i = 0; i < VGA_ROWS * VGA_COLS; i++)
        vga_buf[i] = VGA_ENTRY(' ', vga_attr);
    vga_row = vga_col = 0;
    vga_update_cursor();
}

void vga_set_color(u8 fg, u8 bg) {
    vga_attr = (bg << 4) | (fg & 0x0F);
}

void vga_init(void) {
    vga_clear();
    printk(KERN_INFO "VGA: 80x25 text mode\n");
}
