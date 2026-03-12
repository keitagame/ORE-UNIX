#ifndef _VFS_H
#define _VFS_H

#include "kernel.h"

/* ─── File types (mode bits) ─────────────────────────────────────────────── */
#define S_IFMT    0xF000
#define S_IFSOCK  0xC000
#define S_IFLNK   0xA000
#define S_IFREG   0x8000
#define S_IFBLK   0x6000
#define S_IFDIR   0x4000
#define S_IFCHR   0x2000
#define S_IFIFO   0x1000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

/* permission bits */
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_ISUID 04000
#define S_ISGID 02000

/* open() flags */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_RDWR     0x0002
#define O_ACCMODE  0x0003
#define O_CREAT    0x0040
#define O_EXCL     0x0080
#define O_TRUNC    0x0200
#define O_APPEND   0x0400
#define O_NONBLOCK 0x0800
#define O_CLOEXEC  0x80000
#define O_DIRECTORY 0x10000

/* seek */
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

/* lseek special */
#define EINVAL_SEEK  EINVAL

/* ─── stat structure ─────────────────────────────────────────────────────── */
struct stat {
    dev_t    st_dev;
    ino_t    st_ino;
    mode_t   st_mode;
    nlink_t  st_nlink;
    uid_t    st_uid;
    gid_t    st_gid;
    dev_t    st_rdev;
    off_t    st_size;
    u32      st_blksize;
    u32      st_blocks;
    u32      st_atime;
    u32      st_mtime;
    u32      st_ctime;
};

/* ─── dirent ─────────────────────────────────────────────────────────────── */
#define NAME_MAX 255
struct dirent {
    ino_t  d_ino;
    off_t  d_off;
    u16    d_reclen;
    u8     d_type;
    char   d_name[NAME_MAX + 1];
};
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

/* ─── Forward declarations ───────────────────────────────────────────────── */
struct inode;
struct dentry;
struct file;
struct super_block;
struct filesystem_type;

/* ─── inode operations ───────────────────────────────────────────────────── */
struct inode_ops {
    int (*lookup)(struct inode *dir, struct dentry *de);
    int (*create)(struct inode *dir, struct dentry *de, mode_t mode);
    int (*mkdir)(struct inode *dir, struct dentry *de, mode_t mode);
    int (*rmdir)(struct inode *dir, struct dentry *de);
    int (*unlink)(struct inode *dir, struct dentry *de);
    int (*rename)(struct inode *old_dir, struct dentry *old,
                  struct inode *new_dir, struct dentry *nw);
    int (*link)(struct inode *dir, struct inode *target, struct dentry *de);
    int (*symlink)(struct inode *dir, struct dentry *de, const char *target);
    int (*readlink)(struct inode *inode, char *buf, size_t size);
    int (*mknod)(struct inode *dir, struct dentry *de, mode_t mode, dev_t dev);
    int (*truncate)(struct inode *inode, off_t size);
    int (*chmod)(struct inode *inode, mode_t mode);
    int (*chown)(struct inode *inode, uid_t uid, gid_t gid);
    int (*getattr)(struct inode *inode, struct stat *st);
};

/* ─── file operations ────────────────────────────────────────────────────── */
struct file_ops {
    int    (*open)(struct inode *inode, struct file *f);
    int    (*release)(struct inode *inode, struct file *f);
    ssize_t (*read)(struct file *f, char *buf, size_t count, off_t *pos);
    ssize_t (*write)(struct file *f, const char *buf, size_t count, off_t *pos);
    off_t  (*lseek)(struct file *f, off_t offset, int whence);
    int    (*readdir)(struct file *f, struct dirent *de, int count);
    int    (*ioctl)(struct file *f, u32 cmd, uptr arg);
    int    (*poll)(struct file *f, int events);
    int    (*mmap)(struct file *f, struct mm_struct *mm, uptr addr,
                   size_t len, u32 flags, off_t offset);
    int    (*fsync)(struct file *f);
};

/* ─── inode ──────────────────────────────────────────────────────────────── */
struct inode {
    ino_t             i_ino;
    mode_t            i_mode;
    uid_t             i_uid;
    gid_t             i_gid;
    nlink_t           i_nlink;
    off_t             i_size;
    dev_t             i_rdev;       /* for device files */
    u32               i_atime;
    u32               i_mtime;
    u32               i_ctime;
    u32               i_blksize;
    u32               i_blocks;

    struct super_block *i_sb;
    struct inode_ops   *i_ops;
    struct file_ops    *i_fops;
    void               *i_private;  /* fs-specific data */

    u32                i_refcount;
    spinlock_t         i_lock;
    struct list_head   i_list;       /* sb->inodes */
    wait_queue_head_t  i_wait;
};

/* ─── dentry (directory entry cache) ────────────────────────────────────── */
struct dentry {
    char             d_name[NAME_MAX + 1];
    struct inode    *d_inode;
    struct dentry   *d_parent;
    struct list_head d_children;
    struct list_head d_sibling;
    struct list_head d_lru;
    u32              d_refcount;
    spinlock_t       d_lock;
    struct super_block *d_sb;
    bool             d_mounted;
    struct dentry   *d_mountpoint;   /* where something is mounted here */
};

/* ─── file ───────────────────────────────────────────────────────────────── */
struct file {
    struct dentry    *f_dentry;
    struct file_ops  *f_ops;
    off_t             f_pos;
    int               f_flags;       /* O_RDONLY etc */
    int               f_mode;        /* current access mode */
    u32               f_refcount;
    void             *f_private;
    spinlock_t        f_lock;
    wait_queue_head_t f_wait;
};

