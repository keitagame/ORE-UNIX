/* drivers/serial.c – 16550 UART driver (COM1 = console) */
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <arch/x86/arch.h>

#define COM1_BASE   0x3F8
#define COM2_BASE   0x2F8

#define UART_DATA    0   /* r/w data */
#define UART_IER     1   /* interrupt enable */
#define UART_IIR     2   /* interrupt ID (read) */
#define UART_FCR     2   /* FIFO control (write) */
#define UART_LCR     3   /* line control */
#define UART_MCR     4   /* modem control */
#define UART_LSR     5   /* line status */
#define UART_MSR     6   /* modem status */
#define UART_SCRATCH 7

#define LSR_DR      0x01   /* data ready */
#define LSR_THRE    0x20   /* transmitter holding register empty */

/* ─── Ring buffer ─────────────────────────────────────────────────────────── */
#define SERIAL_BUF  256

static struct {
    char           buf[SERIAL_BUF];
    u32            head, tail, size;
    spinlock_t     lock;
    wait_queue_head_t wait;
} rx_buf;

/* ─── Low-level UART ──────────────────────────────────────────────────────── */
static void uart_init(u16 base, u32 baud) {
    u16 divisor = 115200 / baud;

    outb(base + UART_IER, 0x00);    /* disable interrupts */
    outb(base + UART_LCR, 0x80);    /* DLAB = 1 */
    outb(base + UART_DATA, divisor & 0xFF);
    outb(base + UART_IER,  (divisor >> 8) & 0xFF);
    outb(base + UART_LCR, 0x03);    /* 8N1, DLAB=0 */
    outb(base + UART_FCR, 0xC7);    /* enable FIFO, clear, 14-byte threshold */
    outb(base + UART_MCR, 0x0B);    /* RTS, DTR, out2 */
    outb(base + UART_IER, 0x01);    /* enable received-data interrupt */
}

static bool uart_tx_ready(u16 base) {
    return (inb(base + UART_LSR) & LSR_THRE) != 0;
}

static bool uart_rx_ready(u16 base) {
    return (inb(base + UART_LSR) & LSR_DR) != 0;
}

static void uart_putchar(u16 base, char c) {
    while (!uart_tx_ready(base)) cpu_relax();
    outb(base + UART_DATA, c);
}

/* ─── Kernel console output ───────────────────────────────────────────────── */
void serial_putchar(char c) {
    if (c == '\n') uart_putchar(COM1_BASE, '\r');
    uart_putchar(COM1_BASE, c);
}

void serial_puts(const char *s) {
    while (*s) serial_putchar(*s++);
}

/* ─── IRQ handler ─────────────────────────────────────────────────────────── */
static void serial_irq_handler(struct regs *r) {
    (void)r;
    while (uart_rx_ready(COM1_BASE)) {
        char c = inb(COM1_BASE + UART_DATA);
        irq_flags_t f;
        spin_lock_irqsave(&rx_buf.lock, &f);
        if (rx_buf.size < SERIAL_BUF) {
            rx_buf.buf[rx_buf.head] = c;
            rx_buf.head = (rx_buf.head + 1) % SERIAL_BUF;
            rx_buf.size++;
        }
        spin_unlock_irqrestore(&rx_buf.lock, &f);
    }
    wake_up(&rx_buf.wait);
}

/* ─── file ops ────────────────────────────────────────────────────────────── */
ssize_t serial_read_file(struct file *f, char *buf, size_t count, off_t *pos) {
    (void)f; (void)pos;
    size_t n = 0;
    while (n < count) {
        irq_flags_t fl;
        spin_lock_irqsave(&rx_buf.lock, &fl);
        if (rx_buf.size > 0) {
            buf[n++] = rx_buf.buf[rx_buf.tail];
            rx_buf.tail = (rx_buf.tail + 1) % SERIAL_BUF;
            rx_buf.size--;
            /* echo */
            char c = buf[n-1];
            spin_unlock_irqrestore(&rx_buf.lock, &fl);
            serial_putchar(c);
            if (c == '\n' || c == '\r') break;
        } else {
            spin_unlock_irqrestore(&rx_buf.lock, &fl);
            if (n > 0) break;  /* return what we have */
            /* block waiting for input */
            int r = sleep_on_interruptible(&rx_buf.wait);
            if (r) return n > 0 ? (ssize_t)n : r;
        }
    }
    return n;
}

ssize_t serial_write_file(struct file *f, const char *buf, size_t count, off_t *pos) {
    (void)f; (void)pos;
    for (size_t i = 0; i < count; i++) serial_putchar(buf[i]);
    return count;
}

/* TIOCGWINSZ etc */
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
struct winsize { u16 ws_row, ws_col, ws_xpixel, ws_ypixel; };

int serial_ioctl(struct file *f, u32 cmd, uptr arg) {
    (void)f;
    switch (cmd) {
    case TIOCGWINSZ: {
        struct winsize *ws = (struct winsize*)arg;
        ws->ws_row = 24; ws->ws_col = 80;
        ws->ws_xpixel = ws->ws_ypixel = 0;
        return 0;
    }
    case TIOCSWINSZ: return 0;
    /* tcgetattr / tcsetattr */
    case 0x5401: case 0x5402: return 0;
    default: return -ENOTTY;
    }
}

void serial_init(void) {
    spin_init(&rx_buf.lock);
    init_waitqueue_head(&rx_buf.wait);
    rx_buf.head = rx_buf.tail = rx_buf.size = 0;

    uart_init(COM1_BASE, 115200);
    irq_register(IRQ_COM1, serial_irq_handler);
    printk(KERN_INFO "Serial: COM1 at 115200 baud\n");
}
