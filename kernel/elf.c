/* kernel/elf.c – ELF32 loader + sys_exec */
#include <kernel/kernel.h>
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <arch/x86/arch.h>

/* ─── ELF32 structs ───────────────────────────────────────────────────────── */
#define ELF_MAGIC    0x464C457FU
#define ET_EXEC      2
#define EM_386       3
#define PT_LOAD      1
#define PT_INTERP    3
#define PF_X  1
#define PF_W  2
#define PF_R  4

typedef struct {
    u32 e_magic;
    u8  e_class, e_data, e_version2, e_osabi;
    u8  e_pad[8];
    u16 e_type, e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum;
    u16 e_shentsize, e_shnum, e_shstrndx;
} PACKED Elf32_Ehdr;

typedef struct {
    u32 p_type, p_offset, p_vaddr, p_paddr;
    u32 p_filesz, p_memsz, p_flags, p_align;
} PACKED Elf32_Phdr;

/* AT_* auxiliary vector types */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14

/* ─── copy strings to user stack ─────────────────────────────────────────── */
static uptr push_strings(struct mm_struct *mm, uptr sp,
                          char *const strs[], int *count_out) {
    int count = 0;
    if (strs) {
        while (strs[count]) count++;
    }
    *count_out = count;

    /* push strings in reverse */
    uptr *ptrs = kmalloc((count + 1) * sizeof(uptr));
    if (!ptrs) return 0;

    for (int i = count - 1; i >= 0; i--) {
        size_t len = strlen(strs[i]) + 1;
        sp -= len;
        /* map page if needed */
        uptr page_start = ALIGN_DOWN(sp, PAGE_SIZE);
        if (!virt_to_phys_pd(mm->pgdir, page_start)) {
            uptr phys = pmm_alloc_zero();
            map_page(mm->pgdir, page_start, phys, VM_READ | VM_WRITE | VM_USER);
        }
        /* write string to user space via kernel mapping */
        uptr paddr = virt_to_phys_pd(mm->pgdir, sp);
        memcpy(phys_to_virt(paddr), strs[i], len);
        ptrs[i] = sp;
    }
    ptrs[count] = 0;

    /* align sp */
    sp = ALIGN_DOWN(sp, 4);

    /* push pointer array (including null terminator) */
    sp -= (count + 1) * sizeof(uptr);
    uptr page_start = ALIGN_DOWN(sp, PAGE_SIZE);
    if (!virt_to_phys_pd(mm->pgdir, page_start)) {
        uptr phys = pmm_alloc_zero();
        map_page(mm->pgdir, page_start, phys, VM_READ | VM_WRITE | VM_USER);
    }
    uptr paddr = virt_to_phys_pd(mm->pgdir, sp);
    memcpy(phys_to_virt(paddr), ptrs, (count + 1) * sizeof(uptr));

    kfree(ptrs);
    return sp;
}

static void stack_push32(struct mm_struct *mm, uptr *sp, u32 val) {
    *sp -= 4;
    uptr page_start = ALIGN_DOWN(*sp, PAGE_SIZE);
    if (!virt_to_phys_pd(mm->pgdir, page_start)) {
        uptr phys = pmm_alloc_zero();
        map_page(mm->pgdir, page_start, phys, VM_READ | VM_WRITE | VM_USER);
    }
    uptr paddr = virt_to_phys_pd(mm->pgdir, *sp);
    *(u32*)phys_to_virt(paddr) = val;
}

