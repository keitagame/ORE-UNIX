/* kernel/process.c – task_struct, CFS scheduler, fork/exec/exit/waitpid */
#include <kernel/kernel.h>
#include <kernel/process.h>
#include <kernel/mm.h>
#include <kernel/vfs.h>
#include <arch/x86/arch.h>

/* ─── Globals ─────────────────────────────────────────────────────────────── */
struct task_struct *current = NULL;
struct task_struct  kernel_task;

static LIST_HEAD(task_list);
static LIST_HEAD(runqueue);
static spinlock_t task_lock = SPINLOCK_INIT;
static spinlock_t run_lock  = SPINLOCK_INIT;

static pid_t next_pid = 1;
static struct kmem_cache *task_cache;
static struct kmem_cache *signal_cache;
static struct kmem_cache *files_cache;

/* ─── PID allocator ──────────────────────────────────────────────────────── */
static pid_t alloc_pid(void) {
    irq_flags_t f;
    spin_lock_irqsave(&task_lock, &f);
    pid_t p = next_pid++;
    if (next_pid >= PID_MAX) next_pid = 2;
    spin_unlock_irqrestore(&task_lock, &f);
    return p;
}

/* ─── Task lookup ─────────────────────────────────────────────────────────── */
struct task_struct *find_task_by_pid(pid_t pid) {
    struct task_struct *t;
    irq_flags_t f;
    spin_lock_irqsave(&task_lock, &f);
    list_for_each_entry(t, &task_list, task_list) {
        if (t->pid == pid) {
            spin_unlock_irqrestore(&task_lock, &f);
            return t;
        }
    }
    spin_unlock_irqrestore(&task_lock, &f);
    return NULL;
}

/* ─── Files ───────────────────────────────────────────────────────────────── */
struct files_struct *files_create(void) {
    struct files_struct *f = kmem_cache_alloc(files_cache);
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    spin_init(&f->lock);
    f->max_fds = OPEN_MAX;
    f->fd = kcalloc(OPEN_MAX, sizeof(struct file*));
    return f;
}

struct files_struct *files_clone(struct files_struct *src) {
    struct files_struct *dst = files_create();
    if (!dst) return NULL;
    irq_flags_t fl;
    spin_lock_irqsave(&src->lock, &fl);
    for (int i = 0; i < OPEN_MAX; i++) {
        if (src->fd[i]) {
            dst->fd[i] = file_get(src->fd[i]);
        }
    }
    spin_unlock_irqrestore(&src->lock, &fl);
    return dst;
}

void files_destroy(struct files_struct *f) {
    if (!f) return;
    for (int i = 0; i < OPEN_MAX; i++) {
        if (f->fd[i]) { file_put(f->fd[i]); f->fd[i] = NULL; }
    }
    kfree(f->fd);
    kmem_cache_free(files_cache, f);
}

int files_alloc_fd(struct files_struct *f) {
    for (int i = 0; i < OPEN_MAX; i++) {
        if (!f->fd[i]) return i;
    }
    return -EMFILE;
}

void files_free_fd(struct files_struct *f, int fd) {
    if (fd >= 0 && fd < OPEN_MAX && f->fd[fd]) {
        file_put(f->fd[fd]);
        f->fd[fd] = NULL;
    }
}

/* ─── Signal struct ───────────────────────────────────────────────────────── */
static struct signal_struct *signal_create(void) {
    struct signal_struct *s = kmem_cache_alloc(signal_cache);
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    spin_init(&s->lock);
    for (int i = 0; i < MAX_SIGNALS; i++)
        s->actions[i].sa_handler = SIG_DFL;
    return s;
}

/* ─── Kernel stack allocation ─────────────────────────────────────────────── */
#define KSTACK_PAGES 2
static uptr alloc_kstack(void) {
    uptr phys = pmm_alloc_contiguous(KSTACK_PAGES);
    if (!phys) return 0;
    return (uptr)phys_to_virt(phys) + KSTACK_PAGES * PAGE_SIZE;
}
static void free_kstack(uptr top) {
    uptr virt  = top - KSTACK_PAGES * PAGE_SIZE;
    uptr phys  = virt_to_phys(virt);
    pmm_free_contiguous(phys, KSTACK_PAGES);
}

