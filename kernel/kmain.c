/* kernel/kmain.c – kernel entry point */
#include "kernel.h"
#include "mm.h"
#include "process.h"
#include "vfs.h"
#include "../include/arch.h"

/* ─── Multiboot2 structures ───────────────────────────────────────────────── */
#define MB2_BOOTLOADER_MAGIC 0x36D76289U

struct mb2_info {
    u32 total_size;
    u32 reserved;
};

struct mb2_tag_base {
    u32 type;
    u32 size;
};

/* tag types */
#define MB2_TAG_END         0
#define MB2_TAG_CMDLINE     1
#define MB2_TAG_MODULE      3
#define MB2_TAG_BASIC_MEM   4
#define MB2_TAG_MMAP        6
#define MB2_TAG_FRAMEBUF    8
#define MB2_TAG_EFI32       11

struct mb2_tag_basic_mem {
    u32 type, size;
    u32 mem_lower, mem_upper;  /* KB */
};

struct mb2_tag_module {
    u32 type, size;
    u32 mod_start, mod_end;
    char cmdline[];
};

struct mb2_tag_cmdline {
    u32 type, size;
    char string[];
};

/* ─── initrd globals ─────────────────────────────────────────────────────── */
static uptr  initrd_phys  = 0;
static u32   initrd_size  = 0;
static char  kernel_cmdline[256] = "";

/* ─── initrd CPIO parser ─────────────────────────────────────────────────── */
/* newc format (no CRC) */
struct cpio_newc_hdr {
    char magic[6];    /* "070701" */
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
};

static u32 cpio_parse_hex(const char *s, int n) {
    u32 v = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        int d = (c>='0'&&c<='9') ? c-'0' :
                (c>='a'&&c<='f') ? c-'a'+10 : c-'A'+10;
        v = v * 16 + d;
    }
    return v;
}

#define CPIO_ALIGN4(x)  (((x) + 3) & ~3)

static void initrd_populate(struct dentry *root) {
    if (!initrd_phys || !initrd_size) return;

    u8 *data = (u8*)phys_to_virt(initrd_phys);
    u8 *end  = data + initrd_size;
    u8 *p    = data;

    printk(KERN_INFO "INITRD: parsing %u bytes at %p\n", initrd_size, data);

    while (p + sizeof(struct cpio_newc_hdr) <= end) {
        struct cpio_newc_hdr *hdr = (struct cpio_newc_hdr*)p;
        if (memcmp(hdr->magic, "070701", 6) != 0 &&
            memcmp(hdr->magic, "070702", 6) != 0) break;

        u32 namesize = cpio_parse_hex(hdr->namesize, 8);
        u32 filesize = cpio_parse_hex(hdr->filesize, 8);
        u32 mode     = cpio_parse_hex(hdr->mode, 8);
        u32 uid      = cpio_parse_hex(hdr->uid, 8);
        u32 gid      = cpio_parse_hex(hdr->gid, 8);

        char *name = (char*)(p + sizeof(struct cpio_newc_hdr));
        u8 *file_data = p + CPIO_ALIGN4(sizeof(struct cpio_newc_hdr) + namesize);

        if (strcmp(name, "TRAILER!!!") == 0) break;
        if (strcmp(name, ".") == 0) goto next;
        if (namesize == 0) goto next;

        /* skip leading "./" */
        if (name[0] == '.' && name[1] == '/') name += 2;

        /* split into dir + basename */
        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "/%s", name);

        /* find/create parent directories */
        struct dentry *parent = root;
        char *slash = fullpath + 1;  /* skip leading / */
        char *next_slash;
        while ((next_slash = strchr(slash, '/')) != NULL) {
            *next_slash = '\0';
            struct dentry *child = NULL;
            struct list_head *pos;
            list_for_each(pos, &parent->d_children) {
                struct dentry *de = list_entry(pos, struct dentry, d_sibling);
                if (strcmp(de->d_name, slash) == 0) { child = de; break; }
            }
            if (!child) {
                extern int ramfs_create_dir(struct dentry*, const char*, mode_t);
                ramfs_create_dir(parent, slash, 0755);
                list_for_each(pos, &parent->d_children) {
                    struct dentry *de = list_entry(pos, struct dentry, d_sibling);
                    if (strcmp(de->d_name, slash) == 0) { child = de; break; }
                }
            }
            *next_slash = '/';
            if (!child) break;
            parent = child;
            slash  = next_slash + 1;
        }

        /* leaf name is in 'slash' now */
        if (!*slash) goto next;

        if (S_ISDIR(mode)) {
            extern int ramfs_create_dir(struct dentry*, const char*, mode_t);
            ramfs_create_dir(parent, slash, mode & 07777);
        } else if (S_ISLNK(mode)) {
            /* symlink target is the file data */
            char target[256];
            u32 tlen = MIN(filesize, sizeof(target)-1);
            memcpy(target, file_data, tlen);
            target[tlen] = '\0';
            vfs_symlink(target, fullpath);
        } else if (S_ISREG(mode)) {
            extern int ramfs_create_file(struct dentry*, const char*,
                                          const void*, size_t, mode_t);
            ramfs_create_file(parent, slash, file_data, filesize, mode & 07777);
        }
        /* set ownership */
        (void)uid; (void)gid;

    next:
        u32 hdr_and_name = CPIO_ALIGN4(sizeof(struct cpio_newc_hdr) + namesize);
        u32 data_pad     = CPIO_ALIGN4(filesize);
        p += hdr_and_name + data_pad;
    }

    printk(KERN_INFO "INITRD: population complete\n");
}

