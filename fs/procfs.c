/* fs/procfs.c – /proc filesystem */
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/mm.h>

/* /proc is a synthetic read-only filesystem backed by kernel data */

static struct super_block procfs_sb;

/* ─── procfs inode types ──────────────────────────────────────────────────── */
#define PROC_ROOT       1
#define PROC_MEMINFO    2
#define PROC_CPUINFO    3
#define PROC_UPTIME     4
#define PROC_VERSION    5
#define PROC_CMDLINE    6
#define PROC_STAT       7
#define PROC_MOUNTS     8
#define PROC_PID_DIR    100   /* per-pid entries start here */
#define PROC_PID_STATUS 101
#define PROC_PID_MAPS   102
#define PROC_PID_FD     103
#define PROC_PID_EXE    104
#define PROC_PID_CMDLINE 105

struct procfs_entry {
    const char *name;
    int         id;
    mode_t      mode;
    pid_t       pid;   /* nonzero for pid-specific */
};

/* ─── Generators (fill buffer with proc data) ────────────────────────────── */
static int proc_read_meminfo(char *buf, size_t sz) {
    u32 total = pmm_total_pages() * PAGE_SIZE / 1024;
    u32 free  = pmm_free_pages()  * PAGE_SIZE / 1024;
    return snprintf(buf, sz,
        "MemTotal:   %8u kB\n"
        "MemFree:    %8u kB\n"
        "MemUsed:    %8u kB\n"
        "Buffers:           0 kB\n"
        "Cached:            0 kB\n"
        "SwapTotal:         0 kB\n"
        "SwapFree:          0 kB\n",
        total, free, total - free);
}

static int proc_read_cpuinfo(char *buf, size_t sz) {
    u32 eax, ebx, ecx, edx;
    char vendor[13] = {};
    cpuid(0, &eax, &ebx, &ecx, &edx);
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);

    cpuid(1, &eax, &ebx, &ecx, &edx);
    int family  = (eax >> 8) & 0xF;
    int model   = (eax >> 4) & 0xF;
    int stepping = eax & 0xF;

    return snprintf(buf, sz,
        "processor\t: 0\n"
        "vendor_id\t: %s\n"
        "cpu family\t: %d\n"
        "model\t\t: %d\n"
        "stepping\t: %d\n"
        "cpu MHz\t\t: 0\n"
        "cache size\t: 0 KB\n"
        "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic\n"
        "bogomips\t: 0.00\n",
        vendor, family, model, stepping);
}

static int proc_read_uptime(char *buf, size_t sz) {
    extern u64 uptime_s;
    return snprintf(buf, sz, "%llu.00 %llu.00\n",
                    (unsigned long long)uptime_s,
                    (unsigned long long)uptime_s);
}

static int proc_read_version(char *buf, size_t sz) {
    return snprintf(buf, sz,
        "MyOS version 1.0.0 (root@myos) "
        "(gcc " __VERSION__ ") #1 SMP\n");
}

static int proc_read_stat(char *buf, size_t sz) {
    extern u64 jiffies;
    return snprintf(buf, sz,
        "cpu  0 0 0 %llu 0 0 0 0 0 0\n"
        "cpu0 0 0 0 %llu 0 0 0 0 0 0\n"
        "intr 0\n"
        "ctxt 0\n"
        "btime 0\n"
        "processes %u\n",
        (unsigned long long)jiffies,
        (unsigned long long)jiffies,
        next_pid - 1);
}

static int proc_read_pid_status(char *buf, size_t sz, pid_t pid) {
    struct task_struct *t = find_task_by_pid(pid);
    if (!t) return 0;
    const char *state_names[] = { "R (running)", "S (sleeping)",
                                    "T (stopped)", "Z (zombie)", "X (dead)" };
    return snprintf(buf, sz,
        "Name:\t%s\n"
        "Pid:\t%d\n"
        "PPid:\t%d\n"
        "Uid:\t%d\t%d\t%d\t%d\n"
        "Gid:\t%d\t%d\t%d\t%d\n"
        "State:\t%s\n"
        "VmSize:\t%u kB\n"
        "VmRSS:\t%u kB\n",
        t->name, t->pid, t->ppid,
        t->uid, t->euid, t->suid, t->uid,
        t->gid, t->egid, t->sgid, t->gid,
        (t->state < 5) ? state_names[t->state] : "?",
        t->mm ? (u32)(t->mm->brk / 1024) : 0,
        t->mm ? (u32)(pmm_free_pages() * 4) : 0);
}

/* ─── procfs inode / file ops ────────────────────────────────────────────── */
struct procfs_inode_priv {
    int    id;
    pid_t  pid;
    char  *cache;
    size_t cache_len;
    size_t cache_cap;
};

