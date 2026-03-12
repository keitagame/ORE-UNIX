/* fs/ramfs.c – in-memory filesystem (ramfs / tmpfs / initrd) */
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>

/* ─── ramfs inode private data ───────────────────────────────────────────── */
struct ramfs_inode {
    char    *data;      /* for regular files */
    size_t   capacity;
    ino_t    ino;
    char    *symlink_target;  /* for symlinks */
};

static ino_t next_ino = 2;

/* ─── Super block ─────────────────────────────────────────────────────────── */
static struct super_block ramfs_sb;

/* ─── inode operations ────────────────────────────────────────────────────── */
static int ramfs_lookup(struct inode *dir, struct dentry *de) {
    /* dentry list already managed by VFS; just confirm inode is valid */
    /* In ramfs the dentries ARE the data, so if it's in the dentry tree, it exists */
    (void)dir;
    if (de->d_inode) return 0;
    return -ENOENT;
}

static struct inode *ramfs_new_inode(mode_t mode) {
    struct inode *i = inode_alloc(&ramfs_sb);
    if (!i) return NULL;
    struct ramfs_inode *ri = kcalloc(1, sizeof(*ri));
    if (!ri) { inode_free(i); return NULL; }
    ri->ino  = next_ino++;
    i->i_ino = ri->ino;
    i->i_mode = mode;
    i->i_uid = current ? current->uid : 0;
    i->i_gid = current ? current->gid : 0;
    i->i_nlink = 1;
    i->i_private = ri;
    extern u32 jiffies;
    i->i_atime = i->i_mtime = i->i_ctime = jiffies;
    return i;
}

static int ramfs_create(struct inode *dir, struct dentry *de, mode_t mode) {
    (void)dir;
    struct inode *i = ramfs_new_inode(S_IFREG | (mode & 0777));
    if (!i) return -ENOMEM;
    extern struct inode_ops ramfs_inode_ops;
    extern struct file_ops  ramfs_file_ops;
    i->i_ops  = &ramfs_inode_ops;
    i->i_fops = &ramfs_file_ops;
    de->d_inode = inode_get(i);
    dir->i_mtime = i->i_ctime;
    return 0;
}

static int ramfs_mkdir(struct inode *dir, struct dentry *de, mode_t mode) {
    (void)dir;
    struct inode *i = ramfs_new_inode(S_IFDIR | (mode & 0777));
    if (!i) return -ENOMEM;
    extern struct inode_ops ramfs_inode_ops;
    extern struct file_ops  ramfs_dir_ops;
    i->i_ops  = &ramfs_inode_ops;
    i->i_fops = &ramfs_dir_ops;
    de->d_inode = inode_get(i);
    de->d_sb    = &ramfs_sb;
    dir->i_nlink++;
    return 0;
}

static int ramfs_unlink(struct inode *dir, struct dentry *de) {
    (void)dir;
    if (!de->d_inode) return -ENOENT;
    de->d_inode->i_nlink--;
    if (de->d_inode->i_nlink == 0) {
        struct ramfs_inode *ri = de->d_inode->i_private;
        if (ri) { kfree(ri->data); kfree(ri->symlink_target); kfree(ri); }
        inode_put(de->d_inode);
        de->d_inode = NULL;
    }
    list_del(&de->d_sibling);
    return 0;
}

static int ramfs_rmdir(struct inode *dir, struct dentry *de) {
    if (!list_empty(&de->d_children)) return -ENOTEMPTY;
    dir->i_nlink--;
    return ramfs_unlink(dir, de);
}

static int ramfs_rename(struct inode *od, struct dentry *ode,
                         struct inode *nd, struct dentry *nde) {
    (void)od; (void)nd;
    /* move inode to new dentry */
    nde->d_inode = ode->d_inode; ode->d_inode = NULL;
    list_del(&ode->d_sibling);
    return 0;
}

static int ramfs_symlink(struct inode *dir, struct dentry *de, const char *target) {
    (void)dir;
    struct inode *i = ramfs_new_inode(S_IFLNK | 0777);
    if (!i) return -ENOMEM;
    struct ramfs_inode *ri = i->i_private;
    ri->symlink_target = strdup(target);
    i->i_size = strlen(target);
    extern struct inode_ops ramfs_inode_ops;
    i->i_ops = &ramfs_inode_ops;
    de->d_inode = inode_get(i);
    return 0;
}