/* ─── External init functions ─────────────────────────────────────────────── */
extern void gdt_init(void);
extern void idt_init(void);
extern void paging_init(void);
extern void kmalloc_init(void);
extern void mm_init_caches(void);
extern void sched_init(void);
extern void vfs_init(void);
extern void ramfs_init(void);
extern void serial_init(void);
extern void timer_init(u32 hz);
extern void devfs_init(struct dentry *dev_dir);
extern void ata_register(void);

/* ─── init process ────────────────────────────────────────────────────────── */
static void run_init(void) {
    printk(KERN_INFO "INIT: starting /sbin/init\n");

    /* set up stdio fds for init */
    int fd = sys_open("/dev/console", O_RDWR, 0);
    if (fd != 0) {
        sys_dup2(fd, 0);
        sys_dup2(fd, 1);
        sys_dup2(fd, 2);
        if (fd > 2) sys_close(fd);
    } else {
        sys_dup2(0, 1);
        sys_dup2(0, 2);
    }

    /* try /sbin/init, /init, /bin/sh in that order */
    const char *init_paths[] = { "/sbin/init", "/init", "/bin/sh", NULL };
    char *init_argv[] = { NULL, NULL };
    char *init_envp[] = {
        "HOME=/root",
        "PATH=/bin:/sbin:/usr/bin:/usr/sbin",
        "TERM=vt100",
        "SHELL=/bin/sh",
        NULL
    };

    /* check cmdline for init= */
    const char *init_override = NULL;
    const char *p = strstr(kernel_cmdline, "init=");
    if (p) {
        static char init_path[128];
        p += 5;
        int i = 0;
        while (*p && *p != ' ' && i < 127) init_path[i++] = *p++;
        init_path[i] = '\0';
        init_override = init_path;
    }

    if (init_override) {
        init_argv[0] = (char*)init_override;
        sys_exec(init_override, init_argv, init_envp);
    }

    for (int i = 0; init_paths[i]; i++) {
        init_argv[0] = (char*)init_paths[i];
        sys_exec(init_paths[i], init_argv, init_envp);
    }

    panic("init: no init binary found! (tried /sbin/init, /init, /bin/sh)\n"
          "Please provide an initrd with a valid init binary.\n");
}

