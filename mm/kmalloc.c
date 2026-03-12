/* mm/kmalloc.c – kernel heap: simple free-list + slab cache */
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <arch/x86/arch.h>

/* ─── Heap allocator ─────────────────────────────────────────────────────── */
/* Uses a next-fit free list. Each block has a small header.                 */

#define MAGIC_FREE  0xFEEDFACE
#define MAGIC_USED  0xDEADBEEF
#define MIN_ALLOC   16

struct block_hdr {
    u32   magic;
    u32   size;        /* payload size (not including header) */
    struct block_hdr *next;
    struct block_hdr *prev;
};
#define HDR_SIZE  sizeof(struct block_hdr)

static struct block_hdr *heap_start = NULL;
static spinlock_t heap_lock = SPINLOCK_INIT;

static uptr heap_cur = 0;   /* current sbrk-like pointer */
static uptr heap_end = 0;

static void *heap_sbrk(size_t bytes) {
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    if (heap_cur + bytes > heap_end) return NULL;
    void *p = (void *)heap_cur;
    heap_cur += bytes;
    /* pages mapped on demand by page fault handler */
    return p;
}

void kmalloc_init(void) {
    heap_cur = KERNEL_HEAP_START;
    heap_end = KERNEL_HEAP_START + KERNEL_HEAP_SIZE;
    /* pre-allocate first chunk */
    void *mem = heap_sbrk(PAGE_SIZE * 4);
    ASSERT(mem);
    heap_start = (struct block_hdr *)mem;
    heap_start->magic = MAGIC_FREE;
    heap_start->size  = PAGE_SIZE * 4 - HDR_SIZE;
    heap_start->next  = NULL;
    heap_start->prev  = NULL;
    printk(KERN_INFO "KMALLOC: heap at %08x\n", KERNEL_HEAP_START);
}

static void split_block(struct block_hdr *b, size_t need) {
    if (b->size < need + HDR_SIZE + MIN_ALLOC) return;  /* too small to split */
    struct block_hdr *nb = (struct block_hdr *)((char*)b + HDR_SIZE + need);
    nb->magic = MAGIC_FREE;
    nb->size  = b->size - need - HDR_SIZE;
    nb->next  = b->next;
    nb->prev  = b;
    if (b->next) b->next->prev = nb;
    b->next = nb;
    b->size = need;
}

void *kmalloc(size_t size) {
    if (!size) return NULL;
    size = ALIGN_UP(size, 8);

    irq_flags_t f;
    spin_lock_irqsave(&heap_lock, &f);

    struct block_hdr *b = heap_start;
    while (b) {
        if (b->magic == MAGIC_FREE && b->size >= size) {
            split_block(b, size);
            b->magic = MAGIC_USED;
            spin_unlock_irqrestore(&heap_lock, &f);
            return (void*)((char*)b + HDR_SIZE);
        }
        b = b->next;
    }

    /* need more heap */
    size_t ext = ALIGN_UP(size + HDR_SIZE, PAGE_SIZE * 4);
    void *mem = heap_sbrk(ext);
    if (!mem) {
        spin_unlock_irqrestore(&heap_lock, &f);
        return NULL;
    }
    struct block_hdr *nb = (struct block_hdr *)mem;
    nb->magic = MAGIC_FREE;
    nb->size  = ext - HDR_SIZE;
    nb->next  = NULL;
    /* append to list */
    struct block_hdr *last = heap_start;
    while (last->next) last = last->next;
    last->next = nb;
    nb->prev   = last;
    /* try again */
    spin_unlock_irqrestore(&heap_lock, &f);
    return kmalloc(size);
}

void kfree(void *ptr) {
    if (!ptr) return;
    struct block_hdr *b = (struct block_hdr *)((char*)ptr - HDR_SIZE);
    if (b->magic != MAGIC_USED) {
        printk(KERN_ERR "kfree: bad magic at %p\n", ptr);
        return;
    }

    irq_flags_t f;
    spin_lock_irqsave(&heap_lock, &f);
    b->magic = MAGIC_FREE;

    /* coalesce with next */
    if (b->next && b->next->magic == MAGIC_FREE) {
        b->size += HDR_SIZE + b->next->size;
        struct block_hdr *n = b->next->next;
        if (n) n->prev = b;
        b->next = n;
    }
    /* coalesce with prev */
    if (b->prev && b->prev->magic == MAGIC_FREE) {
        b->prev->size += HDR_SIZE + b->size;
        struct block_hdr *n = b->next;
        if (n) n->prev = b->prev;
        b->prev->next = n;
    }
    spin_unlock_irqrestore(&heap_lock, &f);
}

void *kcalloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (!size) { kfree(ptr); return NULL; }
    struct block_hdr *b = (struct block_hdr *)((char*)ptr - HDR_SIZE);
    if (b->size >= size) return ptr;
    void *np = kmalloc(size);
    if (np) { memcpy(np, ptr, b->size); kfree(ptr); }
    return np;
}

