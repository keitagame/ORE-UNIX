/* fs/vfs.c – Virtual Filesystem core */
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/mm.h>

/* ─── Globals ─────────────────────────────────────────────────────────────── */
struct dentry *vfs_root = NULL;
static LIST_HEAD(fs_types);
static LIST_HEAD(mounts);
static spinlock_t vfs_lock = SPINLOCK_INIT;

static struct kmem_cache *inode_cache;
static struct kmem_cache *dentry_cache;
static struct kmem_cache *file_cache;

/* ─── inode ───────────────────────────────────────────────────────────────── */
struct inode *inode_alloc(struct super_block *sb) {
    struct inode *i = kmem_cache_alloc(inode_cache);
    if (!i) return NULL;
    memset(i, 0, sizeof(*i));
    i->i_sb       = sb;
    i->i_refcount = 1;
    spin_init(&i->i_lock);
    list_init(&i->i_list);
    init_waitqueue_head(&i->i_wait);
    if (sb) list_add_tail(&i->i_list, &sb->s_inodes);
    return i;
}

void inode_free(struct inode *inode) {
    list_del(&inode->i_list);
    if (inode->i_sb && inode->i_sb->s_ops && inode->i_sb->s_ops->free_inode)
        inode->i_sb->s_ops->free_inode(inode);
    else
        kmem_cache_free(inode_cache, inode);
}

struct inode *inode_get(struct inode *i) {
    if (i) __sync_fetch_and_add(&i->i_refcount, 1);
    return i;
}

void inode_put(struct inode *i) {
    if (!i) return;
    if (__sync_sub_and_fetch(&i->i_refcount, 1) == 0)
        inode_free(i);
}

/* ─── dentry ──────────────────────────────────────────────────────────────── */
struct dentry *dentry_alloc(const char *name, struct dentry *parent) {
    struct dentry *de = kmem_cache_alloc(dentry_cache);
    if (!de) return NULL;
    memset(de, 0, sizeof(*de));
    strncpy(de->d_name, name, NAME_MAX);
    de->d_parent   = parent;
    de->d_refcount = 1;
    spin_init(&de->d_lock);
    list_init(&de->d_children);
    list_init(&de->d_sibling);
    list_init(&de->d_lru);
    if (parent) {
        list_add_tail(&de->d_sibling, &parent->d_children);
        de->d_sb = parent->d_sb;
    }
    return de;
}

void dentry_free(struct dentry *de) {
    list_del(&de->d_sibling);
    if (de->d_inode) inode_put(de->d_inode);
    kmem_cache_free(dentry_cache, de);
}

struct dentry *dentry_get(struct dentry *de) {
    if (de) __sync_fetch_and_add(&de->d_refcount, 1);
    return de;
}

void dentry_put(struct dentry *de) {
    if (!de) return;
    if (__sync_sub_and_fetch(&de->d_refcount, 1) == 0)
        dentry_free(de);
}

static struct dentry *dentry_lookup_child(struct dentry *parent, const char *name) {
    struct list_head *pos;
    list_for_each(pos, &parent->d_children) {
        struct dentry *de = list_entry(pos, struct dentry, d_sibling);
        if (strcmp(de->d_name, name) == 0) return de;
    }
    return NULL;
}

/* ─── file ────────────────────────────────────────────────────────────────── */
struct file *file_alloc(void) {
    struct file *f = kmem_cache_alloc(file_cache);
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->f_refcount = 1;
    spin_init(&f->f_lock);
    init_waitqueue_head(&f->f_wait);
    return f;
}

void file_free(struct file *f) {
    if (f->f_ops && f->f_ops->release && f->f_dentry)
        f->f_ops->release(f->f_dentry->d_inode, f);
    if (f->f_dentry) dentry_put(f->f_dentry);
    kmem_cache_free(file_cache, f);
}

