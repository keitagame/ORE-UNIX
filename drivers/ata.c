/* drivers/ata.c – ATA PIO driver for /dev/hda, /dev/hdb */
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <arch/x86/arch.h>

/* ─── ATA registers ───────────────────────────────────────────────────────── */
#define ATA_PRIMARY   0x1F0
#define ATA_SECONDARY 0x170

#define ATA_DATA      0x00
#define ATA_ERROR     0x01
#define ATA_FEATURES  0x01
#define ATA_SECCOUNT  0x02
#define ATA_LBA_LO    0x03
#define ATA_LBA_MID   0x04
#define ATA_LBA_HI    0x05
#define ATA_DRIVE     0x06
#define ATA_STATUS    0x07
#define ATA_CMD       0x07

#define ATA_CTRL      0x3F6   /* primary control / alt status */

#define ATA_SR_BSY    0x80
#define ATA_SR_DRDY   0x40
#define ATA_SR_DF     0x20
#define ATA_SR_DSC    0x10
#define ATA_SR_DRQ    0x08
#define ATA_SR_CORR   0x04
#define ATA_SR_IDX    0x02
#define ATA_SR_ERR    0x01

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_WRITE_PIO  0x30
#define ATA_CMD_IDENTIFY   0xEC
#define ATA_CMD_FLUSH      0xE7

#define ATA_DRIVE_MASTER 0xA0
#define ATA_DRIVE_SLAVE  0xB0
#define ATA_DRIVE_LBA    0x40

/* ─── Drive descriptor ────────────────────────────────────────────────────── */
struct ata_drive {
    bool     exists;
    bool     lba48;
    u16      base;
    u8       drive;   /* 0=master, 1=slave */
    u64      sectors;
    char     model[41];
    char     serial[21];
};

static struct ata_drive drives[4];  /* 0=pri/master, 1=pri/slave, 2=sec/master, 3=sec/slave */
static spinlock_t ata_lock = SPINLOCK_INIT;

/* ─── Helpers ─────────────────────────────────────────────────────────────── */
static void ata_wait_bsy(u16 base) {
    while (inb(base + ATA_STATUS) & ATA_SR_BSY) cpu_relax();
}

static int ata_wait_drq(u16 base) {
    u32 timeout = 1000000;
    while (--timeout) {
        u8 s = inb(base + ATA_STATUS);
        if (s & ATA_SR_ERR) return -EIO;
        if (s & ATA_SR_DRQ) return 0;
        cpu_relax();
    }
    return -EIO;
}

static int ata_wait_ready(u16 base) {
    ata_wait_bsy(base);
    u32 timeout = 1000000;
    while (--timeout) {
        u8 s = inb(base + ATA_STATUS);
        if (s & ATA_SR_ERR) return -EIO;
        if (s & ATA_SR_DRDY) return 0;
        cpu_relax();
    }
    return -ETIMEDOUT;
}
#define ETIMEDOUT 110

/* ─── Identify ────────────────────────────────────────────────────────────── */
static bool ata_identify(struct ata_drive *d) {
    u16 base  = d->base;
    u8  drive = d->drive ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER;

    outb(base + ATA_DRIVE, drive);
    outb(base + ATA_SECCOUNT, 0);
    outb(base + ATA_LBA_LO,  0);
    outb(base + ATA_LBA_MID, 0);
    outb(base + ATA_LBA_HI,  0);
    outb(base + ATA_CMD, ATA_CMD_IDENTIFY);

    u8 status = inb(base + ATA_STATUS);
    if (!status) return false;   /* no drive */

    ata_wait_bsy(base);
    /* check mid/hi bytes: non-zero = ATAPI */
    if (inb(base + ATA_LBA_MID) || inb(base + ATA_LBA_HI)) return false;

    if (ata_wait_drq(base)) return false;

    u16 ident[256];
    for (int i = 0; i < 256; i++) ident[i] = inw(base + ATA_DATA);

    /* model string at words 27..46 */
    for (int i = 0; i < 20; i++) {
        d->model[i*2]   = (ident[27+i] >> 8) & 0xFF;
        d->model[i*2+1] = ident[27+i] & 0xFF;
    }
    d->model[40] = '\0';

    /* LBA28 sector count */
    d->sectors = ((u32)ident[61] << 16) | ident[60];

    /* LBA48 */
    if (ident[83] & (1 << 10)) {
        d->lba48  = true;
        d->sectors = ((u64)ident[103] << 48) | ((u64)ident[102] << 32) |
                     ((u64)ident[101] << 16) | ident[100];
    }

    d->exists = true;
    printk(KERN_INFO "ATA: drive %s [%.40s] %llu MB\n",
           d->drive ? "slave" : "master",
           d->model,
           (d->sectors * 512) / (1024*1024));
    return true;
}

/* ─── LBA28 PIO read/write ────────────────────────────────────────────────── */
static int ata_read_sector(struct ata_drive *d, u64 lba, void *buf) {
    u16 base = d->base;
    u8  drv  = (d->drive ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER) | ATA_DRIVE_LBA;

    ata_wait_bsy(base);
    outb(base + ATA_DRIVE,   drv | ((lba >> 24) & 0x0F));
    outb(base + ATA_SECCOUNT, 1);
    outb(base + ATA_LBA_LO,  (lba)       & 0xFF);
    outb(base + ATA_LBA_MID, (lba >>  8) & 0xFF);
    outb(base + ATA_LBA_HI,  (lba >> 16) & 0xFF);
    outb(base + ATA_CMD,     ATA_CMD_READ_PIO);

    int r = ata_wait_drq(base);
    if (r) return r;

    u16 *p = (u16*)buf;
    for (int i = 0; i < 256; i++) p[i] = inw(base + ATA_DATA);
    return 0;
}