static int ramfs_readlink(struct inode *i, char *buf, size_t sz) {
    struct ramfs_inode *ri = i->i_private;
    if (!ri->symlink_target) return -EINVAL;
    size_t len = strlen(ri->symlink_target);
    if (len >= sz) len = sz - 1;
    memcpy(buf, ri->symlink_target, len);
    buf[len] = '\0';
    return (int)len;
}

static int ramfs_truncate(struct inode *i, off_t size) {
    struct ramfs_inode *ri = i->i_private;
    if (size == 0) { kfree(ri->data); ri->data = NULL; ri->capacity = 0; }
    else if ((size_t)size > ri->capacity) {
        char *nd = krealloc(ri->data, size);
        if (!nd) return -ENOMEM;
        if ((size_t)size > ri->capacity)
            memset(nd + ri->capacity, 0, size - ri->capacity);
        ri->data = nd; ri->capacity = size;
    }
    i->i_size = size;
    return 0;
}

static int ramfs_chmod(struct inode *i, mode_t mode) {
    i->i_mode = (i->i_mode & S_IFMT) | (mode & 07777);
    return 0;
}
static int ramfs_chown(struct inode *i, uid_t u, gid_t g) {
    i->i_uid = u; i->i_gid = g;
    return 0;
}
static int ramfs_mknod(struct inode *dir, struct dentry *de, mode_t mode, dev_t dev) {
    (void)dir;
    struct inode *i = ramfs_new_inode(mode);
    if (!i) return -ENOMEM;
    i->i_rdev = dev;
    /* register character/block device ops */
    extern void dev_setup_inode(struct inode *i, dev_t dev);
    dev_setup_inode(i, dev);
    de->d_inode = inode_get(i);
    return 0;
}

struct inode_ops ramfs_inode_ops = {
    .lookup   = ramfs_lookup,
    .create   = ramfs_create,
    .mkdir    = ramfs_mkdir,
    .rmdir    = ramfs_rmdir,
    .unlink   = ramfs_unlink,
    .rename   = ramfs_rename,
    .symlink  = ramfs_symlink,
    .readlink = ramfs_readlink,
    .truncate = ramfs_truncate,
    .chmod    = ramfs_chmod,
    .chown    = ramfs_chown,
    .mknod    = ramfs_mknod,
};

/* ─── file operations (regular files) ────────────────────────────────────── */
static int ramfs_open(struct inode *i, struct file *f) { (void)i; (void)f; return 0; }
static int ramfs_release(struct inode *i, struct file *f) { (void)i; (void)f; return 0; }

static ssize_t ramfs_read(struct file *f, char *buf, size_t count, off_t *pos) {
    struct inode *i = f->f_dentry->d_inode;
    struct ramfs_inode *ri = i->i_private;
    if (*pos >= i->i_size) return 0;
    if (*pos + (off_t)count > i->i_size) count = i->i_size - *pos;
    if (!ri->data) return 0;
    memcpy(buf, ri->data + *pos, count);
    *pos += count;
    return count;
}

static ssize_t ramfs_write(struct file *f, const char *buf, size_t count, off_t *pos) {
    struct inode *i = f->f_dentry->d_inode;
    struct ramfs_inode *ri = i->i_private;
    if (f->f_flags & O_APPEND) *pos = i->i_size;
    off_t end = *pos + (off_t)count;
    if ((size_t)end > ri->capacity) {
        size_t newcap = MAX(end, ri->capacity * 2 + 4096);
        char *nd = krealloc(ri->data, newcap);
        if (!nd) return -ENOMEM;
        if (newcap > ri->capacity) memset(nd + ri->capacity, 0, newcap - ri->capacity);
        ri->data = nd; ri->capacity = newcap;
    }
    memcpy(ri->data + *pos, buf, count);
    *pos += count;
    if (*pos > i->i_size) i->i_size = *pos;
    return count;
}

struct file_ops ramfs_file_ops = {
    .open    = ramfs_open,
    .release = ramfs_release,
    .read    = ramfs_read,
    .write   = ramfs_write,
};

/* ─── directory file operations ────────────────────────────────────────────── */
static ssize_t ramfs_dir_read(struct file *f, char *buf, size_t count, off_t *pos) {
    (void)f; (void)buf; (void)count; (void)pos;
    return -EISDIR;
}

