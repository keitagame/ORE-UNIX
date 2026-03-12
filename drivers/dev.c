/* drivers/dev.c – device number registry + devfs support */
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>

/* ─── Device major/minor ──────────────────────────────────────────────────── */
#define MAJOR(dev)    ((dev) >> 8)
#define MINOR(dev)    ((dev) & 0xFF)
#define MKDEV(ma,mi)  (((ma) << 8) | ((mi) & 0xFF))

/* standard major numbers */
#define MAJ_MEM     1   /* /dev/null, /dev/zero, /dev/full, /dev/random */
#define MAJ_TTY     4   /* /dev/tty, /dev/console */
#define MAJ_ATA     8   /* /dev/hda etc */
#define MAJ_LOOP    7

#define MAX_CHRDEVS  256
#define MAX_BLKDEVS  256

struct chrdev {
    const char   *name;
    struct file_ops *ops;
};

struct blkdev {
    const char   *name;
    struct file_ops *ops;
    size_t        block_size;
    u64           total_blocks;
};

static struct chrdev chrdevs[MAX_CHRDEVS];
static struct blkdev blkdevs[MAX_BLKDEVS];

int register_chrdev(unsigned int major, const char *name, struct file_ops *ops) {
    if (major >= MAX_CHRDEVS) return -EINVAL;
    chrdevs[major].name = name;
    chrdevs[major].ops  = ops;
    printk(KERN_INFO "DEV: registered chrdev %u (%s)\n", major, name);
    return 0;
}

int register_blkdev(unsigned int major, const char *name, struct file_ops *ops) {
    if (major >= MAX_BLKDEVS) return -EINVAL;
    blkdevs[major].name = name;
    blkdevs[major].ops  = ops;
    printk(KERN_INFO "DEV: registered blkdev %u (%s)\n", major, name);
    return 0;
}

/* called by ramfs_mknod to set up file_ops on device inodes */
void dev_setup_inode(struct inode *i, dev_t dev) {
    unsigned int ma = MAJOR(dev);
    if (S_ISCHR(i->i_mode)) {
        if (ma < MAX_CHRDEVS && chrdevs[ma].ops)
            i->i_fops = chrdevs[ma].ops;
    } else if (S_ISBLK(i->i_mode)) {
        if (ma < MAX_BLKDEVS && blkdevs[ma].ops)
            i->i_fops = blkdevs[ma].ops;
    }
}

/* ─── /dev/null ───────────────────────────────────────────────────────────── */
static ssize_t null_read(struct file *f, char *buf, size_t c, off_t *p) {
    (void)f;(void)buf;(void)c;(void)p; return 0;
}
static ssize_t null_write(struct file *f, const char *buf, size_t c, off_t *p) {
    (void)f;(void)buf;(void)p; return c;
}
static struct file_ops null_ops = { .read = null_read, .write = null_write };

/* ─── /dev/zero ───────────────────────────────────────────────────────────── */
static ssize_t zero_read(struct file *f, char *buf, size_t c, off_t *p) {
    (void)f;(void)p; memset(buf,0,c); return c;
}
static struct file_ops zero_ops = { .read = zero_read, .write = null_write };

/* ─── /dev/full ───────────────────────────────────────────────────────────── */
static ssize_t full_write(struct file *f, const char *buf, size_t c, off_t *p) {
    (void)f;(void)buf;(void)c;(void)p; return -ENOSPC;
}
static struct file_ops full_ops = { .read = zero_read, .write = full_write };

/* ─── /dev/random & /dev/urandom ─────────────────────────────────────────── */
static u32 rand_state = 0xdeadbeef;
static u32 lcg_rand(void) {
    rand_state = rand_state * 1664525 + 1013904223;
    return rand_state;
}
static ssize_t random_read(struct file *f, char *buf, size_t c, off_t *p) {
    (void)f;(void)p;
    for (size_t i = 0; i < c; i += 4) {
        u32 r = lcg_rand();
        size_t left = MIN(4, c - i);
        memcpy(buf + i, &r, left);
    }
    return c;
}
static struct file_ops random_ops = { .read = random_read, .write = null_write };