struct file *file_get(struct file *f) {
    if (f) __sync_fetch_and_add(&f->f_refcount, 1);
    return f;
}

void file_put(struct file *f) {
    if (!f) return;
    if (__sync_sub_and_fetch(&f->f_refcount, 1) == 0)
        file_free(f);
}

/* ─── Filesystem registration ────────────────────────────────────────────── */
int register_filesystem(struct filesystem_type *fs) {
    irq_flags_t f;
    spin_lock_irqsave(&vfs_lock, &f);
    list_add_tail(&fs->list, &fs_types);
    spin_unlock_irqrestore(&vfs_lock, &f);
    printk(KERN_INFO "VFS: registered fs '%s'\n", fs->name);
    return 0;
}

void unregister_filesystem(struct filesystem_type *fs) {
    irq_flags_t f;
    spin_lock_irqsave(&vfs_lock, &f);
    list_del(&fs->list);
    spin_unlock_irqrestore(&vfs_lock, &f);
}

static struct filesystem_type *find_fs(const char *name) {
    struct filesystem_type *fs;
    list_for_each_entry(fs, &fs_types, list) {
        if (strcmp(fs->name, name) == 0) return fs;
    }
    return NULL;
}

/* ─── Path resolution ─────────────────────────────────────────────────────── */
/* resolve a path to a dentry.  Returns 0 and sets *out on success. */
int vfs_lookup(const char *path, struct dentry **out, struct dentry *cwd) {
    if (!path || !*path) return -EINVAL;

    struct dentry *de;
    if (path[0] == '/') {
        de = dentry_get(vfs_root);
        path++;
    } else {
        de = dentry_get(cwd ? cwd : (current ? current->cwd : vfs_root));
    }

    /* walk components */
    char component[NAME_MAX + 1];
    while (*path) {
        /* skip slashes */
        while (*path == '/') path++;
        if (!*path) break;

        /* extract component */
        int len = 0;
        while (*path && *path != '/') component[len++] = *path++;
        component[len] = '\0';

        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            if (de->d_parent) {
                struct dentry *parent = dentry_get(de->d_parent);
                dentry_put(de);
                de = parent;
            }
            continue;
        }

        /* follow mountpoints */
        if (de->d_mounted && de->d_mountpoint)
            de = de->d_mountpoint;

        /* check if inode is a directory */
        if (!de->d_inode) { dentry_put(de); return -ENOENT; }
        if (!S_ISDIR(de->d_inode->i_mode)) { dentry_put(de); return -ENOTDIR; }

        /* look up in dentry cache */
        struct dentry *child = dentry_lookup_child(de, component);
        if (!child) {
            /* call filesystem lookup */
            child = dentry_alloc(component, de);
            if (!child) { dentry_put(de); return -ENOMEM; }
            if (!de->d_inode->i_ops || !de->d_inode->i_ops->lookup) {
                dentry_put(child); dentry_put(de); return -ENOENT;
            }
            int r = de->d_inode->i_ops->lookup(de->d_inode, child);
            if (r) { dentry_put(child); dentry_put(de); return r; }
        } else {
            dentry_get(child);
        }
        dentry_put(de);
        de = child;

        /* handle symlinks */
        if (de->d_inode && S_ISLNK(de->d_inode->i_mode) && *path) {
            char lnk[PATH_MAX];
#define PATH_MAX 4096
            if (!de->d_inode->i_ops || !de->d_inode->i_ops->readlink) {
                dentry_put(de); return -EINVAL;
            }
            int r2 = de->d_inode->i_ops->readlink(de->d_inode, lnk, PATH_MAX);
            if (r2 < 0) { dentry_put(de); return r2; }
            lnk[r2] = '\0';
            struct dentry *lde;
            r2 = vfs_lookup(lnk, &lde, de->d_parent);
            dentry_put(de);
            if (r2) return r2;
            de = lde;
        }
    }

    *out = de;
    return 0;
}

