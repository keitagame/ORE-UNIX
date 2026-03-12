/* mm/vmm.c – Virtual memory manager: page tables, VMAs, page fault */
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <arch/x86/arch.h>

pgdir_t kernel_pgdir = 0;

/* ─── Low-level page table helpers ──────────────────────────────────────── */
static pde_t *pgdir_virt(pgdir_t pd) {
    return (pde_t *)phys_to_virt(pd);
}

static pte_t *get_pte(pgdir_t pd, uptr va, bool create) {
    pde_t *pde_ptr = &pgdir_virt(pd)[PD_INDEX(va)];
    uptr pt_phys;

    if (!(*pde_ptr & PDE_PRESENT)) {
        if (!create) return NULL;
        pt_phys = pmm_alloc_zero();
        if (!pt_phys) return NULL;
        *pde_ptr = pt_phys | PDE_PRESENT | PDE_WRITE | PDE_USER;
    } else {
        pt_phys = PTE_ADDR(*pde_ptr);
    }

    pte_t *pt = (pte_t *)phys_to_virt(pt_phys);
    return &pt[PT_INDEX(va)];
}

int map_page(pgdir_t pd, uptr va, uptr pa, u32 flags) {
    pte_t *pte = get_pte(pd, va, true);
    if (!pte) return -ENOMEM;

    u32 pte_flags = PTE_PRESENT;
    if (flags & VM_WRITE)   pte_flags |= PTE_WRITE;
    if (flags & VM_USER)    pte_flags |= PTE_USER;
    if (flags & VM_NOCACHE) pte_flags |= PTE_PCD;

    *pte = (pa & PAGE_MASK) | pte_flags;
    tlb_flush_page(va);
    return 0;
}

int map_pages(pgdir_t pd, uptr va, uptr pa, u32 n, u32 flags) {
    for (u32 i = 0; i < n; i++) {
        int r = map_page(pd, va + i*PAGE_SIZE, pa + i*PAGE_SIZE, flags);
        if (r) return r;
    }
    return 0;
}

void unmap_page(pgdir_t pd, uptr va) {
    pte_t *pte = get_pte(pd, va, false);
    if (pte && (*pte & PTE_PRESENT)) {
        *pte = 0;
        tlb_flush_page(va);
    }
}

void unmap_pages(pgdir_t pd, uptr va, u32 n) {
    for (u32 i = 0; i < n; i++) unmap_page(pd, va + i*PAGE_SIZE);
}

uptr virt_to_phys_pd(pgdir_t pd, uptr va) {
    pte_t *pte = get_pte(pd, va, false);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
    return PTE_ADDR(*pte) | (va & (PAGE_SIZE - 1));
}

void paging_switch(pgdir_t pd) {
    write_cr3(pd);
}

/* ─── Kernel page directory ─────────────────────────────────────────────── */
void paging_init(void) {
    uptr pd_phys = pmm_alloc_zero();
    ASSERT(pd_phys);
    kernel_pgdir = (pgdir_t)pd_phys;

    /* map kernel: virtual [0xC0000000..0xC0400000] -> phys [0..4 MB] */
    for (uptr p = 0; p < 4*1024*1024; p += PAGE_SIZE) {
        map_page(kernel_pgdir, KERNEL_VIRT_BASE + p, p,
                 VM_WRITE);  /* kernel only */
    }

    /* also keep the kernel heap region */
    /* (pages allocated on demand by page fault handler) */

    paging_switch(kernel_pgdir);
    printk(KERN_INFO "VMM: paging initialized (PGDIR=%08x)\n", kernel_pgdir);
}

/* ─── Page directory create / clone / destroy ───────────────────────────── */
pgdir_t pgdir_create(void) {
    uptr pd_phys = pmm_alloc_zero();
    if (!pd_phys) return 0;

    /* copy kernel mappings (upper half: PD entries 768..1023) */
    pde_t *kpd = pgdir_virt(kernel_pgdir);
    pde_t *upd = (pde_t *)phys_to_virt(pd_phys);
    memcpy(upd + 768, kpd + 768, (1024 - 768) * sizeof(pde_t));
    return (pgdir_t)pd_phys;
}