static ssize_t procfs_read(struct file *f, char *buf, size_t count, off_t *pos) {
    struct procfs_inode_priv *priv = f->f_dentry->d_inode->i_private;
    if (!priv) return -EIO;

    /* generate content on first read */
    if (!priv->cache) {
        priv->cache_cap = 4096;
        priv->cache = kmalloc(priv->cache_cap);
        if (!priv->cache) return -ENOMEM;
        int n = 0;
        switch (priv->id) {
        case PROC_MEMINFO:  n = proc_read_meminfo(priv->cache, priv->cache_cap); break;
        case PROC_CPUINFO:  n = proc_read_cpuinfo(priv->cache, priv->cache_cap); break;
        case PROC_UPTIME:   n = proc_read_uptime(priv->cache,  priv->cache_cap); break;
        case PROC_VERSION:  n = proc_read_version(priv->cache, priv->cache_cap); break;
        case PROC_STAT:     n = proc_read_stat(priv->cache,    priv->cache_cap); break;
        case PROC_PID_STATUS: n = proc_read_pid_status(priv->cache, priv->cache_cap, priv->pid); break;
        default: n = 0;
        }
        priv->cache_len = n > 0 ? n : 0;
    }

    if (*pos >= (off_t)priv->cache_len) return 0;
    size_t avail = priv->cache_len - *pos;
    size_t n = MIN(count, avail);
    memcpy(buf, priv->cache + *pos, n);
    *pos += n;
    return n;
}

static int procfs_open(struct inode *i, struct file *f) {
    (void)i; (void)f;
    /* invalidate cache so fresh data is generated each open */
    struct procfs_inode_priv *priv = i->i_private;
    if (priv && priv->cache) {
        kfree(priv->cache);
        priv->cache     = NULL;
        priv->cache_len = 0;
    }
    return 0;
}

static struct file_ops procfs_file_ops = {
    .open = procfs_open,
    .read = procfs_read,
};

static int procfs_readdir(struct file *f, struct dirent *de, int count) {
    (void)f; (void)de; (void)count;
    /* TODO: list /proc entries and per-pid dirs */
    return 0;
}

static struct file_ops procfs_dir_ops = {
    .readdir = procfs_readdir,
};

/* ─── Create a procfs inode ──────────────────────────────────────────────── */
static struct inode *proc_new_inode(int id, pid_t pid, mode_t mode) {
    struct inode *i = inode_alloc(&procfs_sb);
    if (!i) return NULL;
    i->i_mode = mode;
    i->i_uid  = i->i_gid = 0;
    i->i_nlink = 1;
    struct procfs_inode_priv *priv = kcalloc(1, sizeof(*priv));
    if (!priv) { inode_free(i); return NULL; }
    priv->id  = id;
    priv->pid = pid;
    i->i_private = priv;
    i->i_fops = S_ISDIR(mode) ? &procfs_dir_ops : &procfs_file_ops;
    return i;
}

/* ─── mount ───────────────────────────────────────────────────────────────── */
static struct super_ops procfs_sb_ops = {0};

static struct super_block *procfs_mount(struct filesystem_type *type,
                                         const char *dev, const char *opts) {
    (void)type; (void)dev; (void)opts;
    memset(&procfs_sb, 0, sizeof(procfs_sb));
    procfs_sb.s_blocksize = PAGE_SIZE;
    procfs_sb.s_magic     = 0x9FA0;
    procfs_sb.s_ops       = &procfs_sb_ops;
    procfs_sb.s_rdonly    = true;
    spin_init(&procfs_sb.s_lock);
    list_init(&procfs_sb.s_inodes);
    list_init(&procfs_sb.s_list);

    /* root dentry */
    struct inode *root_i = proc_new_inode(PROC_ROOT, 0, S_IFDIR | 0555);
    struct dentry *root  = dentry_alloc("proc", NULL);
    root->d_inode = inode_get(root_i);
    root->d_sb    = &procfs_sb;
    procfs_sb.s_root = root;

    /* standard proc entries */
    struct {
        const char *name; int id; mode_t mode;
    } entries[] = {
        { "meminfo",  PROC_MEMINFO,  S_IFREG|0444 },
        { "cpuinfo",  PROC_CPUINFO,  S_IFREG|0444 },
        { "uptime",   PROC_UPTIME,   S_IFREG|0444 },
        { "version",  PROC_VERSION,  S_IFREG|0444 },
        { "cmdline",  PROC_CMDLINE,  S_IFREG|0444 },
        { "stat",     PROC_STAT,     S_IFREG|0444 },
        { NULL, 0, 0 }
    };

    for (int i = 0; entries[i].name; i++) {
        struct dentry *de = dentry_alloc(entries[i].name, root);
        struct inode  *in = proc_new_inode(entries[i].id, 0, entries[i].mode);
        de->d_inode = inode_get(in);
        de->d_sb    = &procfs_sb;
    }

    return &procfs_sb;
}

static struct filesystem_type procfs_type = {
    .name  = "proc",
    .mount = procfs_mount,
};

void procfs_init(void) {
    register_filesystem(&procfs_type);
}

/* Call from kmain after vfs_init() */
void procfs_populate_pid(pid_t pid) {
    /* dynamically add /proc/<pid>/ entries */
    /* TODO: implement */
    (void)pid;
}
