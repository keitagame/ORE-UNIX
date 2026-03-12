/* drivers/timer.c – 8253/8254 PIT timer */
#include <kernel/kernel.h>
#include <kernel/process.h>
#include <arch/x86/arch.h>

#define PIT_CH0   0x40
#define PIT_CH1   0x41
#define PIT_CH2   0x42
#define PIT_CMD   0x43
#define PIT_HZ    1193182

volatile u64 jiffies = 0;      /* ticks since boot (100 Hz) */
volatile u64 uptime_s = 0;

static void timer_irq(struct regs *r) {
    (void)r;
    jiffies++;
    if (jiffies % 100 == 0) uptime_s++;
    sched_tick();
}

void timer_init(u32 hz) {
    u32 divisor = PIT_HZ / hz;
    outb(PIT_CMD, 0x36);   /* channel 0, lobyte/hibyte, mode 3 */
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, (divisor >> 8) & 0xFF);
    irq_register(IRQ_TIMER, timer_irq);
    printk(KERN_INFO "Timer: PIT at %u Hz\n", hz);
}

u64 timer_get_ms(void) { return jiffies * 10; }  /* 100 Hz -> 10ms/tick */

/* busy-wait delay (for early init) */
void udelay(u32 us) {
    u64 end = jiffies + (us / 10000) + 1;
    while (jiffies < end) cpu_relax();
}
void mdelay(u32 ms) { udelay(ms * 1000); }
