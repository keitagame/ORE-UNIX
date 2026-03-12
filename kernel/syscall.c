/* kernel/syscall.c – int 0x80 system call dispatcher */
#include <kernel/kernel.h>
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <kernel/mm.h>
#include <arch/x86/arch.h>

/* Linux i386 ABI: eax=syscall#, ebx,ecx,edx,esi,edi,ebp = args 1-6
   Return value in eax; negative = -errno.                              */

/* ─── System call numbers (Linux i386 compatible) ────────────────────────── */
#define SYS_exit          1
#define SYS_fork          2
#define SYS_read          3
#define SYS_write         4
#define SYS_open          5
#define SYS_close         6
#define SYS_waitpid       7
#define SYS_creat         8
#define SYS_link          9
#define SYS_unlink        10
#define SYS_execve        11
#define SYS_chdir         12
#define SYS_time          13
#define SYS_mknod         14
#define SYS_chmod         15
#define SYS_lchown        16
#define SYS_lseek         19
#define SYS_getpid        20
#define SYS_setuid        23
#define SYS_getuid        24
#define SYS_kill          37
#define SYS_rename        38
#define SYS_mkdir         39
#define SYS_rmdir         40
#define SYS_dup           41
#define SYS_pipe          42
#define SYS_times         43
#define SYS_brk           45
#define SYS_setgid        46
#define SYS_getgid        47
#define SYS_geteuid       49
#define SYS_getegid       50
#define SYS_ioctl         54
#define SYS_fcntl         55
#define SYS_setpgid       57
#define SYS_umask         60
#define SYS_dup2          63
#define SYS_getppid       64
#define SYS_getpgrp       65
#define SYS_setsid        66
#define SYS_sigaction     67
#define SYS_setreuid      70
#define SYS_setregid      71
#define SYS_symlink       83
#define SYS_readlink      85
#define SYS_mmap          90
#define SYS_munmap        91
#define SYS_truncate      92
#define SYS_ftruncate     93
#define SYS_fchmod        94
#define SYS_fchown        95
#define SYS_stat          106
#define SYS_lstat         107
#define SYS_fstat         108
#define SYS_uname         122
#define SYS_mprotect      125
#define SYS_sigreturn     119
#define SYS_clone         120
#define SYS_nanosleep     162
#define SYS_getdents      141
#define SYS_getcwd        183
#define SYS_chown         182
#define SYS_exit_group    252
#define SYS_set_tid_address 258
#define SYS_clock_gettime 265
#define SYS_gettid        224
#define SYS_tgkill        270
#define SYS_openat        295
#define SYS_mount         21
#define SYS_umount        22
#define SYS_getdents64    220

/* ─── mmap args struct ────────────────────────────────────────────────────── */
struct mmap_args {
    u32 addr, len, prot, flags, fd;
    s32 offset;
};

/* ─── utsname ─────────────────────────────────────────────────────────────── */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* ─── timespec ────────────────────────────────────────────────────────────── */
struct timespec { s32 tv_sec; s32 tv_nsec; };

/* ─── fcntl commands ──────────────────────────────────────────────────────── */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define FD_CLOEXEC 1

/* ─── mmap flags ──────────────────────────────────────────────────────────── */
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED     0x10
#define MAP_ANON      MAP_ANONYMOUS

/* ─── safe user-space string copy ────────────────────────────────────────── */
static int copy_from_user(void *dst, const void *src, size_t n) {
    /* TODO: proper EFAULT check via page table walk */
    if (!src) return -EFAULT;
    memcpy(dst, src, n);
    return 0;
}
static int strncpy_from_user(char *dst, const char *src, size_t n) {
    if (!src) return -EFAULT;
    strncpy(dst, src, n);
    return 0;
}
static int copy_to_user(void *dst, const void *src, size_t n) {
    if (!dst) return -EFAULT;
    memcpy(dst, src, n);
    return 0;
}

