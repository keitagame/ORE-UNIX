#ifndef _MM_H
#define _MM_H

#include "kernel.h"

/* ─── Physical memory manager ────────────────────────────────────────────── */
#define PHYS_MEM_MAX    (1024 * 1024 * 1024)   /* 1 GB max supported */

void  pmm_init(u32 mem_kb, uptr mmap_addr, u32 mmap_len);
uptr  pmm_alloc(void);              /* allocate one physical page, returns phys addr */
uptr  pmm_alloc_zero(void);         /* allocate + zero */
void  pmm_free(uptr phys);          /* free one physical page */
uptr  pmm_alloc_contiguous(u32 n);  /* allocate n contiguous pages */
void  pmm_free_contiguous(uptr phys, u32 n);
u32   pmm_free_pages(void);
u32   pmm_total_pages(void);
void  pmm_mark_used(uptr start, uptr end);

/* ─── Virtual memory / paging ─────────────────────────────────────────────── */
struct vm_area;
struct mm_struct;

/* Page directory reference (physical address of PD) */
typedef u32 pgdir_t;

void     paging_init(void);
pgdir_t  pgdir_create(void);
void     pgdir_destroy(pgdir_t pd);
pgdir_t  pgdir_clone(pgdir_t src);

int  map_page(pgdir_t pd, uptr va, uptr pa, u32 flags);
int  map_pages(pgdir_t pd, uptr va, uptr pa, u32 n, u32 flags);
void unmap_page(pgdir_t pd, uptr va);
void unmap_pages(pgdir_t pd, uptr va, u32 n);
uptr virt_to_phys_pd(pgdir_t pd, uptr va);

void paging_switch(pgdir_t pd);
extern pgdir_t kernel_pgdir;

/* Page flags for map_page */
#define VM_READ    (1 << 0)
#define VM_WRITE   (1 << 1)
#define VM_EXEC    (1 << 2)
#define VM_USER    (1 << 3)
#define VM_NOCACHE (1 << 4)

/* ─── vm_area (VMA): user address space regions ───────────────────────────── */
struct vm_area {
    uptr            start;
    uptr            end;       /* exclusive */
    u32             flags;     /* VM_READ | VM_WRITE | VM_EXEC */
    struct list_head list;
};

/* ─── mm_struct: per-process memory descriptor ──────────────────────────────*/
struct mm_struct {
    pgdir_t           pgdir;
    struct list_head  vmas;       /* list of vm_area */
    uptr              brk;        /* current heap top */
    uptr              start_brk;
    uptr              start_stack;
    u32               map_count;
    spinlock_t        lock;
};

struct mm_struct *mm_create(void);
void              mm_destroy(struct mm_struct *mm);
struct mm_struct *mm_clone(struct mm_struct *src);   /* for fork() */

int   mm_mmap(struct mm_struct *mm, uptr addr, size_t len, u32 flags);
int   mm_munmap(struct mm_struct *mm, uptr addr, size_t len);
int   mm_brk(struct mm_struct *mm, uptr new_brk);
struct vm_area *mm_find_vma(struct mm_struct *mm, uptr addr);

/* page fault handler */
void  page_fault_handler(uptr fault_addr, u32 err_code);

/* ─── Kernel heap allocator ───────────────────────────────────────────────── */
void  kmalloc_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t nmemb, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);
void *kmalloc_aligned(size_t size, size_t align);

/* convenience */
#define kzalloc(sz) kcalloc(1, sz)

/* ─── Slab allocator for small fixed-size objects ────────────────────────── */
struct kmem_cache {
    size_t           obj_size;
    size_t           slab_capacity;
    struct list_head slabs_full;
    struct list_head slabs_partial;
    struct list_head slabs_free;
    spinlock_t       lock;
    const char      *name;
};

struct kmem_cache *kmem_cache_create(const char *name, size_t obj_size);
void               kmem_cache_destroy(struct kmem_cache *c);
void              *kmem_cache_alloc(struct kmem_cache *c);
void               kmem_cache_free(struct kmem_cache *c, void *obj);

#endif /* _MM_H */