/* resolve parent dentry and final component name */
int vfs_lookup_parent(const char *path, struct dentry **parent,
                       char *name, struct dentry *cwd) {
    /* find last '/' */
    const char *slash = strrchr(path, '/');
    if (!slash) {
        /* relative, no slash */
        *parent = dentry_get(cwd ? cwd : current->cwd);
        strncpy(name, path, NAME_MAX);
        return 0;
    }
    if (slash == path) {
        *parent = dentry_get(vfs_root);
        strncpy(name, slash + 1, NAME_MAX);
        return 0;
    }
    /* path up to slash */
    char dir[4096];
    size_t dlen = (size_t)(slash - path);
    if (dlen >= 4096) return -ENAMETOOLONG;
    memcpy(dir, path, dlen);
    dir[dlen] = '\0';
    strncpy(name, slash + 1, NAME_MAX);
    return vfs_lookup(dir, parent, cwd);
}

/* ─── vfs_open ────────────────────────────────────────────────────────────── */
struct file *vfs_open(const char *path, int flags, mode_t mode) {
    struct dentry *de;
    int r;

    if (flags & O_CREAT) {
        struct dentry *parent;
        char name[NAME_MAX + 1];
        r = vfs_lookup_parent(path, &parent, name, NULL);
        if (r) return NULL;

        de = dentry_lookup_child(parent, name);
        if (!de) {
            de = dentry_alloc(name, parent);
            if (!de) { dentry_put(parent); return NULL; }
            if (!parent->d_inode->i_ops || !parent->d_inode->i_ops->create) {
                dentry_put(de); dentry_put(parent); return NULL;
            }
            r = parent->d_inode->i_ops->create(parent->d_inode, de, mode);
            if (r) { dentry_put(de); dentry_put(parent); return NULL; }
        } else {
            if ((flags & O_EXCL)) { dentry_put(de); dentry_put(parent); return NULL; }
        }
        dentry_put(parent);
    } else {
        r = vfs_lookup(path, &de, NULL);
        if (r) return NULL;
    }

    if (!de->d_inode) { dentry_put(de); return NULL; }

    /* check access */
    if ((flags & O_WRONLY || flags & O_RDWR) && S_ISDIR(de->d_inode->i_mode)) {
        dentry_put(de); return NULL;
    }

    struct file *f = file_alloc();
    if (!f) { dentry_put(de); return NULL; }
    f->f_dentry = de;   /* takes ref */
    f->f_ops    = de->d_inode->i_fops;
    f->f_flags  = flags;
    f->f_pos    = 0;

    if (flags & O_TRUNC && de->d_inode->i_ops && de->d_inode->i_ops->truncate)
        de->d_inode->i_ops->truncate(de->d_inode, 0);

    if (flags & O_APPEND)
        f->f_pos = de->d_inode->i_size;

    if (f->f_ops && f->f_ops->open) {
        r = f->f_ops->open(de->d_inode, f);
        if (r) { file_free(f); return NULL; }
    }

    return f;
}

int vfs_close(struct file *f) {
    file_put(f);
    return 0;
}

ssize_t vfs_read(struct file *f, void *buf, size_t count) {
    if (!f->f_ops || !f->f_ops->read) return -EINVAL;
    if (!(f->f_flags == O_RDONLY || f->f_flags & O_RDWR)) return -EBADF;
    return f->f_ops->read(f, buf, count, &f->f_pos);
}

ssize_t vfs_write(struct file *f, const void *buf, size_t count) {
    if (!f->f_ops || !f->f_ops->write) return -EINVAL;
    if (f->f_flags == O_RDONLY) return -EBADF;
    return f->f_ops->write(f, buf, count, &f->f_pos);
}

