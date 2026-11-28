/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 */

/*
 * kthread.h - kernel-only threads (S21).
 *
 * A kernel thread is an execution carrier that never enters user mode.  It
 * has its own kernel stack and runs the scheduler's cooperative context
 * switch.  Softirq processing and page reclaim use these threads, so those
 * jobs no longer run on whatever user thread happened to call into the
 * kernel.
 * 内核线程是永不进入用户态的执行载体：它有自己的内核栈，并走调度器的
 * 协作式上下文切换；软中断与页回收在这些线程上运行，不再借用用户线程的栈。
 */
#ifndef _KTHREAD_H_
#define _KTHREAD_H_

#include "sys/platform.h"
#include "sys/smp.h"		/* MAXCPU + cpu_id() for the per-CPU curk macro */

/*
 * KLT pool (S407/S410/S411): the TCB table capacity is now a *runtime*
 * value, nkthread, derived from physmem by mem_plan_init() (one slot per
 * 4 MB of RAM: 32 at 128 MB, 1024 at 4 GB).  NKTHREAD_MAX is only the
 * storage/VA ceiling used for static bitmaps; the pool itself is sized at
 * boot from nkthread.
 *
 * Each slot owns a fixed 16 KB kernel stack (S411: was 8 KB) mapped in the
 * KSTACK_VA window, so the window is nkthread * 16 KB and never more than
 * NKTHREAD_MAX * 16 KB = 16 MB:
 *
 *	0xFC000000 .. 0xFD000000	1024 x 16 KB (four PDEs 0x3f0..0x3f3)
 *
 * The window moved down from 0xFF000000 because 16 MB does not fit before
 * the recursive page-table slot at 0xFFC00000.  0xFC000000 sits below the
 * kernel image at 0xFE000000 and is copied into every user page directory
 * (see user_pd_create).
 * 每个槽位 16KB 内核栈固定在 KSTACK_VA 窗口；1024 槽即 16MB（四个 PDE）。
 * 从 0xFF000000 下移是因为 16MB 塞不进 0xFFC00000 的递归页表槽之前；
 * 0xFC000000 在内核镜像 0xFE000000 之下，且会被拷进每个用户页目录。
 */
#define	NKTHREAD_MAX	1024	/* storage ceiling, not the runtime count */
extern int	nkthread;	/* runtime TCB slots, set by mem_plan_init() */


/*
 * TID namespace partition (pid may reach NPROC now):
 *   main thread    tid == pid                     (1 .. NPROC+1)
 *   extra user tid = TID_USER_BASE + pid*TID_STRIDE + n
 *   kernel thread  tid = TID_KERN_BASE + slot      (high, never a pid)
 * kthread_find()/kthread_user_find() key on tid, so ranges must not overlap.
 */
#define	TID_USER_BASE	1000
#define	TID_STRIDE	16
#define	TID_KERN_BASE	0x40000000

#define	KT_FREE		0
#define	KT_RUN		1
#define	KT_BLOCK	2
#define	KT_ZOMBIE	3

/* priority scale: 0 = LOWEST, 19 = HIGHEST (20 levels) */
#define	NPRIO		20
#define	PRI_LOWEST	0
#define	PRI_IDLE	2
#define	PRI_BATCH	5		/* CPU-bound baseline (low) */
#define	PRI_NORMAL	10
#define	PRI_INTERACTIVE	14	/* interactive / I/O-bound baseline */
#define	PRI_HIGH	17
#define	PRI_HIGHEST	19

/* quantum (ticks) -- fixed base; CPU-bound threads get a longer one */
#define	QUANTUM_BASE	4
#define	QUANTUM_CPU	8
#define	QUANTUM_IO	2

/* dynamic priority tuning */
#define	PRI_BOOST_IO	6	/* boost on wake from sleep/I/O */
#define	PRI_BOOST_TICKS	3	/* S424: how long that boost lasts (ticks) */
#define	ESTCPU_MAX_T	255	/* S424: per-thread CPU charge ceiling */
#define	PRI_BOOST_STARVE	8	/* max anti-starvation boost */
#define	STARVE_THRESHOLD	40	/* ticks before anti-starvation kicks in */
#define	CPU_BOUND_THRESHOLD	3	/* consecutive CPU quanta => CPU-bound */

#define	KSTACK_VA	0xFC000000	/* S411: 16 MB, 1024 x 16 KB */
#define	KTHREAD_TABLE_VA 0xFEF00000	/* S410: TCB pool window */
#define	KSTACK_SIZE	16384

struct process;