/* ─── super_block ────────────────────────────────────────────────────────── */
struct super_ops {
    struct inode *(*alloc_inode)(struct super_block *sb);
    void          (*free_inode)(struct inode *inode);
    int           (*write_inode)(struct inode *inode);
    int           (*sync_fs)(struct super_block *sb);
    void          (*put_super)(struct super_block *sb);
    int           (*statfs)(struct super_block *sb, struct statfs_info *info);
};

struct statfs_info {
    u64 total_blocks;
    u64 free_blocks;
    u64 total_inodes;
    u64 free_inodes;
    u32 block_size;
};

struct super_block {
    dev_t              s_dev;
    u32                s_blocksize;
    u64                s_magic;
    struct dentry     *s_root;
    struct super_ops  *s_ops;
    struct filesystem_type *s_type;
    void              *s_private;
    struct list_head   s_list;       /* global sb list */
    struct list_head   s_inodes;
    spinlock_t         s_lock;
    bool               s_rdonly;
};

/* ─── filesystem type ────────────────────────────────────────────────────── */
struct filesystem_type {
    const char  *name;
    struct super_block *(*mount)(struct filesystem_type *type,
                                  const char *dev, const char *opts);
    void        (*umount)(struct super_block *sb);
    struct list_head list;
};

/* ─── Mount table entry ──────────────────────────────────────────────────── */
struct mount {
    struct dentry       *mnt_point;   /* dentry in parent FS */
    struct super_block  *mnt_sb;
    struct mount        *mnt_parent;
    struct list_head     mnt_list;
    char                 mnt_devname[64];
    char                 mnt_fstype[32];
};

/* ─── VFS interface ──────────────────────────────────────────────────────── */
void vfs_init(void);

/* path resolution */
int vfs_lookup(const char *path, struct dentry **out, struct dentry *cwd);
int vfs_lookup_parent(const char *path, struct dentry **parent,
                       char *name, struct dentry *cwd);

/* file operations */
struct file *vfs_open(const char *path, int flags, mode_t mode);
int          vfs_close(struct file *f);
ssize_t      vfs_read(struct file *f, void *buf, size_t count);
ssize_t      vfs_write(struct file *f, const void *buf, size_t count);
off_t        vfs_lseek(struct file *f, off_t offset, int whence);
int          vfs_stat(const char *path, struct stat *st);
int          vfs_fstat(struct file *f, struct stat *st);
int          vfs_readdir(struct file *f, struct dirent *de, int count);
int          vfs_ioctl(struct file *f, u32 cmd, uptr arg);

/* directory operations */
int vfs_mkdir(const char *path, mode_t mode);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rename(const char *oldpath, const char *newpath);
int vfs_link(const char *oldpath, const char *newpath);
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, size_t size);
int vfs_chmod(const char *path, mode_t mode);
int vfs_chown(const char *path, uid_t uid, gid_t gid);
int vfs_mknod(const char *path, mode_t mode, dev_t dev);
int vfs_truncate(const char *path, off_t size);

/* mount / unmount */
int vfs_mount(const char *dev, const char *point,
              const char *fstype, const char *opts);
int vfs_umount(const char *point);

/* fs registration */
int  register_filesystem(struct filesystem_type *fs);
void unregister_filesystem(struct filesystem_type *fs);

/* inode / dentry helpers */
struct inode  *inode_alloc(struct super_block *sb);
void           inode_free(struct inode *inode);
struct inode  *inode_get(struct inode *inode);
void           inode_put(struct inode *inode);

struct dentry *dentry_alloc(const char *name, struct dentry *parent);
void           dentry_free(struct dentry *de);
struct dentry *dentry_get(struct dentry *de);
void           dentry_put(struct dentry *de);

struct file   *file_alloc(void);
void           file_free(struct file *f);
struct file   *file_get(struct file *f);
void           file_put(struct file *f);

/* global roots */
extern struct dentry *vfs_root;

/* pipe */
int  vfs_pipe(struct file **read_end, struct file **write_end);

/* System call layer */
int    sys_open(const char *path, int flags, mode_t mode);
int    sys_close(int fd);
ssize_t sys_read(int fd, void *buf, size_t count);
ssize_t sys_write(int fd, const void *buf, size_t count);
off_t  sys_lseek(int fd, off_t off, int whence);
int    sys_stat(const char *path, struct stat *st);
int    sys_fstat(int fd, struct stat *st);
int    sys_lstat(const char *path, struct stat *st);
int    sys_mkdir(const char *path, mode_t mode);
int    sys_rmdir(const char *path);
int    sys_unlink(const char *path);
int    sys_rename(const char *old, const char *nw);
int    sys_link(const char *old, const char *nw);
int    sys_symlink(const char *tgt, const char *lp);
int    sys_readlink(const char *path, char *buf, size_t sz);
int    sys_chmod(const char *path, mode_t mode);
int    sys_chown(const char *path, uid_t u, gid_t g);
int    sys_dup(int fd);
int    sys_dup2(int old, int nw);
int    sys_pipe(int fds[2]);
int    sys_chdir(const char *path);
int    sys_getcwd(char *buf, size_t sz);
int    sys_ioctl(int fd, u32 cmd, uptr arg);
int    sys_getdents(int fd, struct dirent *de, size_t sz);
int    sys_mknod(const char *path, mode_t mode, dev_t dev);
int    sys_mount(const char *dev, const char *point,
                 const char *fstype, u32 flags, const void *data);
int    sys_umount(const char *point);

#endif /* _VFS_H */