off_t vfs_lseek(struct file *f, off_t offset, int whence) {
    if (f->f_ops && f->f_ops->lseek)
        return f->f_ops->lseek(f, offset, whence);
    struct inode *i = f->f_dentry->d_inode;
    switch (whence) {
    case SEEK_SET: f->f_pos = offset; break;
    case SEEK_CUR: f->f_pos += offset; break;
    case SEEK_END: f->f_pos = i->i_size + offset; break;
    default: return -EINVAL;
    }
    if (f->f_pos < 0) f->f_pos = 0;
    return f->f_pos;
}

int vfs_stat(const char *path, struct stat *st) {
    struct dentry *de;
    int r = vfs_lookup(path, &de, NULL);
    if (r) return r;
    r = vfs_fstat_inode(de->d_inode, st);
    dentry_put(de);
    return r;
}

int vfs_fstat_inode(struct inode *i, struct stat *st) {
    if (i->i_ops && i->i_ops->getattr) return i->i_ops->getattr(i, st);
    st->st_ino   = i->i_ino;
    st->st_mode  = i->i_mode;
    st->st_nlink = i->i_nlink;
    st->st_uid   = i->i_uid;
    st->st_gid   = i->i_gid;
    st->st_rdev  = i->i_rdev;
    st->st_size  = i->i_size;
    st->st_blksize = PAGE_SIZE;
    st->st_blocks  = (i->i_size + 511) / 512;
    st->st_atime   = i->i_atime;
    st->st_mtime   = i->i_mtime;
    st->st_ctime   = i->i_ctime;
    return 0;
}

int vfs_fstat(struct file *f, struct stat *st) {
    if (!f->f_dentry || !f->f_dentry->d_inode) return -EBADF;
    return vfs_fstat_inode(f->f_dentry->d_inode, st);
}

int vfs_readdir(struct file *f, struct dirent *de, int count) {
    if (!f->f_ops || !f->f_ops->readdir) return -ENOTDIR;
    return f->f_ops->readdir(f, de, count);
}

int vfs_ioctl(struct file *f, u32 cmd, uptr arg) {
    if (!f->f_ops || !f->f_ops->ioctl) return -ENOTTY;
    return f->f_ops->ioctl(f, cmd, arg);
}

/* ─── Directory ops ──────────────────────────────────────────────────────── */
int vfs_mkdir(const char *path, mode_t mode) {
    struct dentry *parent; char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(path, &parent, name, NULL);
    if (r) return r;
    struct dentry *de = dentry_alloc(name, parent);
    if (!de) { dentry_put(parent); return -ENOMEM; }
    if (!parent->d_inode->i_ops || !parent->d_inode->i_ops->mkdir)
        { dentry_put(de); dentry_put(parent); return -EPERM; }
    r = parent->d_inode->i_ops->mkdir(parent->d_inode, de, mode | S_IFDIR);
    dentry_put(parent);
    if (r) dentry_put(de);
    return r;
}

int vfs_unlink(const char *path) {
    struct dentry *parent; char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(path, &parent, name, NULL);
    if (r) return r;
    struct dentry *de = dentry_lookup_child(parent, name);
    if (!de) { dentry_put(parent); return -ENOENT; }
    if (S_ISDIR(de->d_inode->i_mode)) { dentry_put(parent); return -EISDIR; }
    if (!parent->d_inode->i_ops || !parent->d_inode->i_ops->unlink)
        { dentry_put(parent); return -EPERM; }
    r = parent->d_inode->i_ops->unlink(parent->d_inode, de);
    dentry_put(parent);
    return r;
}