/* deep clone of user pages */
pgdir_t pgdir_clone(pgdir_t src) {
    pgdir_t dst = pgdir_create();
    if (!dst) return 0;

    pde_t *spd = pgdir_virt(src);
    pde_t *dpd = pgdir_virt(dst);

    /* clone user half (PDE 0..767) */
    for (int di = 0; di < 768; di++) {
        if (!(spd[di] & PDE_PRESENT)) continue;
        uptr pt_phys = pmm_alloc_zero();
        if (!pt_phys) { pgdir_destroy(dst); return 0; }

        pte_t *spt = (pte_t *)phys_to_virt(PTE_ADDR(spd[di]));
        pte_t *dpt = (pte_t *)phys_to_virt(pt_phys);

        for (int pi = 0; pi < 1024; pi++) {
            if (!(spt[pi] & PTE_PRESENT)) continue;
            /* copy-on-write: for now do physical copy */
            uptr page_phys = pmm_alloc();
            if (!page_phys) { pgdir_destroy(dst); return 0; }
            memcpy(phys_to_virt(page_phys),
                   phys_to_virt(PTE_ADDR(spt[pi])), PAGE_SIZE);
            dpt[pi] = page_phys | (spt[pi] & (PAGE_SIZE - 1));
        }
        dpd[di] = pt_phys | (spd[di] & (PAGE_SIZE - 1));
    }
    return dst;
}

void pgdir_destroy(pgdir_t pd) {
    if (!pd || pd == kernel_pgdir) return;
    pde_t *pde = pgdir_virt(pd);
    for (int di = 0; di < 768; di++) {
        if (!(pde[di] & PDE_PRESENT)) continue;
        pte_t *pt = (pte_t *)phys_to_virt(PTE_ADDR(pde[di]));
        for (int pi = 0; pi < 1024; pi++) {
            if (pt[pi] & PTE_PRESENT) pmm_free(PTE_ADDR(pt[pi]));
        }
        pmm_free(PTE_ADDR(pde[di]));
    }
    pmm_free(pd);
}

/* ─── mm_struct ─────────────────────────────────────────────────────────── */
static struct kmem_cache *mm_cache;
static struct kmem_cache *vma_cache;

void mm_init_caches(void) {
    mm_cache  = kmem_cache_create("mm_struct",  sizeof(struct mm_struct));
    vma_cache = kmem_cache_create("vm_area",    sizeof(struct vm_area));
}

struct mm_struct *mm_create(void) {
    struct mm_struct *mm = kmem_cache_alloc(mm_cache);
    if (!mm) return NULL;
    memset(mm, 0, sizeof(*mm));
    mm->pgdir = pgdir_create();
    if (!mm->pgdir) { kmem_cache_free(mm_cache, mm); return NULL; }
    list_init(&mm->vmas);
    spin_init(&mm->lock);
    mm->start_brk = 0x08048000 + 64*1024*1024; /* after typical ELF */
    mm->brk       = mm->start_brk;
    mm->start_stack = USER_STACK_TOP;
    return mm;
}

void mm_destroy(struct mm_struct *mm) {
    if (!mm) return;
    /* free VMAs */
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &mm->vmas) {
        struct vm_area *vma = list_entry(pos, struct vm_area, list);
        list_del(&vma->list);
        kmem_cache_free(vma_cache, vma);
    }
    pgdir_destroy(mm->pgdir);
    kmem_cache_free(mm_cache, mm);
}

struct mm_struct *mm_clone(struct mm_struct *src) {
    struct mm_struct *dst = kmem_cache_alloc(mm_cache);
    if (!dst) return NULL;
    memset(dst, 0, sizeof(*dst));
    dst->pgdir = pgdir_clone(src->pgdir);
    if (!dst->pgdir) { kmem_cache_free(mm_cache, dst); return NULL; }
    list_init(&dst->vmas);
    spin_init(&dst->lock);
    dst->brk        = src->brk;
    dst->start_brk  = src->start_brk;
    dst->start_stack = src->start_stack;

    /* clone VMAs */
    struct vm_area *vma;
    list_for_each_entry(vma, &src->vmas, list) {
        struct vm_area *nv = kmem_cache_alloc(vma_cache);
        if (!nv) { mm_destroy(dst); return NULL; }
        *nv = *vma;
        list_add_tail(&nv->list, &dst->vmas);
        dst->map_count++;
    }
    return dst;
}

static struct vm_area *vma_alloc(uptr start, uptr end, u32 flags) {
    struct vm_area *vma = kmem_cache_alloc(vma_cache);
    if (!vma) return NULL;
    vma->start = start;
    vma->end   = end;
    vma->flags = flags;
    list_init(&vma->list);
    return vma;
}