static int ramfs_readdir(struct file *f, struct dirent *de, int count) {
    struct dentry *dir = f->f_dentry;
    int idx = 0; int filled = 0;
    struct list_head *pos;
    list_for_each(pos, &dir->d_children) {
        if (idx++ < (int)f->f_pos) continue;
        if (filled >= count) break;
        struct dentry *child = list_entry(pos, struct dentry, d_sibling);
        de[filled].d_ino = child->d_inode ? child->d_inode->i_ino : 0;
        de[filled].d_off = f->f_pos;
        strncpy(de[filled].d_name, child->d_name, NAME_MAX);
        de[filled].d_reclen = sizeof(struct dirent);
        if (child->d_inode) {
            mode_t m = child->d_inode->i_mode;
            de[filled].d_type = S_ISDIR(m) ? DT_DIR :
                                 S_ISLNK(m) ? DT_LNK :
                                 S_ISCHR(m) ? DT_CHR :
                                 S_ISBLK(m) ? DT_BLK :
                                 S_ISFIFO(m) ? DT_FIFO : DT_REG;
        }
        f->f_pos++;
        filled++;
    }
    return filled;
}

struct file_ops ramfs_dir_ops = {
    .open    = ramfs_open,
    .release = ramfs_release,
    .read    = ramfs_dir_read,
    .readdir = ramfs_readdir,
};

/* ─── super_block ops ─────────────────────────────────────────────────────── */
static struct super_ops ramfs_sb_ops = { 0 };

/* ─── mount ───────────────────────────────────────────────────────────────── */
static struct super_block *ramfs_mount(struct filesystem_type *type,
                                        const char *dev, const char *opts) {
    (void)type; (void)dev; (void)opts;
    memset(&ramfs_sb, 0, sizeof(ramfs_sb));
    ramfs_sb.s_blocksize = PAGE_SIZE;
    ramfs_sb.s_magic     = 0x858458F6;  /* ramfs magic */
    ramfs_sb.s_ops       = &ramfs_sb_ops;
    spin_init(&ramfs_sb.s_lock);
    list_init(&ramfs_sb.s_inodes);
    list_init(&ramfs_sb.s_list);

    /* create root inode */
    struct inode *root_i = ramfs_new_inode(S_IFDIR | 0755);
    root_i->i_ops  = &ramfs_inode_ops;
    root_i->i_fops = &ramfs_dir_ops;
    root_i->i_nlink = 2;

    struct dentry *root = dentry_alloc("/", NULL);
    root->d_inode = inode_get(root_i);
    root->d_sb    = &ramfs_sb;
    ramfs_sb.s_root = root;
    return &ramfs_sb;
}

static struct filesystem_type ramfs_type = {
    .name  = "ramfs",
    .mount = ramfs_mount,
};

static struct filesystem_type tmpfs_type = {
    .name  = "tmpfs",
    .mount = ramfs_mount,  /* tmpfs = ramfs for now */
};

void ramfs_init(void) {
    register_filesystem(&ramfs_type);
    register_filesystem(&tmpfs_type);
}

/* ─── ramfs_populate: create a file with given content ──────────────────── */
int ramfs_create_file(struct dentry *dir, const char *name,
                       const void *data, size_t size, mode_t mode) {
    struct dentry *de = dentry_alloc(name, dir);
    if (!de) return -ENOMEM;
    int r = ramfs_create(dir->d_inode, de, mode);
    if (r) { dentry_put(de); return r; }
    if (data && size) {
        struct ramfs_inode *ri = de->d_inode->i_private;
        ri->data = kmalloc(size);
        if (!ri->data) { ramfs_unlink(dir->d_inode, de); return -ENOMEM; }
        memcpy(ri->data, data, size);
        ri->capacity = size;
        de->d_inode->i_size = size;
    }
    return 0;
}

int ramfs_create_dir(struct dentry *parent, const char *name, mode_t mode) {
    struct dentry *de = dentry_alloc(name, parent);
    if (!de) return -ENOMEM;
    return ramfs_mkdir(parent->d_inode, de, mode);
}

/* Helper to add the "." and ".." entries */
void ramfs_add_dotdot(struct dentry *dir) {
    /* VFS handles . and .. specially in path resolution */
    (void)dir;
}