/* ─── kmain ───────────────────────────────────────────────────────────────── */
void kmain(u32 mb_magic, u32 mb_info_phys) {
    /* ── 1. early serial output ── */
    /* serial_init requires IRQ table from idt_init, so do minimal UART here */
    extern void uart_init_early(void);
    /* direct port init */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);   /* 115200 baud divisor lo */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);

    kprintf("\n");
    kprintf("╔══════════════════════════════════════╗\n");
    kprintf("║         MyOS Kernel v1.0             ║\n");
    kprintf("║    Unix-compatible, extensible OS    ║\n");
    kprintf("╚══════════════════════════════════════╝\n");
    kprintf("\n");

    /* ── 2. validate multiboot ── */
    if (mb_magic != MB2_BOOTLOADER_MAGIC)
        panic("Invalid multiboot2 magic: %08x\n", mb_magic);

    /* ── 3. parse multiboot2 tags ── */
    u32 mem_lower = 640, mem_upper = 32*1024;  /* defaults */
    uptr mmap_addr = 0; u32 mmap_len = 0;

    struct mb2_info *mbi = (struct mb2_info*)phys_to_virt(mb_info_phys);
    u8 *tag_ptr = (u8*)mbi + 8;  /* tags start after 8-byte header */
    u8 *mbi_end = (u8*)mbi + mbi->total_size;

    while (tag_ptr < mbi_end) {
        struct mb2_tag_base *tag = (struct mb2_tag_base*)tag_ptr;
        if (tag->type == MB2_TAG_END) break;

        switch (tag->type) {
        case MB2_TAG_CMDLINE: {
            struct mb2_tag_cmdline *ct = (struct mb2_tag_cmdline*)tag;
            strncpy(kernel_cmdline, ct->string, sizeof(kernel_cmdline)-1);
            kprintf(KERN_INFO "CMDLINE: %s\n", kernel_cmdline);
            break;
        }
        case MB2_TAG_BASIC_MEM: {
            struct mb2_tag_basic_mem *m = (struct mb2_tag_basic_mem*)tag;
            mem_lower = m->mem_lower;
            mem_upper = m->mem_upper;
            kprintf(KERN_INFO "MEM: lower=%u KB upper=%u KB\n", mem_lower, mem_upper);
            break;
        }
        case MB2_TAG_MMAP:
            mmap_addr = (uptr)tag_ptr - KERNEL_VIRT_BASE;
            mmap_len  = tag->size;
            break;
        case MB2_TAG_MODULE: {
            struct mb2_tag_module *mod = (struct mb2_tag_module*)tag;
            kprintf(KERN_INFO "MODULE: [%08x-%08x] '%s'\n",
                    mod->mod_start, mod->mod_end, mod->cmdline);
            if (!initrd_phys) {
                initrd_phys = mod->mod_start;
                initrd_size = mod->mod_end - mod->mod_start;
                /* protect initrd from PMM */
            }
            break;
        }
        }
        /* next tag aligned to 8 bytes */
        tag_ptr += ALIGN_UP(tag->size, 8);
    }

    /* ── 4. GDT / IDT ── */
    kprintf(KERN_INFO "Initializing GDT...\n");
    gdt_init();
    kprintf(KERN_INFO "Initializing IDT...\n");
    idt_init();

    /* ── 5. Physical memory manager ── */
    kprintf(KERN_INFO "Initializing PMM...\n");
    pmm_init(mem_lower + mem_upper, mmap_addr, mmap_len);

    /* protect initrd */
    if (initrd_phys)
        pmm_mark_used(initrd_phys,
                      ALIGN_UP(initrd_phys + initrd_size, PAGE_SIZE));

    /* ── 6. Virtual memory / paging ── */
    kprintf(KERN_INFO "Initializing paging...\n");
    paging_init();

    /* ── 7. Kernel heap ── */
    kprintf(KERN_INFO "Initializing kmalloc...\n");
    kmalloc_init();
    mm_init_caches();

    /* ── 8. Scheduler / process management ── */
    kprintf(KERN_INFO "Initializing scheduler...\n");
    sched_init();

    /* ── 9. Serial (full, with IRQ) ── */
    serial_init();

    /* ── 10. Timer ── */
    timer_init(100);  /* 100 Hz */

    /* ── 11. VFS ── */
    kprintf(KERN_INFO "Initializing VFS...\n");
    vfs_init();
    ramfs_init();

    /* mount rootfs as ramfs */
    vfs_mount(NULL, "/", "ramfs", NULL);

    /* create standard directory structure */
    vfs_mkdir("/bin",  0755);
    vfs_mkdir("/sbin", 0755);
    vfs_mkdir("/etc",  0755);
    vfs_mkdir("/dev",  0755);
    vfs_mkdir("/proc", 0755);
    vfs_mkdir("/sys",  0755);
    vfs_mkdir("/tmp",  01777);
    vfs_mkdir("/var",  0755);
    vfs_mkdir("/var/log", 0755);
    vfs_mkdir("/var/run", 0755);
    vfs_mkdir("/home", 0755);
    vfs_mkdir("/root", 0700);
    vfs_mkdir("/usr",  0755);
    vfs_mkdir("/usr/bin",  0755);
    vfs_mkdir("/usr/lib",  0755);
    vfs_mkdir("/usr/share",0755);
    vfs_mkdir("/lib",  0755);
    vfs_mkdir("/mnt",  0755);
    vfs_mkdir("/opt",  0755);

    /* ── 12. Device files ── */
    kprintf(KERN_INFO "Initializing devices...\n");
    struct dentry *dev_dir;
    vfs_lookup("/dev", &dev_dir, NULL);
    devfs_init(dev_dir);
    dentry_put(dev_dir);

    /* ── 13. Populate from initrd ── */
    initrd_populate(vfs_root);

    /* write /etc/hostname if not present */
    {
        struct file *f = vfs_open("/etc/hostname", O_WRONLY|O_CREAT|O_TRUNC, 0644);
        if (f) {
            vfs_write(f, "myos\n", 5);
            vfs_close(f);
        }
    }

    /* write /etc/passwd if not present */
    {
        struct file *f = vfs_open("/etc/passwd",
                                   O_WRONLY|O_CREAT, 0644);
        if (f) {
            const char *passwd =
                "root:x:0:0:root:/root:/bin/sh\n"
                "nobody:x:65534:65534:nobody:/:/bin/false\n";
            vfs_write(f, passwd, strlen(passwd));
            vfs_close(f);
        }
    }

    /* ── 14. Enable interrupts ── */
    kprintf(KERN_INFO "Enabling interrupts...\n");
    sti();

    /* ── 15. Create init process ── */
    kprintf(KERN_INFO "Starting init process...\n");
    kprintf(KERN_INFO "System RAM: %u MB\n",
            (pmm_total_pages() * PAGE_SIZE) / (1024*1024));
    kprintf(KERN_INFO "Free RAM:   %u MB\n",
            (pmm_free_pages()  * PAGE_SIZE) / (1024*1024));

    /* create PID 1 task */
    struct task_struct *init_task = task_create("init", run_init, 0);
    if (!init_task) panic("Failed to create init task\n");
    init_task->pid = 1;
    init_task->uid = init_task->euid = 0;
    init_task->gid = init_task->egid = 0;

    /* set cwd and root for init */
    vfs_lookup("/", &init_task->root, NULL);
    vfs_lookup("/", &init_task->cwd,  NULL);

    sched_add(init_task);

    kprintf(KERN_INFO "Kernel boot complete. Switching to init...\n\n");

    /* enter scheduler - this never returns */
    schedule();

    /* idle loop */
    while (1) {
        sti();
        hlt();
    }
}