struct kthread {
	int		t_state;
	int		t_tid;
	const char	*t_name;
	void		(*t_entry)(void *);	/* kernel threads only */
	void		*t_arg;
	unsigned	t_ksp;		/* saved kernel esp while switched out */
	unsigned	t_stack_top;
	void		*t_stack_va;
	int		t_used;
	/* KLT: threads belonging to a process form a list via t_next;
	 * t_proc points back to the owning kernel process object. */
	struct process	*t_proc;
	struct kthread	*t_next;	/* next thread in proc->uk_p_threads list */
	struct kthread	*t_rqnext;	/* next TCB in the kernel-thread run queue */
	int		t_onrq;		/* 1 if currently linked on that queue */
	/* S428: per-CPU user run queue link.  A user TCB sits on exactly one
	 * CPU's queue while it is KT_RUN; t_runq_cpu is that CPU, or -1.  It
	 * is separate from t_rqnext above because kernel threads (t_proc==0)
	 * queue globally, and a TCB is only ever one of the two. */
	struct kthread	*t_runq;
	int		t_runq_cpu;
	/* Scheduling: the quantum is per-thread, fairness is per-process. */
	/* ---- scheduling: dynamic priority, quantum, workload class ---- */
	int		t_prio;		/* base priority 0(low)..19(high) */
	int		t_curprio;	/* effective priority (dynamic boosts/decay) */
	/* priority inheritance: a low-prio holder temporarily runs at a
	 * high-prio waiter's level, then restores its original priority. */
	int		t_base_prio;	/* original priority before inheritance */
	int		t_inherit_prio;	/* donated priority (0 = none) */
	int		t_blocked_on;	/* tid of thread holding the wanted lock, -1 none */
	/* preemption control: >0 means in a kernel critical section; the
	 * thread cannot be preempted until it returns to zero.  User mode is
	 * always preemptable; preemption happens at safe points only. */
	int		t_preempt_cnt;
	int		t_ticks;	/* ticks consumed in current quantum */
	int		t_quantum;	/* quantum for this slice (workload-tuned) */
	int		t_sleep_ticks;	/* ticks slept: I/O-bound detector */
	int		t_starve;	/* ticks waiting: anti-starvation boost */
	int		t_cpu_burst;	/* consecutive CPU ticks: CPU-bound detector */
	/* S424: remaining ticks of the "woken from a real sleep, treat as
	 * interactive" boost.  The old test was t_sleep_ticks > 0, but that
	 * counter is never cleared, so every thread that had ever blocked
	 * kept the boost forever and it stopped discriminating anything.
	 * This one decays per tick so the boost only covers the wakeup. */
	int		t_ioboost;
	/* S424: per-thread CPU charge for the BSD interaction penalty.
	 * uk_estcpu is per process, so with it alone an 8-thread process
	 * collected 8x the charge of a single-threaded one and every thread
	 * shared the same penalty; charging the TCB makes the scheduler's
	 * unit (a thread) match its accounting unit.  uk_estcpu keeps being
	 * maintained for /dev/ps compatibility. */
	int		t_estcpu;
	int		t_lastcpu;	/* affinity: last CPU this thread ran on */
	/* S424: where the thread *prefers* to run (affinity hint only).
	 * Ownership (t_oncpu) is a kernel-stack mutex, not affinity - it is
	 * released once the CPU has switched off the stack, so an idle CPU
	 * may pick the thread up. */
	int		t_want_cpu;
	/* S413: the CPU that currently owns this TCB, -1 when unowned.  A TCB
	 * is only ever resumed by its owner, which is what stops two CPUs
	 * from running the same thread (and sharing its kernel stack). */
	int		t_oncpu;
	/* S413: slot was released by the thread still running on it.  The
	 * slot may not be handed out again until the CPU leaves that stack. */
	int		t_reap_cpu;
	void		*t_wchan;	/* non-NULL => blocked on this channel */
	struct kthread	*t_wqnext;	/* S200: next TCB on a sync wait queue */
	struct sysregs	t_regs;
	int		t_wait_pid;
	unsigned	t_wait_status;
	int		t_wait_options;
	int		t_pipeidx;
	int		t_piperw;
	unsigned	t_pipeat;
	unsigned	t_pipelen;
	int		t_exitcode;
	int		t_join_tid;	/* thread waiting in thread_join */
	int		t_detached;
	unsigned	t_sigmask;	/* thread-directed blocked mask */
	unsigned	t_sig;		/* thread-directed pending signals */
	/* os.md mechanism 4: scheduling/IPL properties */
	int		t_base_ipl;	/* preemption sensitivity (0..15) */
	int		t_eff_ipl;	/* base + priority inheritance */
	int		t_preemptable;	/* may its resources be taken over? */
	int		t_boosted;	/* inheriting a higher level */
};