/* ─── Dispatcher ──────────────────────────────────────────────────────────── */
void syscall_handler(struct regs *r) {
    u32 num  = r->eax;
    u32 arg1 = r->ebx;
    u32 arg2 = r->ecx;
    u32 arg3 = r->edx;
    u32 arg4 = r->esi;
    u32 arg5 = r->edi;
    (void)arg4; (void)arg5;

    s32 ret = -ENOSYS;

    switch (num) {

    /* ── process ── */
    case SYS_exit:
    case SYS_exit_group:
        sys_exit((int)arg1);
        break;  /* noreturn */

    case SYS_fork:
        ret = sys_fork(r);
        break;

    case SYS_execve: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_exec(path, (char*const*)arg2, (char*const*)arg3);
        break;
    }

    case SYS_waitpid:
        ret = sys_waitpid((pid_t)arg1, (int*)arg2, (int)arg3);
        break;

    case SYS_getpid:  ret = sys_getpid();  break;
    case SYS_getppid: ret = sys_getppid(); break;
    case SYS_gettid:  ret = current->pid;  break;
    case SYS_getpgrp: ret = sys_getpgrp(); break;
    case SYS_setsid:  ret = sys_setsid();  break;

    case SYS_setpgid:
        ret = sys_setpgid((pid_t)arg1, (pid_t)arg2);
        break;

    case SYS_kill:
        ret = sys_kill((pid_t)arg1, (int)arg2);
        break;

    case SYS_tgkill:
        ret = sys_kill((pid_t)arg2, (int)arg3);
        break;

    case SYS_sigaction: {
        struct sigaction act, old;
        if (arg2) copy_from_user(&act, (void*)arg2, sizeof(act));
        ret = sys_sigaction((int)arg1,
                             arg2 ? &act : NULL,
                             arg3 ? &old : NULL);
        if (!ret && arg3) copy_to_user((void*)arg3, &old, sizeof(old));
        break;
    }

    case SYS_sigreturn:
        /* restore user context from signal frame – simplified */
        ret = 0;
        break;

    case SYS_set_tid_address:
        ret = current->pid;
        break;

    /* ── credentials ── */
    case SYS_getuid:  ret = current->uid;  break;
    case SYS_geteuid: ret = current->euid; break;
    case SYS_getgid:  ret = current->gid;  break;
    case SYS_getegid: ret = current->egid; break;

    case SYS_setuid:
        if (current->euid == 0) { current->uid = current->euid = arg1; ret = 0; }
        else ret = -EPERM;
        break;
    case SYS_setgid:
        if (current->euid == 0) { current->gid = current->egid = arg1; ret = 0; }
        else ret = -EPERM;
        break;
    case SYS_setreuid:
        current->uid = arg1; current->euid = arg2; ret = 0;
        break;
    case SYS_setregid:
        current->gid = arg1; current->egid = arg2; ret = 0;
        break;

    case SYS_umask: {
        mode_t old = current->umask;
        current->umask = arg1 & 0777;
        ret = old;
        break;
    }

    /* ── file I/O ── */
    case SYS_open: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_open(path, (int)arg2, (mode_t)arg3);
        break;
    }

    case SYS_openat: {
        /* dirfd=arg1, path=arg2, flags=arg3, mode=arg4 */
        char path[256];
        strncpy_from_user(path, (const char*)arg2, sizeof(path)-1);
        ret = sys_open(path, (int)arg3, (mode_t)r->esi);
        break;
    }

    case SYS_creat: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)arg2);
        break;
    }

    case SYS_close:
        ret = sys_close((int)arg1);
        break;

    case SYS_read:
        ret = sys_read((int)arg1, (void*)arg2, (size_t)arg3);
        break;

    case SYS_write:
        ret = sys_write((int)arg1, (const void*)arg2, (size_t)arg3);
        break;

    case SYS_lseek:
        ret = (s32)sys_lseek((int)arg1, (off_t)(s32)arg2, (int)arg3);
        break;

    case SYS_dup:
        ret = sys_dup((int)arg1);
        break;

    case SYS_dup2:
        ret = sys_dup2((int)arg1, (int)arg2);
        break;

    case SYS_pipe:
        ret = sys_pipe((int*)arg1);
        break;

    case SYS_ioctl:
        ret = sys_ioctl((int)arg1, arg2, arg3);
        break;

    case SYS_fcntl: {
        int fd = arg1, cmd = arg2, val = arg3;
        struct file *f = current->files ? current->files->fd[fd] : NULL;
        if (!f) { ret = -EBADF; break; }
        switch (cmd) {
        case F_DUPFD: ret = sys_dup(fd); break;
        case F_GETFD:
            ret = (current->files->close_on_exec[fd/32] >> (fd%32)) & 1;
            break;
        case F_SETFD:
            if (val & FD_CLOEXEC)
                current->files->close_on_exec[fd/32] |=  (1u<<(fd%32));
            else
                current->files->close_on_exec[fd/32] &= ~(1u<<(fd%32));
            ret = 0;
            break;
        case F_GETFL: ret = f->f_flags; break;
        case F_SETFL: f->f_flags = val; ret = 0; break;
        default: ret = -EINVAL;
        }
        break;
    }

    case SYS_stat: {
        char path[256]; struct stat st;
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_stat(path, &st);
        if (!ret) copy_to_user((void*)arg2, &st, sizeof(st));
        break;
    }

    case SYS_lstat: {
        char path[256]; struct stat st;
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_lstat(path, &st);
        if (!ret) copy_to_user((void*)arg2, &st, sizeof(st));
        break;
    }

    case SYS_fstat: {
        struct stat st;
        ret = sys_fstat((int)arg1, &st);
        if (!ret) copy_to_user((void*)arg2, &st, sizeof(st));
        break;
    }

    case SYS_mkdir: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_mkdir(path, (mode_t)arg2);
        break;
    }

    case SYS_rmdir: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_rmdir(path);
        break;
    }

    case SYS_unlink: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_unlink(path);
        break;
    }

    case SYS_link: {
        char old[256], nw[256];
        strncpy_from_user(old, (const char*)arg1, sizeof(old)-1);
        strncpy_from_user(nw,  (const char*)arg2, sizeof(nw)-1);
        ret = sys_link(old, nw);
        break;
    }

    case SYS_rename: {
        char old[256], nw[256];
        strncpy_from_user(old, (const char*)arg1, sizeof(old)-1);
        strncpy_from_user(nw,  (const char*)arg2, sizeof(nw)-1);
        ret = sys_rename(old, nw);
        break;
    }

    case SYS_symlink: {
        char tgt[256], lp[256];
        strncpy_from_user(tgt, (const char*)arg1, sizeof(tgt)-1);
        strncpy_from_user(lp,  (const char*)arg2, sizeof(lp)-1);
        ret = sys_symlink(tgt, lp);
        break;
    }

    case SYS_readlink: {
        char path[256], buf[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_readlink(path, buf, MIN((size_t)arg3, sizeof(buf)-1));
        if (ret >= 0) copy_to_user((void*)arg2, buf, ret);
        break;
    }

    case SYS_chmod: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_chmod(path, (mode_t)arg2);
        break;
    }
    case SYS_fchmod: {
        struct file *f = current->files ? current->files->fd[arg1] : NULL;
        if (!f) { ret = -EBADF; break; }
        f->f_dentry->d_inode->i_mode =
            (f->f_dentry->d_inode->i_mode & S_IFMT) | (arg2 & 07777);
        ret = 0;
        break;
    }

    case SYS_chown: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_chown(path, (uid_t)arg2, (gid_t)arg3);
        break;
    }
    case SYS_lchown: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_chown(path, (uid_t)arg2, (gid_t)arg3);
        break;
    }
    case SYS_fchown: {
        struct file *f = current->files ? current->files->fd[arg1] : NULL;
        if (!f) { ret = -EBADF; break; }
        f->f_dentry->d_inode->i_uid = arg2;
        f->f_dentry->d_inode->i_gid = arg3;
        ret = 0;
        break;
    }

    case SYS_truncate: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        struct dentry *de; int r2 = vfs_lookup(path, &de, NULL);
        if (r2) { ret = r2; break; }
        if (de->d_inode->i_ops && de->d_inode->i_ops->truncate)
            ret = de->d_inode->i_ops->truncate(de->d_inode, (off_t)(s32)arg2);
        else ret = -EPERM;
        dentry_put(de);
        break;
    }
    case SYS_ftruncate: {
        struct file *f = current->files ? current->files->fd[arg1] : NULL;
        if (!f) { ret = -EBADF; break; }
        struct inode *i = f->f_dentry->d_inode;
        ret = (i->i_ops && i->i_ops->truncate) ?
              i->i_ops->truncate(i, (off_t)(s32)arg2) : -EPERM;
        break;
    }

    case SYS_chdir: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_chdir(path);
        break;
    }

    case SYS_getcwd:
        ret = sys_getcwd((char*)arg1, (size_t)arg2);
        break;

    case SYS_getdents:
    case SYS_getdents64:
        ret = sys_getdents((int)arg1, (struct dirent*)arg2, (size_t)arg3);
        break;

    case SYS_mknod: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_mknod(path, (mode_t)arg2, (dev_t)arg3);
        break;
    }

    case SYS_mount: {
        char dev[128], pt[128], fst[32];
        strncpy_from_user(dev, (const char*)arg1, sizeof(dev)-1);
        strncpy_from_user(pt,  (const char*)arg2, sizeof(pt)-1);
        strncpy_from_user(fst, (const char*)arg3, sizeof(fst)-1);
        ret = sys_mount(dev, pt, fst, arg4, (const void*)arg5);
        break;
    }

    case SYS_umount: {
        char path[256];
        strncpy_from_user(path, (const char*)arg1, sizeof(path)-1);
        ret = sys_umount(path);
        break;
    }

    /* ── memory ── */
    case SYS_brk: {
        if (!current->mm) { ret = -ENOMEM; break; }
        uptr new_brk = arg1 ? (uptr)arg1 : current->mm->brk;
        ret = mm_brk(current->mm, new_brk);
        if (!ret) ret = (s32)current->mm->brk;
        break;
    }

    case SYS_mmap: {
        struct mmap_args *ma = (struct mmap_args*)arg1;
        if (!current->mm) { ret = -ENOMEM; break; }
        u32 vm_flags = VM_USER;
        if (ma->prot & PROT_READ)  vm_flags |= VM_READ;
        if (ma->prot & PROT_WRITE) vm_flags |= VM_WRITE;
        if (ma->prot & PROT_EXEC)  vm_flags |= VM_EXEC;

        uptr addr = ma->addr;
        if (!addr) {
            addr = current->mm->brk;
            current->mm->brk += ALIGN_UP(ma->len, PAGE_SIZE);
        }
        ret = mm_mmap(current->mm, addr, ma->len, vm_flags);
        if (!ret) ret = (s32)addr;
        break;
    }

    case SYS_munmap:
        if (!current->mm) { ret = -ENOMEM; break; }
        ret = mm_munmap(current->mm, (uptr)arg1, (size_t)arg2);
        break;

    case SYS_mprotect:
        /* TODO: update PTE flags */
        ret = 0;
        break;

    /* ── time ── */
    case SYS_time: {
        extern u64 uptime_s;
        u32 t = (u32)uptime_s;
        if (arg1) copy_to_user((void*)arg1, &t, sizeof(t));
        ret = t;
        break;
    }

    case SYS_nanosleep: {
        struct timespec ts;
        copy_from_user(&ts, (void*)arg1, sizeof(ts));
        extern u64 jiffies;
        u64 end = jiffies + (u64)ts.tv_sec * 100 + ts.tv_nsec / 10000000;
        while (jiffies < end) {
            sched_yield();
            if (current->signal && current->signal->pending) {
                ret = -EINTR; goto done;
            }
        }
        ret = 0;
        break;
    }

    case SYS_clock_gettime: {
        extern u64 jiffies;
        struct timespec ts;
        ts.tv_sec  = (s32)(jiffies / 100);
        ts.tv_nsec = (s32)((jiffies % 100) * 10000000);
        copy_to_user((void*)arg2, &ts, sizeof(ts));
        ret = 0;
        break;
    }

    case SYS_times: {
        /* tms struct */
        if (arg1) memset((void*)arg1, 0, 16);
        extern u64 jiffies;
        ret = (s32)jiffies;
        break;
    }

    /* ── system info ── */
    case SYS_uname: {
        struct utsname u;
        memset(&u, 0, sizeof(u));
        strcpy(u.sysname,  "MyOS");
        strcpy(u.nodename, "myos");
        strcpy(u.release,  "1.0.0");
        strcpy(u.version,  "#1");
        strcpy(u.machine,  "i686");
        copy_to_user((void*)arg1, &u, sizeof(u));
        ret = 0;
        break;
    }

    case SYS_clone:
        /* minimal clone: treat like fork for now */
        ret = sys_fork(r);
        break;

    default:
        printk(KERN_WARN "SYSCALL: unknown %u from pid %d\n", num, current->pid);
        ret = -ENOSYS;
        break;
    }

done:
    r->eax = (u32)ret;
}