int vfs_rmdir(const char *path) {
    struct dentry *parent; char name[NAME_MAX + 1];
    int r = vfs_lookup_parent(path, &parent, name, NULL);
    if (r) return r;
    struct dentry *de = dentry_lookup_child(parent, name);
    if (!de) { dentry_put(parent); return -ENOENT; }
    if (!list_empty(&de->d_children)) { dentry_put(parent); return -ENOTEMPTY; }
    if (!parent->d_inode->i_ops || !parent->d_inode->i_ops->rmdir)
        { dentry_put(parent); return -EPERM; }
    r = parent->d_inode->i_ops->rmdir(parent->d_inode, de);
    dentry_put(parent);
    return r;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    struct dentry *opar, *npar; char oname[NAME_MAX+1], nname[NAME_MAX+1];
    int r;
    r = vfs_lookup_parent(oldpath, &opar, oname, NULL); if (r) return r;
    r = vfs_lookup_parent(newpath, &npar, nname, NULL); if (r) { dentry_put(opar); return r; }
    struct dentry *ode = dentry_lookup_child(opar, oname);
    if (!ode) { dentry_put(opar); dentry_put(npar); return -ENOENT; }
    struct dentry *nde = dentry_alloc(nname, npar);
    if (!nde) { dentry_put(opar); dentry_put(npar); return -ENOMEM; }
    if (!opar->d_inode->i_ops || !opar->d_inode->i_ops->rename)
        { dentry_put(nde); dentry_put(opar); dentry_put(npar); return -EPERM; }
    r = opar->d_inode->i_ops->rename(opar->d_inode, ode, npar->d_inode, nde);
    dentry_put(opar); dentry_put(npar);
    return r;
}

int vfs_symlink(const char *target, const char *linkpath) {
    struct dentry *parent; char name[NAME_MAX+1];
    int r = vfs_lookup_parent(linkpath, &parent, name, NULL);
    if (r) return r;
    struct dentry *de = dentry_alloc(name, parent);
    if (!de) { dentry_put(parent); return -ENOMEM; }
    if (!parent->d_inode->i_ops || !parent->d_inode->i_ops->symlink)
        { dentry_put(de); dentry_put(parent); return -EPERM; }
    r = parent->d_inode->i_ops->symlink(parent->d_inode, de, target);
    dentry_put(parent);
    return r;
}

int vfs_readlink(const char *path, char *buf, size_t size) {
    struct dentry *de;
    int r = vfs_lookup(path, &de, NULL);
    if (r) return r;
    if (!S_ISLNK(de->d_inode->i_mode)) { dentry_put(de); return -EINVAL; }
    r = de->d_inode->i_ops->readlink(de->d_inode, buf, size);
    dentry_put(de);
    return r;
}

/* ─── Mount / umount ──────────────────────────────────────────────────────── */
int vfs_mount(const char *dev, const char *point,
              const char *fstype, const char *opts) {
    struct filesystem_type *fs = find_fs(fstype);
    if (!fs) { printk(KERN_ERR "VFS: unknown fs type '%s'\n", fstype); return -ENODEV; }

    struct super_block *sb = fs->mount(fs, dev, opts);
    if (!sb) return -EIO;

    struct mount *mnt = kmalloc(sizeof(*mnt));
    if (!mnt) return -ENOMEM;
    memset(mnt, 0, sizeof(*mnt));
    mnt->mnt_sb = sb;
    strncpy(mnt->mnt_devname, dev ? dev : "none", 63);
    strncpy(mnt->mnt_fstype, fstype, 31);

    if (!vfs_root) {
        /* first mount = root */
        vfs_root = sb->s_root;
        dentry_get(vfs_root);
        mnt->mnt_point = vfs_root;
    } else {
        struct dentry *mp;
        int r = vfs_lookup(point, &mp, NULL);
        if (r) { kfree(mnt); return r; }
        mp->d_mounted    = true;
        mp->d_mountpoint = sb->s_root;
        mnt->mnt_point   = mp;
    }

    list_add_tail(&mnt->mnt_list, &mounts);
    printk(KERN_INFO "VFS: mounted %s on %s (%s)\n",
           dev ? dev : "none", point ? point : "/", fstype);
    return 0;
}

