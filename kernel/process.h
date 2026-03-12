#ifndef _PROCESS_H
#define _PROCESS_H

#include "kernel.h"
#include "mm.h"

/* ─── Process states ─────────────────────────────────────────────────────── */
#define TASK_RUNNING    0   /* on runqueue or currently executing */
#define TASK_SLEEPING   1   /* waiting for event (interruptible) */
#define TASK_STOPPED    2   /* stopped by signal */
#define TASK_ZOMBIE     3   /* exited, waiting for parent wait() */
#define TASK_DEAD       4   /* fully cleaned up */

/* ─── Limits ─────────────────────────────────────────────────────────────── */
#define PID_MAX         32768
#define OPEN_MAX        1024
#define MAX_ARGS        256
#define MAX_ENV         256
#define TASK_NAME_LEN   64
#define MAX_SIGNALS     64

/* ─── File descriptor table ──────────────────────────────────────────────── */
struct file;
struct files_struct {
    spinlock_t   lock;
    int          max_fds;
    struct file **fd;          /* array of open files */
    u32          close_on_exec[OPEN_MAX/32];
};
struct files_struct *files_create(void);
struct files_struct *files_clone(struct files_struct *f);
void                 files_destroy(struct files_struct *f);
int                  files_alloc_fd(struct files_struct *f);
void                 files_free_fd(struct files_struct *f, int fd);

/* ─── Signal handling ────────────────────────────────────────────────────── */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGFPE    8
#define SIGKILL   9
#define SIGSEGV   11
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGUSR1   10
#define SIGUSR2   12

#define SIG_DFL   ((void*)0)
#define SIG_IGN   ((void*)1)
#define SIG_ERR   ((void*)-1)

typedef void (*sighandler_t)(int);

struct sigaction {
    sighandler_t sa_handler;
    u32          sa_flags;
    u64          sa_mask;
};

struct signal_struct {
    spinlock_t       lock;
    u64              pending;          /* bitmask of pending signals */
    struct sigaction actions[MAX_SIGNALS];
};

/* ─── CPU context ────────────────────────────────────────────────────────── */
struct context {
    u32 edi, esi, ebx, ebp, esp, eip;
    u32 eflags;
};

/* ─── task_struct ─────────────────────────────────────────────────────────── */
struct task_struct {
    /* identity */
    pid_t             pid;
    pid_t             ppid;
    pid_t             pgid;
    pid_t             sid;
    char              name[TASK_NAME_LEN];

    /* state */
    int               state;
    int               exit_code;
    u32               flags;

    /* credentials */
    uid_t             uid, euid, suid;
    gid_t             gid, egid, sgid;

    /* CPU context (kernel) */
    struct context    ctx;

    /* memory */
    struct mm_struct *mm;
    uptr              kernel_stack;   /* phys addr of kernel stack page */
    uptr              kernel_esp;     /* top of kernel stack */

    /* files */
    struct files_struct *files;
    struct dentry       *cwd;        /* current working directory */
    struct dentry       *root;       /* root directory */
    mode_t               umask;

    /* signals */
    struct signal_struct *signal;

    /* scheduler */
    int               priority;      /* 0 (highest) .. 19 (lowest) */
    int               nice;
    u64               runtime_ns;    /* total CPU time */
    u64               vruntime;      /* virtual runtime for CFS */
    u32               time_slice;    /* ticks remaining */
    struct list_head  run_list;      /* runqueue link */

    /* family links */
    struct task_struct *parent;
    struct list_head    children;
    struct list_head    sibling;

    /* global task list */
    struct list_head    task_list;

    /* wait queue support */
    struct list_head    wait_entry;

    /* ELF aux data */
    uptr              entry;         /* user entry point */
};

/* ─── Scheduler ──────────────────────────────────────────────────────────── */
extern struct task_struct *current;
extern struct task_struct  kernel_task;

void sched_init(void);
void schedule(void);
void sched_add(struct task_struct *t);
void sched_remove(struct task_struct *t);
void sched_tick(void);               /* called from timer IRQ */
void sched_yield(void);

/* context switch (arch) */
void context_switch(struct task_struct *prev, struct task_struct *next);
void __switch_to(struct context *old_ctx, struct context *new_ctx);

/* ─── Process lifecycle ──────────────────────────────────────────────────── */
struct task_struct *task_create(const char *name, void (*entry)(void), int kthread);
void                task_destroy(struct task_struct *t);

pid_t  sys_fork(struct regs *r);
int    sys_exec(const char *path, char *const argv[], char *const envp[]);
NORETURN void sys_exit(int code);
pid_t  sys_waitpid(pid_t pid, int *status, int options);
pid_t  sys_getpid(void);
pid_t  sys_getppid(void);
int    sys_setpgid(pid_t pid, pid_t pgid);
pid_t  sys_getpgrp(void);
pid_t  sys_setsid(void);

/* sleep / wake */
void   sleep_on(wait_queue_head_t *q);
int    sleep_on_interruptible(wait_queue_head_t *q);
void   wake_up_process(struct task_struct *t);

/* signal dispatch */
void   signal_deliver(struct task_struct *t);
int    sys_kill(pid_t pid, int sig);
int    sys_sigaction(int sig, const struct sigaction *act, struct sigaction *old);

/* pid lookup */
struct task_struct *find_task_by_pid(pid_t pid);

/* init task */
void init_process(void);

#endif /* _PROCESS_H */