/* ─── task_create ─────────────────────────────────────────────────────────── */
struct task_struct *task_create(const char *name, void (*entry)(void), int kthread) {
    struct task_struct *t = kmem_cache_alloc(task_cache);
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));

    t->pid    = alloc_pid();
    t->ppid   = current ? current->pid : 0;
    t->pgid   = current ? current->pgid : t->pid;
    t->sid    = current ? current->sid  : t->pid;
    t->uid = t->euid = t->suid = 0;
    t->gid = t->egid = t->sgid = 0;
    t->umask  = 0022;
    t->state  = TASK_RUNNING;
    t->priority = 5;
    t->nice     = 0;
    t->time_slice = 20;   /* 20 ticks = ~200 ms */
    strncpy(t->name, name, TASK_NAME_LEN - 1);

    list_init(&t->run_list);
    list_init(&t->task_list);
    list_init(&t->children);
    list_init(&t->sibling);
    list_init(&t->wait_entry);

    t->signal = signal_create();

    if (kthread) {
        t->mm = NULL;
    } else {
        t->mm = mm_create();
        if (!t->mm) goto err;
    }

    t->kernel_stack = alloc_kstack();
    if (!t->kernel_stack) goto err;
    t->kernel_esp = t->kernel_stack;

    /* set up initial kernel context */
    u32 *sp = (u32*)t->kernel_esp;
    *--sp = (u32)entry;      /* return address (eip) */
    t->ctx.esp    = (u32)sp;
    t->ctx.eip    = (u32)entry;
    t->ctx.eflags = 0x200;   /* IF */

    t->files = files_create();
    t->cwd   = current ? dentry_get(current->cwd) : NULL;
    t->root  = current ? dentry_get(current->root) : NULL;
    t->parent = current;

    /* add to task list */
    irq_flags_t fl;
    spin_lock_irqsave(&task_lock, &fl);
    list_add_tail(&t->task_list, &task_list);
    if (current) list_add_tail(&t->sibling, &current->children);
    spin_unlock_irqrestore(&task_lock, &fl);

    return t;

err:
    if (t->mm) mm_destroy(t->mm);
    if (t->signal) kmem_cache_free(signal_cache, t->signal);
    kmem_cache_free(task_cache, t);
    return NULL;
}

void task_destroy(struct task_struct *t) {
    if (!t) return;
    if (t->mm)     mm_destroy(t->mm);
    if (t->files)  files_destroy(t->files);
    if (t->signal) kmem_cache_free(signal_cache, t->signal);
    if (t->cwd)    dentry_put(t->cwd);
    if (t->root)   dentry_put(t->root);
    free_kstack(t->kernel_stack);
    kmem_cache_free(task_cache, t);
}

/* ─── Scheduler ──────────────────────────────────────────────────────────── */
void sched_init(void) {
    task_cache   = kmem_cache_create("task_struct",    sizeof(struct task_struct));
    signal_cache = kmem_cache_create("signal_struct",  sizeof(struct signal_struct));
    files_cache  = kmem_cache_create("files_struct",   sizeof(struct files_struct));

    /* init kernel task */
    memset(&kernel_task, 0, sizeof(kernel_task));
    kernel_task.pid   = 0;
    kernel_task.state = TASK_RUNNING;
    strncpy(kernel_task.name, "kernel", TASK_NAME_LEN);
    list_init(&kernel_task.run_list);
    list_init(&kernel_task.task_list);
    list_init(&kernel_task.children);
    list_init(&kernel_task.sibling);
    list_init(&kernel_task.wait_entry);
    current = &kernel_task;
    list_add(&kernel_task.task_list, &task_list);
}

void sched_add(struct task_struct *t) {
    irq_flags_t f;
    spin_lock_irqsave(&run_lock, &f);
    t->state = TASK_RUNNING;
    list_add_tail(&t->run_list, &runqueue);
    spin_unlock_irqrestore(&run_lock, &f);
}