static int ata_write_sector(struct ata_drive *d, u64 lba, const void *buf) {
    u16 base = d->base;
    u8  drv  = (d->drive ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER) | ATA_DRIVE_LBA;

    ata_wait_bsy(base);
    outb(base + ATA_DRIVE,   drv | ((lba >> 24) & 0x0F));
    outb(base + ATA_SECCOUNT, 1);
    outb(base + ATA_LBA_LO,  (lba)       & 0xFF);
    outb(base + ATA_LBA_MID, (lba >>  8) & 0xFF);
    outb(base + ATA_LBA_HI,  (lba >> 16) & 0xFF);
    outb(base + ATA_CMD,     ATA_CMD_WRITE_PIO);

    int r = ata_wait_drq(base);
    if (r) return r;

    const u16 *p = (const u16*)buf;
    for (int i = 0; i < 256; i++) outw(base + ATA_DATA, p[i]);
    outb(base + ATA_CMD, ATA_CMD_FLUSH);
    ata_wait_bsy(base);
    return 0;
}

/* ─── File operations ─────────────────────────────────────────────────────── */
struct ata_file_priv { struct ata_drive *drive; };

static ssize_t ata_file_read(struct file *f, char *buf, size_t count, off_t *pos) {
    struct ata_drive *d = f->f_private;
    if (!d) return -ENODEV;

    size_t n = 0;
    while (n < count) {
        u64 lba  = (*pos + n) / 512;
        u32 off  = (*pos + n) % 512;
        size_t chunk = MIN(512 - off, count - n);

        char sector[512];
        irq_flags_t fl;
        spin_lock_irqsave(&ata_lock, &fl);
        int r = ata_read_sector(d, lba, sector);
        spin_unlock_irqrestore(&ata_lock, &fl);
        if (r) return n > 0 ? (ssize_t)n : r;

        memcpy(buf + n, sector + off, chunk);
        n += chunk;
    }
    *pos += n;
    return n;
}

static ssize_t ata_file_write(struct file *f, const char *buf, size_t count, off_t *pos) {
    struct ata_drive *d = f->f_private;
    if (!d) return -ENODEV;

    size_t n = 0;
    while (n < count) {
        u64 lba  = (*pos + n) / 512;
        u32 off  = (*pos + n) % 512;
        size_t chunk = MIN(512 - off, count - n);

        char sector[512];
        if (off || chunk < 512) {
            /* read-modify-write */
            irq_flags_t fl;
            spin_lock_irqsave(&ata_lock, &fl);
            ata_read_sector(d, lba, sector);
            spin_unlock_irqrestore(&ata_lock, &fl);
        }
        memcpy(sector + off, buf + n, chunk);
        irq_flags_t fl;
        spin_lock_irqsave(&ata_lock, &fl);
        int r = ata_write_sector(d, lba, sector);
        spin_unlock_irqrestore(&ata_lock, &fl);
        if (r) return n > 0 ? (ssize_t)n : r;
        n += chunk;
    }
    *pos += n;
    return n;
}

static int ata_file_open(struct inode *i, struct file *f) {
    f->f_private = i->i_private;
    return 0;
}

static struct file_ops ata_ops = {
    .open  = ata_file_open,
    .read  = ata_file_read,
    .write = ata_file_write,
};

/* ─── Init & devnode creation ─────────────────────────────────────────────── */
void ata_register(void) {
    /* probe 4 possible drives */
    u16 bases[2]  = { ATA_PRIMARY, ATA_SECONDARY };
    u8  drvs[2]   = { 0, 1 };

    int idx = 0;
    for (int b = 0; b < 2; b++) {
        for (int d = 0; d < 2; d++, idx++) {
            drives[idx].base  = bases[b];
            drives[idx].drive = drvs[d];
            ata_identify(&drives[idx]);
        }
    }

    extern int register_blkdev(unsigned int, const char *, struct file_ops *);
    register_blkdev(8, "ata", &ata_ops);
}

void ata_create_devnodes(struct dentry *dev_dir) {
    char names[][8] = { "hda", "hdb", "hdc", "hdd" };
    for (int i = 0; i < 4; i++) {
        if (!drives[i].exists) continue;
        extern struct inode_ops ramfs_inode_ops;
        struct dentry *de = dentry_alloc(names[i], dev_dir);
        if (!de) continue;
        struct inode *ino = inode_alloc(dev_dir->d_sb);
        if (!ino) { dentry_put(de); continue; }
        ino->i_mode    = S_IFBLK | 0660;
        ino->i_rdev    = (8 << 8) | (i * 16);
        ino->i_size    = drives[i].sectors * 512;
        ino->i_fops    = &ata_ops;
        ino->i_ops     = &ramfs_inode_ops;
        ino->i_private = &drives[i];
        de->d_inode    = inode_get(ino);
    }
}
