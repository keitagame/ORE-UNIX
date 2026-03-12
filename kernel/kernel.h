#ifndef _KERNEL_H
#define _KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ─── Compiler helpers ────────────────────────────────────────────────────── */
#define PACKED          __attribute__((packed))
#define ALIGNED(n)      __attribute__((aligned(n)))
#define NORETURN        __attribute__((noreturn))
#define UNUSED          __attribute__((unused))
#define LIKELY(x)       __builtin_expect(!!(x), 1)
#define UNLIKELY(x)     __builtin_expect(!!(x), 0)
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)  (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x,a) ((x) & ~((a) - 1))
#define MIN(a,b)        ((a)<(b)?(a):(b))
#define MAX(a,b)        ((a)>(b)?(a):(b))
#define OFFSETOF(t,m)   __builtin_offsetof(t, m)
#define CONTAINER_OF(ptr, type, member) \
    ((type *)((char*)(ptr) - OFFSETOF(type, member)))

/* ─── Basic types ─────────────────────────────────────────────────────────── */
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;
typedef uintptr_t uptr;
typedef intptr_t  sptr;
typedef u32       pid_t;
typedef u32       uid_t;
typedef u32       gid_t;
typedef s64       off_t;
typedef u64       ino_t;
typedef u32       dev_t;
typedef u32       mode_t;
typedef u32       nlink_t;

/* ─── Boolean ─────────────────────────────────────────────────────────────── */
#define true  1
#define false 0
typedef int bool;

/* ─── Error codes (POSIX compatible) ─────────────────────────────────────── */
#define EPERM    1
#define ENOENT   2
#define ESRCH    3
#define EINTR    4
#define EIO      5
#define ENXIO    6
#define EBADF    9
#define ECHILD   10
#define EAGAIN   11
#define ENOMEM   12
#define EACCES   13
#define EFAULT   14
#define EBUSY    16
#define EEXIST   17
#define EXDEV    18
#define ENODEV   19
#define ENOTDIR  20
#define EISDIR   21
#define EINVAL   22
#define ENFILE   23
#define EMFILE   24
#define ENOTTY   25
#define EFBIG    27
#define ENOSPC   28
#define ESPIPE   29
#define EROFS    30
#define EPIPE    32
#define ERANGE   34
#define ENOSYS   38
#define ENOTEMPTY 39
#define ELOOP    40
#define ENAMETOOLONG 36
#define EOVERFLOW 75