void *kmalloc_aligned(size_t size, size_t align) {
    /* simple: over-allocate and align */
    void *p = kmalloc(size + align);
    if (!p) return NULL;
    uptr a = ALIGN_UP((uptr)p, align);
    return (void*)a;
}

/* ─── Slab allocator ─────────────────────────────────────────────────────── */
#define SLAB_MAGIC  0xA1ACED

struct slab_hdr {
    u32              magic;
    struct kmem_cache *cache;
    u32              inuse;
    u32              capacity;
    u32             *free_list;  /* stack of free indices */
    u32              free_top;
    struct list_head list;
    char             data[];
};

struct kmem_cache *kmem_cache_create(const char *name, size_t obj_size) {
    struct kmem_cache *c = kmalloc(sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->name = name;
    c->obj_size = ALIGN_UP(obj_size, 8);
    c->slab_capacity = MAX(1, (PAGE_SIZE * 4) / c->obj_size);
    list_init(&c->slabs_full);
    list_init(&c->slabs_partial);
    list_init(&c->slabs_free);
    spin_init(&c->lock);
    return c;
}

static struct slab_hdr *slab_new(struct kmem_cache *c) {
    size_t overhead = sizeof(struct slab_hdr) + c->slab_capacity * sizeof(u32);
    size_t total    = ALIGN_UP(overhead + c->slab_capacity * c->obj_size, PAGE_SIZE);
    struct slab_hdr *s = kmalloc(total);
    if (!s) return NULL;
    s->magic     = SLAB_MAGIC;
    s->cache     = c;
    s->inuse     = 0;
    s->capacity  = c->slab_capacity;
    s->free_list = (u32*)((char*)s + sizeof(struct slab_hdr));
    s->free_top  = c->slab_capacity;
    for (u32 i = 0; i < c->slab_capacity; i++) s->free_list[i] = i;
    list_init(&s->list);
    return s;
}

void *kmem_cache_alloc(struct kmem_cache *c) {
    irq_flags_t f;
    spin_lock_irqsave(&c->lock, &f);

    struct slab_hdr *s = NULL;
    if (!list_empty(&c->slabs_partial))
        s = list_entry(c->slabs_partial.next, struct slab_hdr, list);
    else if (!list_empty(&c->slabs_free))
        s = list_entry(c->slabs_free.next, struct slab_hdr, list);
    else {
        spin_unlock_irqrestore(&c->lock, &f);
        s = slab_new(c);
        if (!s) return NULL;
        spin_lock_irqsave(&c->lock, &f);
        list_add(&s->list, &c->slabs_free);
    }

    list_del(&s->list);
    u32 idx = s->free_list[--s->free_top];
    s->inuse++;
    char *obj_area = (char*)s->free_list + c->slab_capacity * sizeof(u32);
    void *obj = obj_area + idx * c->obj_size;

    if (s->inuse == s->capacity) list_add(&s->list, &c->slabs_full);
    else                          list_add(&s->list, &c->slabs_partial);

    spin_unlock_irqrestore(&c->lock, &f);
    return obj;
}

void kmem_cache_free(struct kmem_cache *c, void *obj) {
    if (!obj) return;
    irq_flags_t f;
    spin_lock_irqsave(&c->lock, &f);

    /* find slab */
    char *o = (char*)obj;
    /* try partial + full lists */
    struct list_head *lists[] = { &c->slabs_partial, &c->slabs_full, NULL };
    for (int i = 0; lists[i]; i++) {
        struct list_head *pos;
        list_for_each(pos, lists[i]) {
            struct slab_hdr *s = list_entry(pos, struct slab_hdr, list);
            char *obj_area = (char*)s->free_list + c->slab_capacity * sizeof(u32);
            if (o >= obj_area && o < obj_area + s->capacity * c->obj_size) {
                u32 idx = (u32)(o - obj_area) / (u32)c->obj_size;
                s->free_list[s->free_top++] = idx;
                s->inuse--;
                list_del(&s->list);
                if (s->inuse == 0) list_add(&s->list, &c->slabs_free);
                else               list_add(&s->list, &c->slabs_partial);
                spin_unlock_irqrestore(&c->lock, &f);
                return;
            }
        }
    }
    spin_unlock_irqrestore(&c->lock, &f);
    printk(KERN_ERR "kmem_cache_free: object %p not found in cache %s\n",
           obj, c->name);
}

void kmem_cache_destroy(struct kmem_cache *c) {
    /* free all slabs */
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &c->slabs_free) {
        kfree(list_entry(pos, struct slab_hdr, list));
    }
    list_for_each_safe(pos, n, &c->slabs_partial) {
        kfree(list_entry(pos, struct slab_hdr, list));
    }
    list_for_each_safe(pos, n, &c->slabs_full) {
        kfree(list_entry(pos, struct slab_hdr, list));
    }
    kfree(c);
}