int vfs_umount(const char *point) {
    struct list_head *pos;
    list_for_each(pos, &mounts) {
        struct mount *mnt = list_entry(pos, struct mount, mnt_list);
        if (strcmp(mnt->mnt_devname, point) == 0 ||
            (mnt->mnt_point && strcmp(mnt->mnt_point->d_name, point) == 0)) {
            list_del(&mnt->mnt_list);
            if (mnt->mnt_sb->s_type && mnt->mnt_sb->s_type->umount)
                mnt->mnt_sb->s_type->umount(mnt->mnt_sb);
            kfree(mnt);
            return 0;
        }
    }
    return -EINVAL;
}

/* ─── VFS init ────────────────────────────────────────────────────────────── */
void vfs_init(void) {
    inode_cache  = kmem_cache_create("inode",   sizeof(struct inode));
    dentry_cache = kmem_cache_create("dentry",  sizeof(struct dentry));
    file_cache   = kmem_cache_create("file",    sizeof(struct file));
    list_init(&fs_types);
    list_init(&mounts);
    printk(KERN_INFO "VFS: initialized\n");
}

/* ─── System call layer ───────────────────────────────────────────────────── */
static struct file *fd_to_file(int fd) {
    if (!current || !current->files) return NULL;
    if (fd < 0 || fd >= OPEN_MAX) return NULL;
    return current->files->fd[fd];
}

int sys_open(const char *path, int flags, mode_t mode) {
    struct file *f = vfs_open(path, flags, mode);
    if (!f) return -ENOENT;
    int fd = files_alloc_fd(current->files);
    if (fd < 0) { file_put(f); return fd; }
    current->files->fd[fd] = f;
    return fd;
}

int sys_close(int fd) {
    struct file *f = fd_to_file(fd);
    if (!f) return -EBADF;
    files_free_fd(current->files, fd);
    return 0;
}

ssize_t sys_read(int fd, void *buf, size_t count) {
    struct file *f = fd_to_file(fd);
    if (!f) return -EBADF;
    return vfs_read(f, buf, count);
}

ssize_t sys_write(int fd, const void *buf, size_t count) {
    struct file *f = fd_to_file(fd);
    if (!f) return -EBADF;
    return vfs_write(f, buf, count);
}

off_t sys_lseek(int fd, off_t off, int whence) {
    struct file *f = fd_to_file(fd);
    if (!f) return -EBADF;
    return vfs_lseek(f, off, whence);
}

int sys_stat(const char *path, struct stat *st)  { return vfs_stat(path, st); }
int sys_lstat(const char *path, struct stat *st) { return vfs_stat(path, st); /* TODO: no followlink */ }
int sys_fstat(int fd, struct stat *st) {
    struct file *f = fd_to_file(fd); if (!f) return -EBADF;
    return vfs_fstat(f, st);
}

int sys_mkdir(const char *path, mode_t mode)    { return vfs_mkdir(path, mode); }
int sys_rmdir(const char *path)                  { return vfs_rmdir(path); }
int sys_unlink(const char *path)                 { return vfs_unlink(path); }
int sys_rename(const char *o, const char *n)     { return vfs_rename(o, n); }
int sys_symlink(const char *t, const char *l)    { return vfs_symlink(t, l); }
int sys_readlink(const char *p, char *b, size_t s) { return vfs_readlink(p, b, s); }

int sys_dup(int fd) {
    struct file *f = fd_to_file(fd); if (!f) return -EBADF;
    int nfd = files_alloc_fd(current->files); if (nfd < 0) return nfd;
    current->files->fd[nfd] = file_get(f);
    return nfd;
}

int sys_dup2(int old, int nw) {
    if (old == nw) return nw;
    struct file *f = fd_to_file(old); if (!f) return -EBADF;
    if (nw < 0 || nw >= OPEN_MAX) return -EBADF;
    if (current->files->fd[nw]) files_free_fd(current->files, nw);
    current->files->fd[nw] = file_get(f);
    return nw;
}