/* ─── Kernel panic ────────────────────────────────────────────────────────── */
NORETURN void panic(const char *fmt, ...);
#define ASSERT(cond) \
    do { if (UNLIKELY(!(cond))) panic("ASSERT(%s) at %s:%d", #cond, __FILE__, __LINE__); } while(0)
#define BUG() panic("BUG at %s:%d", __FILE__, __LINE__)

/* ─── Logging ─────────────────────────────────────────────────────────────── */
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, va_list ap);
#define KERN_INFO  "[INFO] "
#define KERN_WARN  "[WARN] "
#define KERN_ERR   "[ERR ] "
#define printk(fmt, ...) kprintf(fmt, ##__VA_ARGS__)

/* ─── Memory layout ───────────────────────────────────────────────────────── */
#define KERNEL_VIRT_BASE    0xC0000000UL   /* 3 GB */
#define KERNEL_PHYS_BASE    0x00100000UL   /* 1 MB */
#define KERNEL_HEAP_START   0xD0000000UL
#define KERNEL_HEAP_SIZE    (64 * 1024 * 1024)
#define USER_STACK_TOP      0xBFFFF000UL
#define USER_STACK_SIZE     (8 * 1024 * 1024)
#define PAGE_SIZE           4096
#define PAGE_SHIFT          12
#define PAGE_MASK           (~(PAGE_SIZE - 1))

#define phys_to_virt(p)     ((void *)((uptr)(p) + KERNEL_VIRT_BASE))
#define virt_to_phys(v)     ((uptr)(v) - KERNEL_VIRT_BASE)

/* ─── I/O ports ───────────────────────────────────────────────────────────── */
static inline void outb(u16 port, u8 val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port) : "memory");
}
static inline u8 inb(u16 port) {
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void outw(u16 port, u16 val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port) : "memory");
}
static inline u16 inw(u16 port) {
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void outl(u16 port, u32 val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port) : "memory");
}
static inline u32 inl(u16 port) {
    u32 v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
static inline void io_wait(void) { outb(0x80, 0); }

/* ─── CPU barriers ────────────────────────────────────────────────────────── */
#define mb()   __asm__ volatile("" ::: "memory")
#define rmb()  __asm__ volatile("" ::: "memory")
#define wmb()  __asm__ volatile("" ::: "memory")
#define cpu_relax() __asm__ volatile("pause" ::: "memory")

/* ─── Interrupt control ───────────────────────────────────────────────────── */
static inline void sti(void)  { __asm__ volatile("sti" ::: "memory"); }
static inline void cli(void)  { __asm__ volatile("cli" ::: "memory"); }
static inline u32  read_eflags(void) {
    u32 f; __asm__ volatile("pushfl; popl %0" : "=r"(f)); return f;
}
static inline bool irqs_enabled(void) { return (read_eflags() & 0x200) != 0; }

typedef u32 irq_flags_t;
#define local_irq_save(f)    do { f = read_eflags(); cli(); } while(0)
#define local_irq_restore(f) do { if ((f) & 0x200) sti(); else cli(); } while(0)
#define local_irq_disable()  cli()
#define local_irq_enable()   sti()

/* ─── Linked list ─────────────────────────────────────────────────────────── */
struct list_head {
    struct list_head *next, *prev;
};
#define LIST_HEAD_INIT(n) { &(n), &(n) }
#define LIST_HEAD(n)       struct list_head n = LIST_HEAD_INIT(n)
static inline void list_init(struct list_head *h) { h->next = h->prev = h; }
static inline void __list_add(struct list_head *n,
                               struct list_head *prev,
                               struct list_head *next) {
    next->prev = n; n->next = next; n->prev = prev; prev->next = n;
}
static inline void list_add(struct list_head *n, struct list_head *h) {
    __list_add(n, h, h->next);
}
static inline void list_add_tail(struct list_head *n, struct list_head *h) {
    __list_add(n, h->prev, h);
}
static inline void list_del(struct list_head *e) {
    e->prev->next = e->next; e->next->prev = e->prev;
    e->next = e->prev = e;
}
static inline bool list_empty(const struct list_head *h) { return h->next == h; }
#define list_entry(ptr, type, member) CONTAINER_OF(ptr, type, member)
#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_entry(pos, head, member) \
    for (pos = list_entry((head)->next, typeof(*pos), member); \
         &pos->member != (head); \
         pos = list_entry(pos->member.next, typeof(*pos), member))

/* ─── Spinlock ────────────────────────────────────────────────────────────── */
typedef struct { volatile int locked; } spinlock_t;
#define SPINLOCK_INIT { 0 }
static inline void spin_init(spinlock_t *l)  { l->locked = 0; }
static inline void spin_lock(spinlock_t *l) {
    while (__sync_lock_test_and_set(&l->locked, 1)) cpu_relax();
}
static inline void spin_unlock(spinlock_t *l) {
    __sync_lock_release(&l->locked);
}
static inline void spin_lock_irqsave(spinlock_t *l, irq_flags_t *f) {
    local_irq_save(*f); spin_lock(l);
}
static inline void spin_unlock_irqrestore(spinlock_t *l, irq_flags_t *f) {
    spin_unlock(l); local_irq_restore(*f);
}

/* ─── Wait queue ──────────────────────────────────────────────────────────── */
struct wait_queue_head {
    spinlock_t lock;
    struct list_head task_list;
};
typedef struct wait_queue_head wait_queue_head_t;
void init_waitqueue_head(wait_queue_head_t *q);
void wake_up(wait_queue_head_t *q);
void wake_up_all(wait_queue_head_t *q);

/* ─── String functions ────────────────────────────────────────────────────── */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strdup(const char *s);
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);

#endif /* _KERNEL_H */