/*
 * S414: "current kernel thread" is per-CPU.  The macro expands to an lvalue
 * (curk_percpu[cpu_id()]) so the existing `curk = t;` assignments keep
 * working, while two CPUs no longer share one pointer.
 * S414：当前内核线程按核分开。宏展开为左值，原有的 `curk = t;` 赋值无需改动，
 * 但两个核不再共用一个指针。
 */
extern struct kthread *curk_percpu[MAXCPU];
#define	curk		(curk_percpu[cpu_id()])
struct kthread *kthread_current(void);
/* TCB pool lives in a VA window, not in .bss (see kthread.c). */
extern struct kthread *kthreads;
void kthread_table_init(void);
void kthread_prime_stacks(void);

void	kthread_init(void);
struct kthread *kthread_spawn(const char *name, void (*entry)(void *),
	    void *arg);
struct kthread *kthread_user_spawn(struct process *p, int tid);
struct kthread *kthread_user_find(int tid);
void	kthread_user_free(struct kthread *t);
/* S413: CPU ownership of a TCB (atomic claim, see kthread.c). */
int	kthread_claim_cpu(struct kthread *t, int id);
void	kthread_release_cpu(struct kthread *t);
int	kthread_owned_by_other(struct kthread *t, int id);
void	kthread_reap(struct kthread *leaving);
int	kstack_overflowed(struct kthread *t);
void	kthread_exit(void);
void	kthread_block(void);
int	kthread_report_invisible(void);
void	kthread_dump_all(const char *why);
void	kthread_block_prepare(void);
void	kthread_block_commit(void);
void	kthread_wake(struct kthread *t);
void	kthread_poll(void);	/* run one runnable kernel thread */
void	kthread_run(void);	/* run until none are runnable */
void	kthread_print(void);
int	kthread_count(void);
int	kthread_runnable(void);
unsigned kthread_selftest(void);
extern unsigned kthread_switches;

/* context switch: save current esp in *old, continue on new stack */
void	kswitch(unsigned *old, unsigned new);

/* interaction scoring hook provided by the process layer (usermode.c):
 * returns a penalty 0..N derived from BSD estcpu + p_nice. */
int	sched_interaction_penalty(struct kthread *t);

/* dynamic priority / workload scheduling */
void	sched_prio_init(struct kthread *t, int base);
int	sched_effective_prio(struct kthread *t);
void	sched_on_wake(struct kthread *t);	/* boost on I/O/sleep wake */
void	sched_on_block(struct kthread *t);	/* record sleep for classification */
void	sched_on_tick(struct kthread *t);	/* update cpu/starve counters */
int	sched_is_cpu_bound(struct kthread *t);
/* priority inheritance: waiter donates its priority to the lock holder */
void	prio_inherit(struct kthread *waiter, struct kthread *holder);
void	prio_restore(struct kthread *holder);
int	sched_prio_selftest(void);
/* preemption control: kernel critical sections disable preemption */
void	preempt_disable(void);
void	preempt_enable(void);	/* may schedule when count drops to 0 */
int	preempt_is_disabled(void);
/* current user thread's TCB (provided by usermode.c), or 0 if none */
struct kthread *cur_proc_kthread(void);
/*
 * Take a TCB off whatever run queue it is on (provided by usermode.c).
 * Called from the free path: a slot that is still chained into a run queue
 * gets bzeroed when it is reused, and the bzeroed t_runq pointer truncates
 * the list - every thread behind it becomes unreachable and its CPU goes
 * idle with the queue looking non-empty.
 * 中文：把 TCB 从它所在的运行队列上摘下来（由 usermode.c 提供）。释放路径必须
 * 调用：仍挂在队列上的槽位一旦被复用就会被 bzero，t_runq 清零会把链表截断，
 * 其后的所有线程都取不到，而对应 CPU 会空转。
 */
void	kthread_rq_detach(struct kthread *t);

/* os.md mechanism 4: per-thread IPL policy + priority inheritance */
void	thread_ipl_set(struct kthread *t, int base_ipl, int preemptable);
int	thread_ipl_effective(struct kthread *t);
int	thread_ipl_boost(struct kthread *t, int ipl);
void	thread_ipl_unboost(struct kthread *t);
struct kthread *kthread_find(int tid);
int	kthread_ipl_selftest(void);
extern unsigned ipl_boosts;
extern unsigned ipl_donations;

/* softirq */
#define	SOFT_RECLAIM	0
#define	SOFT_INPUT	1
#define	SOFT_NET	2
#define	SOFT_TEST	3
#define	NSOFT		4

void	softirq_init(void);
void	softint_schedule(int id);
void	softint_register(int id, void (*fn)(void));
void	softirq_print(void);
unsigned softirq_count(void);
int	softirq_selftest(void);

/* page cache reclaim hook (pagecache.c) */
int	pagecache_reclaim(unsigned n);

#endif