int sys_pipe(int fds[2]) {
    struct file *r, *w;
    int ret = vfs_pipe(&r, &w);
    if (ret) return ret;
    int rfd = files_alloc_fd(current->files);
    if (rfd < 0) { file_put(r); file_put(w); return rfd; }
    current->files->fd[rfd] = r;
    int wfd = files_alloc_fd(current->files);
    if (wfd < 0) { files_free_fd(current->files, rfd); file_put(w); return wfd; }
    current->files->fd[wfd] = w;
    fds[0] = rfd; fds[1] = wfd;
    return 0;
}

int sys_chdir(const char *path) {
    struct dentry *de;
    int r = vfs_lookup(path, &de, NULL);
    if (r) return r;
    if (!S_ISDIR(de->d_inode->i_mode)) { dentry_put(de); return -ENOTDIR; }
    dentry_put(current->cwd);
    current->cwd = de;
    return 0;
}

int sys_getcwd(char *buf, size_t sz) {
    if (!current->cwd) { strncpy(buf, "/", sz); return 0; }
    /* walk up dentry tree to build path */
    char tmp[4096];
    char *p = tmp + 4095;
    *p = '\0';
    struct dentry *de = current->cwd;
    while (de && de != vfs_root && de->d_parent) {
        size_t len = strlen(de->d_name);
        p -= len; memcpy(p, de->d_name, len);
        *--p = '/';
        de = de->d_parent;
    }
    if (*p != '/') *--p = '/';
    strncpy(buf, p, sz);
    return 0;
}

int sys_ioctl(int fd, u32 cmd, uptr arg) {
    struct file *f = fd_to_file(fd); if (!f) return -EBADF;
    return vfs_ioctl(f, cmd, arg);
}

int sys_getdents(int fd, struct dirent *de, size_t sz) {
    struct file *f = fd_to_file(fd); if (!f) return -EBADF;
    return vfs_readdir(f, de, sz / sizeof(struct dirent));
}

int sys_chmod(const char *path, mode_t mode) {
    struct dentry *de; int r = vfs_lookup(path, &de, NULL); if (r) return r;
    r = de->d_inode->i_ops ? de->d_inode->i_ops->chmod(de->d_inode, mode) : -EPERM;
    dentry_put(de); return r;
}

int sys_chown(const char *path, uid_t u, gid_t g) {
    struct dentry *de; int r = vfs_lookup(path, &de, NULL); if (r) return r;
    r = de->d_inode->i_ops ? de->d_inode->i_ops->chown(de->d_inode, u, g) : -EPERM;
    dentry_put(de); return r;
}

int sys_mknod(const char *path, mode_t mode, dev_t dev) {
    struct dentry *parent; char name[NAME_MAX+1];
    int r = vfs_lookup_parent(path, &parent, name, NULL); if (r) return r;
    struct dentry *de = dentry_alloc(name, parent);
    if (!de) { dentry_put(parent); return -ENOMEM; }
    r = parent->d_inode->i_ops->mknod(parent->d_inode, de, mode, dev);
    dentry_put(parent); return r;
}

int sys_mount(const char *dev, const char *point,
              const char *fstype, u32 flags, const void *data) {
    (void)flags; (void)data;
    return vfs_mount(dev, point, fstype, NULL);
}

int sys_umount(const char *point) { return vfs_umount(point); }
int sys_link(const char *o, const char *n) {
    struct dentry *src; int r = vfs_lookup(o, &src, NULL); if (r) return r;
    struct dentry *parent; char name[NAME_MAX+1];
    r = vfs_lookup_parent(n, &parent, name, NULL); if (r) { dentry_put(src); return r; }
    struct dentry *de = dentry_alloc(name, parent);
    if (!de) { dentry_put(src); dentry_put(parent); return -ENOMEM; }
    r = parent->d_inode->i_ops->link(parent->d_inode, src->d_inode, de);
    dentry_put(src); dentry_put(parent); return r;
}