void sched_remove(struct task_struct *t) {
    irq_flags_t f;
    spin_lock_irqsave(&run_lock, &f);
    list_del(&t->run_list);
    spin_unlock_irqrestore(&run_lock, &f);
}

/* simple round-robin with priority weighting */
void schedule(void) {
    if (!current) return;

    irq_flags_t f;
    spin_lock_irqsave(&run_lock, &f);

    if (list_empty(&runqueue)) {
        /* idle */
        spin_unlock_irqrestore(&run_lock, &f);
        sti();
        hlt();
        return;
    }

    struct task_struct *next = list_entry(runqueue.next, struct task_struct, run_list);

    /* round-robin: move to tail */
    list_del(&next->run_list);
    list_add_tail(&next->run_list, &runqueue);

    struct task_struct *prev = current;
    if (prev == next) {
        spin_unlock_irqrestore(&run_lock, &f);
        return;
    }

    current = next;
    next->time_slice = 20;

    /* switch address space */
    if (next->mm) paging_switch(next->mm->pgdir);
    else          paging_switch(kernel_pgdir);

    /* update TSS kernel stack */
    tss_set_kernel_stack(next->kernel_stack);

    spin_unlock_irqrestore(&run_lock, &f);
    context_switch(prev, next);
}

void context_switch(struct task_struct *prev, struct task_struct *next) {
    __switch_to(&prev->ctx, &next->ctx);
    /* deliver pending signals after returning to user */
    if (current && current->signal)
        signal_deliver(current);
}

void sched_tick(void) {
    if (!current || current == &kernel_task) return;
    current->runtime_ns += 10000000ULL;  /* ~10 ms */
    if (current->time_slice > 0) current->time_slice--;
    if (current->time_slice == 0) {
        current->time_slice = 20;
        /* re-add to tail of runqueue (already there from sched_add) */
        schedule();
    }
}

void sched_yield(void) {
    if (current && current->time_slice > 0)
        current->time_slice = 0;
    schedule();
}

/* ─── Sleep / wake ────────────────────────────────────────────────────────── */
void init_waitqueue_head(wait_queue_head_t *q) {
    spin_init(&q->lock);
    list_init(&q->task_list);
}

void sleep_on(wait_queue_head_t *q) {
    irq_flags_t f;
    spin_lock_irqsave(&q->lock, &f);
    current->state = TASK_SLEEPING;
    list_add_tail(&current->wait_entry, &q->task_list);
    sched_remove(current);
    spin_unlock_irqrestore(&q->lock, &f);
    schedule();
}

int sleep_on_interruptible(wait_queue_head_t *q) {
    sleep_on(q);
    /* check for pending signals */
    if (current->signal && current->signal->pending)
        return -EINTR;
    return 0;
}

void wake_up(wait_queue_head_t *q) {
    irq_flags_t f;
    spin_lock_irqsave(&q->lock, &f);
    if (!list_empty(&q->task_list)) {
        struct task_struct *t = list_entry(q->task_list.next,
                                            struct task_struct, wait_entry);
        list_del(&t->wait_entry);
        t->state = TASK_RUNNING;
        sched_add(t);
    }
    spin_unlock_irqrestore(&q->lock, &f);
}

void wake_up_all(wait_queue_head_t *q) {
    irq_flags_t f;
    spin_lock_irqsave(&q->lock, &f);
    struct list_head *pos, *n;
    list_for_each_safe(pos, n, &q->task_list) {
        struct task_struct *t = list_entry(pos, struct task_struct, wait_entry);
        list_del(&t->wait_entry);
        t->state = TASK_RUNNING;
        sched_add(t);
    }
    spin_unlock_irqrestore(&q->lock, &f);
}

void wake_up_process(struct task_struct *t) {
    t->state = TASK_RUNNING;
    sched_add(t);
}