int mm_mmap(struct mm_struct *mm, uptr addr, size_t len, u32 flags) {
    len = ALIGN_UP(len, PAGE_SIZE);
    /* allocate and map pages */
    for (uptr p = addr; p < addr + len; p += PAGE_SIZE) {
        uptr phys = pmm_alloc_zero();
        if (!phys) return -ENOMEM;
        u32 pf = VM_USER;
        if (flags & VM_WRITE) pf |= VM_WRITE;
        if (flags & VM_READ)  pf |= VM_READ;
        map_page(mm->pgdir, p, phys, pf);
    }
    struct vm_area *vma = vma_alloc(addr, addr + len, flags);
    if (!vma) return -ENOMEM;
    list_add_tail(&vma->list, &mm->vmas);
    mm->map_count++;
    return 0;
}

int mm_munmap(struct mm_struct *mm, uptr addr, size_t len) {
    len = ALIGN_UP(len, PAGE_SIZE);
    /* free physical pages */
    for (uptr p = addr; p < addr + len; p += PAGE_SIZE) {
        uptr phys = virt_to_phys_pd(mm->pgdir, p);
        if (phys) pmm_free(phys & PAGE_MASK);
        unmap_page(mm->pgdir, p);
    }
    /* remove VMAs that overlap */
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &mm->vmas) {
        struct vm_area *vma = list_entry(pos, struct vm_area, list);
        if (vma->end <= addr || vma->start >= addr + len) continue;
        list_del(&vma->list);
        kmem_cache_free(vma_cache, vma);
        mm->map_count--;
    }
    return 0;
}

int mm_brk(struct mm_struct *mm, uptr new_brk) {
    new_brk = ALIGN_UP(new_brk, PAGE_SIZE);
    if (new_brk < mm->start_brk) return -EINVAL;
    if (new_brk > mm->brk) {
        /* expand */
        int r = mm_mmap(mm, mm->brk, new_brk - mm->brk,
                        VM_READ | VM_WRITE);
        if (r) return r;
    } else if (new_brk < mm->brk) {
        /* shrink */
        mm_munmap(mm, new_brk, mm->brk - new_brk);
    }
    mm->brk = new_brk;
    return 0;
}

struct vm_area *mm_find_vma(struct mm_struct *mm, uptr addr) {
    struct vm_area *vma;
    list_for_each_entry(vma, &mm->vmas, list) {
        if (addr >= vma->start && addr < vma->end) return vma;
    }
    return NULL;
}

/* ─── Page fault handler ─────────────────────────────────────────────────── */
void page_fault_handler(uptr fault_addr, u32 err_code) {
    bool present  = err_code & 1;
    bool write    = (err_code >> 1) & 1;
    bool user     = (err_code >> 2) & 1;

    extern struct task_struct *current;

    if (user && current && current->mm) {
        struct mm_struct *mm = current->mm;
        struct vm_area *vma = mm_find_vma(mm, fault_addr);
        if (vma) {
            if (write && !(vma->flags & VM_WRITE)) {
                /* protection fault */
                goto kill;
            }
            if (!present) {
                /* demand paging: allocate page */
                uptr page = pmm_alloc_zero();
                if (!page) goto kill;
                map_page(mm->pgdir, ALIGN_DOWN(fault_addr, PAGE_SIZE),
                         page, vma->flags | VM_USER);
                return;
            }
        }
    }

    if (!user) {
        /* kernel page fault: could be kernel heap demand */
        if (fault_addr >= KERNEL_HEAP_START &&
            fault_addr < KERNEL_HEAP_START + KERNEL_HEAP_SIZE && !present) {
            uptr page = pmm_alloc_zero();
            if (!page) panic("Kernel OOM in page fault at %08x", fault_addr);
            map_page(kernel_pgdir, ALIGN_DOWN(fault_addr, PAGE_SIZE),
                     page, VM_WRITE);
            return;
        }
        panic("Kernel page fault at %08x (err=%08x)", fault_addr, err_code);
    }

kill:
    printk(KERN_WARN "Segfault: pid=%d addr=%08x write=%d\n",
           current ? (int)current->pid : -1, fault_addr, write);
    sys_kill(current->pid, SIGSEGV);
}