/* ─── sys_exec ────────────────────────────────────────────────────────────── */
int sys_exec(const char *path, char *const argv[], char *const envp[]) {
    /* ── 1. open ELF ── */
    struct file *f = vfs_open(path, O_RDONLY, 0);
    if (!f) return -ENOENT;

    Elf32_Ehdr ehdr;
    off_t pos = 0;
    ssize_t r = f->f_ops->read(f, (char*)&ehdr, sizeof(ehdr), &pos);
    if (r != (ssize_t)sizeof(ehdr)) { vfs_close(f); return -ENOEXEC; }

    if (ehdr.e_magic   != ELF_MAGIC ||
        ehdr.e_class   != 1         ||
        ehdr.e_type    != ET_EXEC   ||
        ehdr.e_machine != EM_386) {
        vfs_close(f);
        return -ENOEXEC;
    }

    /* ── 2. read program headers ── */
    u16 phnum = ehdr.e_phnum;
    Elf32_Phdr *phdrs = kmalloc(phnum * sizeof(Elf32_Phdr));
    if (!phdrs) { vfs_close(f); return -ENOMEM; }

    pos = ehdr.e_phoff;
    r = f->f_ops->read(f, (char*)phdrs, phnum * sizeof(Elf32_Phdr), &pos);
    if (r != (ssize_t)(phnum * sizeof(Elf32_Phdr))) {
        kfree(phdrs); vfs_close(f); return -ENOEXEC;
    }

    /* ── 3. create new mm ── */
    struct mm_struct *new_mm = mm_create();
    if (!new_mm) { kfree(phdrs); vfs_close(f); return -ENOMEM; }

    /* ── 4. load PT_LOAD segments ── */
    uptr load_base = 0, load_end = 0;
    for (int i = 0; i < phnum; i++) {
        Elf32_Phdr *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0) continue;

        uptr vaddr   = ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE);
        uptr vend    = ALIGN_UP(ph->p_vaddr + ph->p_memsz, PAGE_SIZE);
        u32  npages  = (vend - vaddr) / PAGE_SIZE;

        u32 vm_flags = VM_USER | VM_READ;
        if (ph->p_flags & PF_W) vm_flags |= VM_WRITE;
        if (ph->p_flags & PF_X) vm_flags |= VM_EXEC;

        /* allocate and map pages */
        for (u32 p = 0; p < npages; p++) {
            uptr phys = pmm_alloc_zero();
            if (!phys) {
                mm_destroy(new_mm); kfree(phdrs); vfs_close(f);
                return -ENOMEM;
            }
            map_page(new_mm->pgdir, vaddr + p * PAGE_SIZE, phys, vm_flags);
        }

        /* read file data into mapped region */
        if (ph->p_filesz > 0) {
            /* copy page by page through kernel window */
            u32 offset_in_page = ph->p_vaddr - vaddr;
            size_t to_read = ph->p_filesz;
            uptr   dst_va  = ph->p_vaddr;
            off_t  src_off = ph->p_offset;

            while (to_read > 0) {
                uptr  pa   = virt_to_phys_pd(new_mm->pgdir, dst_va);
                uptr  kva  = (uptr)phys_to_virt(pa & PAGE_MASK) + (dst_va & (PAGE_SIZE-1));
                size_t chunk = MIN(to_read, PAGE_SIZE - (dst_va & (PAGE_SIZE-1)));
                pos = src_off;
                f->f_ops->read(f, (char*)kva, chunk, &pos);
                src_off += chunk;
                dst_va  += chunk;
                to_read -= chunk;
            }
            (void)offset_in_page;
        }

        if (!load_base || vaddr < load_base) load_base = vaddr;
        if (vend > load_end) load_end = vend;
    }

    /* update brk */
    new_mm->start_brk = ALIGN_UP(load_end, PAGE_SIZE);
    new_mm->brk       = new_mm->start_brk;

    /* ── 5. set up user stack ── */
    uptr stack_top = USER_STACK_TOP;
    /* allocate stack pages */
    for (uptr p = stack_top - USER_STACK_SIZE; p < stack_top; p += PAGE_SIZE) {
        uptr phys = pmm_alloc_zero();
        if (!phys) { mm_destroy(new_mm); kfree(phdrs); vfs_close(f); return -ENOMEM; }
        map_page(new_mm->pgdir, p, phys, VM_READ | VM_WRITE | VM_USER);
    }
    new_mm->start_stack = stack_top;

    /* ── 6. push argv, envp, auxv onto user stack ── */
    uptr sp = stack_top;

    int envc, argc;
    uptr envp_sp = push_strings(new_mm, sp, (char*const*)envp, &envc);
    if (!envp_sp) { mm_destroy(new_mm); kfree(phdrs); vfs_close(f); return -ENOMEM; }
    sp = envp_sp;

    uptr argv_sp = push_strings(new_mm, sp, (char*const*)argv, &argc);
    if (!argv_sp) { mm_destroy(new_mm); kfree(phdrs); vfs_close(f); return -ENOMEM; }
    sp = argv_sp;

    /* align to 16 bytes */
    sp = ALIGN_DOWN(sp, 16);

    /* aux vector (AT_NULL terminated) */
    u32 auxv[][2] = {
        { AT_PAGESZ, PAGE_SIZE },
        { AT_PHDR,   load_base + ehdr.e_phoff },
        { AT_PHENT,  ehdr.e_phentsize },
        { AT_PHNUM,  phnum },
        { AT_ENTRY,  ehdr.e_entry },
        { AT_UID,    current->uid },
        { AT_EUID,   current->euid },
        { AT_GID,    current->gid },
        { AT_EGID,   current->egid },
        { AT_NULL,   0 },
    };
    for (int i = ARRAY_SIZE(auxv) - 1; i >= 0; i--) {
        stack_push32(new_mm, &sp, auxv[i][1]);
        stack_push32(new_mm, &sp, auxv[i][0]);
    }

    /* push argc */
    stack_push32(new_mm, &sp, (u32)argc);

    /* ── 7. replace current process mm ── */
    /* close old mm */
    if (current->mm) mm_destroy(current->mm);
    current->mm = new_mm;

    /* update name */
    const char *basename = strrchr(path, '/');
    strncpy(current->name, basename ? basename + 1 : path, TASK_NAME_LEN - 1);

    /* close exec-on-close fds */
    if (current->files) {
        for (int i = 0; i < OPEN_MAX; i++) {
            if (current->files->fd[i] &&
                (current->files->close_on_exec[i/32] & (1u << (i%32)))) {
                files_free_fd(current->files, i);
            }
        }
    }

    kfree(phdrs);
    vfs_close(f);

    /* ── 8. jump to user entry point ── */
    paging_switch(new_mm->pgdir);
    tss_set_kernel_stack(current->kernel_stack);

    /* switch to user mode via iret */
    __asm__ volatile(
        "mov %0, %%esp\n\t"
        "push %1\n\t"       /* ss = SEG_UDATA */
        "push %2\n\t"       /* user esp */
        "push $0x200\n\t"   /* eflags: IF */
        "push %3\n\t"       /* cs = SEG_UCODE */
        "push %4\n\t"       /* eip = entry */
        "iret\n\t"
        :
        : "r"(current->kernel_stack),
          "i"(SEG_UDATA),
          "r"(sp),
          "i"(SEG_UCODE),
          "r"(ehdr.e_entry)
        : "memory");

    BUG(); /* never reached */
    return 0;
}