/* ─── fork() ──────────────────────────────────────────────────────────────── */
pid_t sys_fork(struct regs *r) {
    struct task_struct *child = kmem_cache_alloc(task_cache);
    if (!child) return -ENOMEM;
    *child = *current;   /* copy everything */

    child->pid    = alloc_pid();
    child->ppid   = current->pid;
    child->state  = TASK_RUNNING;
    child->parent = current;
    list_init(&child->run_list);
    list_init(&child->task_list);
    list_init(&child->children);
    list_init(&child->sibling);
    list_init(&child->wait_entry);

    /* clone address space */
    child->mm = mm_clone(current->mm);
    if (!child->mm) goto err;

    /* clone files */
    child->files = files_clone(current->files);
    if (!child->files) goto err;

    /* clone signals */
    child->signal = signal_create();
    if (!child->signal) goto err;
    *child->signal = *current->signal;

    /* new kernel stack */
    child->kernel_stack = alloc_kstack();
    if (!child->kernel_stack) goto err;
    child->kernel_esp = child->kernel_stack;

    /* child returns 0 from fork */
    child->ctx = current->ctx;
    /* copy saved user register state into child kernel stack */
    uptr kstack_base = child->kernel_stack - KSTACK_PAGES * PAGE_SIZE;
    uptr parent_frame_offset = (uptr)r - (current->kernel_stack - KSTACK_PAGES * PAGE_SIZE);
    struct regs *child_regs = (struct regs *)(kstack_base + parent_frame_offset);
    *child_regs = *r;
    child_regs->eax = 0;  /* child returns 0 */
    child->ctx.esp  = (u32)child_regs;
    child->ctx.eip  = (u32)&&fork_child_return;  /* dummy, handled by context */

    /* actually we set eip to a trampoline */
    extern void fork_return_trampoline(void);
    child->ctx.eip = (u32)fork_return_trampoline;

    if (current->cwd)  dentry_get(current->cwd);
    if (current->root) dentry_get(current->root);

    irq_flags_t fl;
    spin_lock_irqsave(&task_lock, &fl);
    list_add_tail(&child->task_list, &task_list);
    list_add_tail(&child->sibling, &current->children);
    spin_unlock_irqrestore(&task_lock, &fl);

    sched_add(child);
    return child->pid;

fork_child_return:
    return child->pid;  /* parent path */

err:
    if (child->mm)     mm_destroy(child->mm);
    if (child->files)  files_destroy(child->files);
    if (child->signal) kmem_cache_free(signal_cache, child->signal);
    if (child->kernel_stack) free_kstack(child->kernel_stack);
    kmem_cache_free(task_cache, child);
    return -ENOMEM;
}

/* Trampoline for fork child: restores regs and returns to user */
__attribute__((naked)) void fork_return_trampoline(void) {
    __asm__ volatile(
        "mov %%esp, %%esp\n\t"
        "jmp restore_regs\n\t"
        ::: "memory");
}

/* ─── exit() ──────────────────────────────────────────────────────────────── */
NORETURN void sys_exit(int code) {
    current->state     = TASK_ZOMBIE;
    current->exit_code = code;

    /* close files */
    if (current->files) { files_destroy(current->files); current->files = NULL; }
    if (current->mm)    { mm_destroy(current->mm);       current->mm    = NULL; }

    /* notify parent */
    if (current->parent) {
        sys_kill(current->parent->pid, SIGCHLD);
        wake_up_process(current->parent);
    }

    /* reparent children to init */
    struct task_struct *init = find_task_by_pid(1);
    struct list_head *pos;
    list_for_each(pos, &current->children) {
        struct task_struct *child = list_entry(pos, struct task_struct, sibling);
        child->parent = init;
        child->ppid   = 1;
    }

    sched_remove(current);
    schedule();
    BUG(); /* never reached */
}

