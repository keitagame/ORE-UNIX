/* mm/pmm.c – Physical memory manager (bitmap allocator) */
#include <kernel/kernel.h>
#include <kernel/mm.h>

/* Multiboot2 memory-map entry types */
#define MB2_MEM_AVAILABLE   1
#define MB2_MEM_RESERVED    2
#define MB2_MEM_ACPI_RECL   3
#define MB2_MEM_ACPI_NVS    4
#define MB2_MEM_BAD         5

/* ─── Bitmap ──────────────────────────────────────────────────────────────── */
#define BITMAP_BITS   (PHYS_MEM_MAX / PAGE_SIZE)
#define BITMAP_WORDS  (BITMAP_BITS / 32)

static u32 bitmap[BITMAP_WORDS];   /* 1 = free, 0 = used */
static u32 total_pages_count  = 0;
static u32 free_pages_count   = 0;
static spinlock_t pmm_lock = SPINLOCK_INIT;

#define BIT_SET(i)   (bitmap[(i)/32] |=  (1u << ((i)%32)))
#define BIT_CLR(i)   (bitmap[(i)/32] &= ~(1u << ((i)%32)))
#define BIT_TST(i)   ((bitmap[(i)/32] >> ((i)%32)) & 1)

/* Multiboot2 tag helpers */
struct mb2_tag {
    u32 type;
    u32 size;
} PACKED;

struct mb2_tag_mmap {
    u32 type;       /* 6 */
    u32 size;
    u32 entry_size;
    u32 entry_version;
    /* entries follow */
} PACKED;

struct mb2_mmap_entry {
    u64 base_addr;
    u64 length;
    u32 type;
    u32 reserved;
} PACKED;

/* externals from linker script */
extern char _kernel_phys_end[];

void pmm_init(u32 mem_kb, uptr mmap_addr, u32 mmap_len) {
    (void)mem_kb;
    memset(bitmap, 0, sizeof(bitmap));   /* mark everything used */

    /* parse Multiboot2 memory map */
    if (mmap_addr) {
        struct mb2_tag_mmap *tag = (struct mb2_tag_mmap *)phys_to_virt(mmap_addr);
        u8 *end = (u8*)tag + mmap_len;
        struct mb2_mmap_entry *e = (struct mb2_mmap_entry *)((u8*)tag + 16);

        while ((u8*)e < end) {
            if (e->type == MB2_MEM_AVAILABLE) {
                u64 base = e->base_addr;
                u64 len  = e->length;
                /* align up base, down end */
                uptr page_base = (uptr)ALIGN_UP(base, PAGE_SIZE);
                uptr page_end  = (uptr)ALIGN_DOWN(base + len, PAGE_SIZE);
                for (uptr p = page_base; p < page_end; p += PAGE_SIZE) {
                    u32 idx = p / PAGE_SIZE;
                    if (idx < BITMAP_BITS) {
                        BIT_SET(idx);
                        total_pages_count++;
                        free_pages_count++;
                    }
                }
            }
            e = (struct mb2_mmap_entry *)((u8*)e + tag->entry_size);
        }
    } else {
        /* fallback: assume 64 MB conventional */
        for (u32 i = 256; i < 16384; i++) {   /* 1 MB .. 64 MB */
            BIT_SET(i);
            total_pages_count++;
            free_pages_count++;
        }
    }

    /* mark kernel itself as used */
    uptr kend = (uptr)_kernel_phys_end;
    kend = ALIGN_UP(kend, PAGE_SIZE);
    pmm_mark_used(0, kend + PAGE_SIZE * 16);  /* +16 pages safety margin */

    /* mark first page (null page) always used */
    BIT_CLR(0);

    printk(KERN_INFO "PMM: %u MB available (%u pages)\n",
           (free_pages_count * PAGE_SIZE) / (1024*1024), free_pages_count);
}

void pmm_mark_used(uptr start, uptr end) {
    start = ALIGN_DOWN(start, PAGE_SIZE);
    end   = ALIGN_UP(end,   PAGE_SIZE);
    for (uptr p = start; p < end; p += PAGE_SIZE) {
        u32 idx = p / PAGE_SIZE;
        if (idx < BITMAP_BITS && BIT_TST(idx)) {
            BIT_CLR(idx);
            if (free_pages_count) free_pages_count--;
        }
    }
}

uptr pmm_alloc(void) {
    irq_flags_t f;
    spin_lock_irqsave(&pmm_lock, &f);

    /* scan bitmap for first free bit */
    for (u32 w = 0; w < BITMAP_WORDS; w++) {
        if (bitmap[w]) {
            u32 bit = __builtin_ctz(bitmap[w]);
            BIT_CLR(w * 32 + bit);
            free_pages_count--;
            spin_unlock_irqrestore(&pmm_lock, &f);
            return (uptr)(w * 32 + bit) * PAGE_SIZE;
        }
    }

    spin_unlock_irqrestore(&pmm_lock, &f);
    return 0;   /* out of memory */
}

uptr pmm_alloc_zero(void) {
    uptr p = pmm_alloc();
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

void pmm_free(uptr phys) {
    if (!phys || phys >= PHYS_MEM_MAX) return;
    u32 idx = phys / PAGE_SIZE;
    irq_flags_t f;
    spin_lock_irqsave(&pmm_lock, &f);
    if (!BIT_TST(idx)) {
        BIT_SET(idx);
        free_pages_count++;
    }
    spin_unlock_irqrestore(&pmm_lock, &f);
}

uptr pmm_alloc_contiguous(u32 n) {
    if (!n) return 0;
    irq_flags_t f;
    spin_lock_irqsave(&pmm_lock, &f);
    u32 found = 0, start = 0;
    for (u32 i = 1; i < BITMAP_BITS; i++) {
        if (BIT_TST(i)) {
            if (!found) start = i;
            found++;
            if (found == n) {
                for (u32 j = start; j < start + n; j++) BIT_CLR(j);
                free_pages_count -= n;
                spin_unlock_irqrestore(&pmm_lock, &f);
                return (uptr)start * PAGE_SIZE;
            }
        } else {
            found = 0;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, &f);
    return 0;
}

void pmm_free_contiguous(uptr phys, u32 n) {
    for (u32 i = 0; i < n; i++) pmm_free(phys + (uptr)i * PAGE_SIZE);
}

u32 pmm_free_pages(void)  { return free_pages_count; }
u32 pmm_total_pages(void) { return total_pages_count; }