/* ─── console / tty (serial-backed) ──────────────────────────────────────── */
/* forward decls from serial driver */
extern ssize_t serial_write_file(struct file *f, const char *buf, size_t c, off_t *p);
extern ssize_t serial_read_file(struct file *f, char *buf, size_t c, off_t *p);
extern int     serial_ioctl(struct file *f, u32 cmd, uptr arg);

static struct file_ops console_ops = {
    .read  = serial_read_file,
    .write = serial_write_file,
    .ioctl = serial_ioctl,
};

/* ─── /dev/tty (process controlling terminal) ────────────────────────────── */
static int tty_open(struct inode *i, struct file *f) { (void)i;(void)f; return 0; }
static struct file_ops tty_ops = {
    .open  = tty_open,
    .read  = serial_read_file,
    .write = serial_write_file,
    .ioctl = serial_ioctl,
};

/* ─── /dev/kmsg ───────────────────────────────────────────────────────────── */
static ssize_t kmsg_read(struct file *f, char *buf, size_t c, off_t *p) {
    (void)f;(void)buf;(void)c;(void)p; return 0;
}
static struct file_ops kmsg_ops = { .read = kmsg_read, .write = null_write };

/* ─── devfs: populate /dev ────────────────────────────────────────────────── */
void devfs_init(struct dentry *dev_dir) {
    /* Register chr major numbers */
    register_chrdev(MAJ_MEM, "mem", &null_ops);
    register_chrdev(MAJ_TTY, "tty", &tty_ops);

    extern void ata_register(void);
    ata_register();

    /* Create device files in /dev */
    struct {
        const char       *name;
        mode_t            mode;
        dev_t             dev;
        struct file_ops  *ops;
    } devs[] = {
        { "null",    S_IFCHR|0666, MKDEV(MAJ_MEM,3), &null_ops },
        { "zero",    S_IFCHR|0666, MKDEV(MAJ_MEM,5), &zero_ops },
        { "full",    S_IFCHR|0666, MKDEV(MAJ_MEM,7), &full_ops },
        { "random",  S_IFCHR|0444, MKDEV(MAJ_MEM,8), &random_ops },
        { "urandom", S_IFCHR|0444, MKDEV(MAJ_MEM,9), &random_ops },
        { "console", S_IFCHR|0600, MKDEV(MAJ_TTY,0), &console_ops },
        { "tty",     S_IFCHR|0666, MKDEV(MAJ_TTY,5), &tty_ops },
        { "tty0",    S_IFCHR|0600, MKDEV(MAJ_TTY,0), &tty_ops },
        { "ttyS0",   S_IFCHR|0600, MKDEV(MAJ_TTY,64),&tty_ops },
        { "kmsg",    S_IFCHR|0600, MKDEV(MAJ_MEM,11),&kmsg_ops },
        { NULL, 0, 0, NULL }
    };

    for (int i = 0; devs[i].name; i++) {
        extern struct inode_ops ramfs_inode_ops;
        struct dentry *de = dentry_alloc(devs[i].name, dev_dir);
        if (!de) continue;
        /* create inode via ramfs_mknod logic */
        struct inode *ino = inode_alloc(dev_dir->d_sb);
        if (!ino) { dentry_put(de); continue; }
        ino->i_mode  = devs[i].mode;
        ino->i_rdev  = devs[i].dev;
        ino->i_nlink = 1;
        ino->i_fops  = devs[i].ops;
        ino->i_ops   = &ramfs_inode_ops;
        extern struct ramfs_inode *ramfs_alloc_ri(void);
        void *ri = kcalloc(1, 32); /* minimal ramfs_inode */
        ino->i_private = ri;
        de->d_inode = inode_get(ino);
    }

    /* /dev/hda, /dev/hdb from ATA */
    extern void ata_create_devnodes(struct dentry *dev_dir);
    ata_create_devnodes(dev_dir);

    printk(KERN_INFO "DEVFS: populated /dev\n");
}