/* ─── waitpid() ───────────────────────────────────────────────────────────── */
pid_t sys_waitpid(pid_t pid, int *status, int options) {
    (void)options;
    while (1) {
        struct task_struct *child = NULL;
        irq_flags_t f;
        spin_lock_irqsave(&task_lock, &f);
        struct list_head *pos;
        list_for_each(pos, &current->children) {
            struct task_struct *t = list_entry(pos, struct task_struct, sibling);
            if (pid == -1 || t->pid == pid) {
                if (t->state == TASK_ZOMBIE) { child = t; break; }
            }
        }
        spin_unlock_irqrestore(&task_lock, &f);

        if (child) {
            pid_t cpid = child->pid;
            if (status) *status = (child->exit_code & 0xFF) << 8;
            /* remove from task list */
            spin_lock_irqsave(&task_lock, &f);
            list_del(&child->task_list);
            list_del(&child->sibling);
            spin_unlock_irqrestore(&task_lock, &f);
            task_destroy(child);
            return cpid;
        }

        /* no zombie child yet */
        if (options & 1 /* WNOHANG */) return 0;

        /* sleep until SIGCHLD */
        static wait_queue_head_t wait_q;
        static bool wait_q_init = false;
        if (!wait_q_init) { init_waitqueue_head(&wait_q); wait_q_init = true; }
        sleep_on_interruptible(&wait_q);
    }
}

/* ─── getpid / misc ───────────────────────────────────────────────────────── */
pid_t sys_getpid(void)  { return current->pid;  }
pid_t sys_getppid(void) { return current->ppid; }

int sys_setpgid(pid_t pid, pid_t pgid) {
    struct task_struct *t = pid ? find_task_by_pid(pid) : current;
    if (!t) return -ESRCH;
    t->pgid = pgid ? pgid : t->pid;
    return 0;
}

pid_t sys_getpgrp(void) { return current->pgid; }

pid_t sys_setsid(void) {
    current->sid  = current->pid;
    current->pgid = current->pid;
    return current->sid;
}

/* ─── Signal ──────────────────────────────────────────────────────────────── */
int sys_kill(pid_t pid, int sig) {
    if (sig <= 0 || sig >= MAX_SIGNALS) return -EINVAL;
    struct task_struct *t = find_task_by_pid(pid);
    if (!t) return -ESRCH;
    irq_flags_t f;
    spin_lock_irqsave(&t->signal->lock, &f);
    t->signal->pending |= (1ULL << sig);
    spin_unlock_irqrestore(&t->signal->lock, &f);
    if (t->state == TASK_SLEEPING) wake_up_process(t);
    return 0;
}

int sys_sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (sig <= 0 || sig >= MAX_SIGNALS) return -EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return -EINVAL;
    irq_flags_t f;
    spin_lock_irqsave(&current->signal->lock, &f);
    if (old) *old = current->signal->actions[sig];
    if (act) current->signal->actions[sig] = *act;
    spin_unlock_irqrestore(&current->signal->lock, &f);
    return 0;
}

void signal_deliver(struct task_struct *t) {
    if (!t->signal || !t->signal->pending) return;

    irq_flags_t f;
    spin_lock_irqsave(&t->signal->lock, &f);
    u64 pending = t->signal->pending;
    t->signal->pending = 0;
    spin_unlock_irqrestore(&t->signal->lock, &f);

    for (int sig = 1; sig < MAX_SIGNALS && pending; sig++) {
        if (!(pending & (1ULL << sig))) continue;
        pending &= ~(1ULL << sig);

        sighandler_t handler = t->signal->actions[sig].sa_handler;
        if (handler == SIG_IGN) continue;
        if (handler == SIG_DFL) {
            switch (sig) {
            case SIGKILL: case SIGSEGV: case SIGBUS:
            case SIGFPE:  case SIGILL:  case SIGABRT:
                sys_exit(128 + sig);
                break;
            case SIGCHLD: case SIGCONT:
                break;
            case SIGSTOP: case SIGTSTP:
                t->state = TASK_STOPPED;
                sched_remove(t);
                break;
            case SIGTERM: case SIGHUP: case SIGINT:
            case SIGPIPE: case SIGALRM: case SIGUSR1: case SIGUSR2:
                sys_exit(128 + sig);
                break;
            }
        } else {
            /* call user handler – set up user stack frame */
            /* TODO: full signal frame setup (sigreturn) */
            handler(sig);
        }
    }
}
