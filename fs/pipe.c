/* fs/pipe.c – anonymous pipes */
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/mm.h>

#define PIPE_BUF   4096

struct pipe_inode_info {
    char              *buf;
    u32                head;
    u32                tail;
    u32                size;
    u32                capacity;
    spinlock_t         lock;
    wait_queue_head_t  rd_wait;
    wait_queue_head_t  wr_wait;
    int                readers;
    int                writers;
};

static ssize_t pipe_read(struct file *f, char *buf, size_t count, off_t *pos) {
    (void)pos;
    struct pipe_inode_info *p = f->f_dentry->d_inode->i_private;
    if (!p) return -EINVAL;

    while (1) {
        irq_flags_t fl;
        spin_lock_irqsave(&p->lock, &fl);
        if (p->size > 0) {
            size_t n = MIN(count, p->size);
            for (size_t i = 0; i < n; i++) {
                buf[i] = p->buf[p->tail];
                p->tail = (p->tail + 1) % p->capacity;
            }
            p->size -= n;
            spin_unlock_irqrestore(&p->lock, &fl);
            wake_up(&p->wr_wait);
            return n;
        }
        if (p->writers == 0) {
            spin_unlock_irqrestore(&p->lock, &fl);
            return 0;  /* EOF */
        }
        spin_unlock_irqrestore(&p->lock, &fl);
        int r = sleep_on_interruptible(&p->rd_wait);
        if (r) return r;
    }
}

static ssize_t pipe_write(struct file *f, const char *buf, size_t count, off_t *pos) {
    (void)pos;
    struct pipe_inode_info *p = f->f_dentry->d_inode->i_private;
    if (!p) return -EINVAL;
    if (p->readers == 0) {
        sys_kill(current->pid, SIGPIPE);
        return -EPIPE;
    }

    size_t written = 0;
    while (written < count) {
        irq_flags_t fl;
        spin_lock_irqsave(&p->lock, &fl);
        size_t avail = p->capacity - p->size;
        if (avail > 0) {
            size_t n = MIN(count - written, avail);
            for (size_t i = 0; i < n; i++) {
                p->buf[p->head] = buf[written + i];
                p->head = (p->head + 1) % p->capacity;
            }
            p->size += n;
            written += n;
            spin_unlock_irqrestore(&p->lock, &fl);
            wake_up(&p->rd_wait);
        } else {
            spin_unlock_irqrestore(&p->lock, &fl);
            int r = sleep_on_interruptible(&p->wr_wait);
            if (r) return written > 0 ? (ssize_t)written : r;
        }
    }
    return written;
}

static int pipe_read_release(struct inode *i, struct file *f) {
    (void)f;
    struct pipe_inode_info *p = i->i_private;
    p->readers--;
    wake_up(&p->wr_wait);
    if (p->readers == 0 && p->writers == 0) {
        kfree(p->buf); kfree(p);
        i->i_private = NULL;
    }
    return 0;
}

static int pipe_write_release(struct inode *i, struct file *f) {
    (void)f;
    struct pipe_inode_info *p = i->i_private;
    p->writers--;
    wake_up(&p->rd_wait);
    if (p->readers == 0 && p->writers == 0) {
        kfree(p->buf); kfree(p);
        i->i_private = NULL;
    }
    return 0;
}

static struct file_ops pipe_read_ops = {
    .read    = pipe_read,
    .release = pipe_read_release,
};

static struct file_ops pipe_write_ops = {
    .write   = pipe_write,
    .release = pipe_write_release,
};

int vfs_pipe(struct file **rend, struct file **wend) {
    struct pipe_inode_info *p = kmalloc(sizeof(*p));
    if (!p) return -ENOMEM;
    p->buf      = kmalloc(PIPE_BUF);
    if (!p->buf) { kfree(p); return -ENOMEM; }
    p->head = p->tail = p->size = 0;
    p->capacity = PIPE_BUF;
    spin_init(&p->lock);
    init_waitqueue_head(&p->rd_wait);
    init_waitqueue_head(&p->wr_wait);
    p->readers = 1;
    p->writers = 1;

    /* create a shared inode */
    struct inode *i = inode_alloc(NULL);
    if (!i) { kfree(p->buf); kfree(p); return -ENOMEM; }
    i->i_mode    = S_IFIFO | 0600;
    i->i_private = p;

    /* read dentry+file */
    struct dentry *rde = dentry_alloc("pipe:r", NULL);
    rde->d_inode = inode_get(i);
    struct file *rf = file_alloc();
    rf->f_dentry = rde;
    rf->f_ops    = &pipe_read_ops;
    rf->f_flags  = O_RDONLY;

    /* write dentry+file */
    struct dentry *wde = dentry_alloc("pipe:w", NULL);
    wde->d_inode = inode_get(i);
    struct file *wf = file_alloc();
    wf->f_dentry = wde;
    wf->f_ops    = &pipe_write_ops;
    wf->f_flags  = O_WRONLY;

    inode_put(i);  /* dentries hold refs */
    *rend = rf;
    *wend = wf;
    return 0;
}
