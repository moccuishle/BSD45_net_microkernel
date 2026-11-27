/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * smp.h - symmetric multiprocessing support for BSD45 i386.
 *
 * Layout:
 *   - CPU 0 is the BSP (boot processor), started by stage1/stage2.
 *   - APs are started via the MP startup protocol: INIT-SIPI-SIPI to the
 *     Local APIC, with a 4K trampoline page in low physical memory.
 *   - per-CPU data is addressed through the GS segment (flat, base = &cpu0).
 *   - cross-CPU coordination uses IPI + a simple spinlock.
 */
#ifndef _SMP_H_
#define _SMP_H_

/*
 * S413: MAXCPU is the compile-time *ceiling* on CPUs, not the machine size.
 * The online count still comes from CPUID at boot, so a 2-core guest uses 2
 * of these slots and a 16-core machine uses 16.  Raising this number only
 * costs MAXCPU * (TSS + scheduler state + 16 KB AP stack); it does not change
 * the scheduling policy.
 * S413：MAXCPU 是编译期上限，不是机器规模。在线核数仍由启动时的 CPUID 决定，
 * 双核 guest 只用 2 个槽，16 核机器用 16 个。抬高这个数只多花
 * MAXCPU *（TSS + 调度状态 + 16KB AP 栈），不改变调度策略。
 */
#define	MAXCPU		16
#define	CPU_TRAMPOLINE_PA	0x7000	/* low phys page for AP trampoline */

/*
 * S413: per-CPU identity without a memory reference.
 *
 * Every CPU loads its own Task State Segment and keeps it in the task
 * register, so `str` returns a selector that is unique per CPU.  The GDT
 * layout (see machdep.c) puts CPU 0's TSS at selector 6 and CPU n at
 * 6+n, so the selector alone names the CPU.  This replaces the global
 * "current CPU id" variable an SMP kernel cannot have, and it is valid
 * even inside an interrupt handler that another CPU interrupted.
 * S413：用 TSS 选择子标识当前 CPU。每个 CPU 在任务寄存器里放自己的 TSS，
 * `str` 读回的选择子是唯一的；GDT 中 CPU0 占 6 号、CPU n 占 6+n，所以选择子
 * 就是 CPU 号。这替代了 SMP 内核不能存在的"全局当前 CPU 变量"。
 */
#define	CPU_TSS_SEL_BASE	6	/* must match GPROC0_SEL in machdep.c */

#ifndef ASSEMBLER
/*
 * S432: cpu_id() used to be a static __inline here that executed `str`
 * inline.  The instruction now lives in arch/i386/smp.c (see asmble.md:
 * no assembly outside arch/), so this header only carries the prototype.
 * The call is one `call`/`ret`; every caller in the tree is on a path that
 * already touches memory, so the extra indirection does not show up.
 * S432：cpu_id() 原先是本头文件里的 static __inline，直接内联执行 `str`。
 * 该指令现已移入 arch/i386/smp.c（asmble.md：arch/ 之外不得有汇编），
 * 本头文件只保留原型。
 */
int	cpu_id(void);
struct cpu_info {
	int		ci_id;		/* 0 = BSP */
	int		ci_active;	/* AP finished basic init */
	unsigned	ci_apicid;	/* hardware local APIC ID */
	unsigned	ci_lapic;	/* local APIC MMIO base (VA) */
	unsigned	ci_idle_tss;	/* per-CPU TSS selector */
	void		*ci_curthread;	/* currently running TCB on this CPU */
	unsigned	ci_ticks;
};

extern struct cpu_info cpus[MAXCPU];
extern int ncpu;			/* number of active CPUs */

/*
 * S412: topology discovered from CPUID (not CMOS guesses).
 *   logical  = schedulable logical processors (includes SMT siblings)
 *   cores    = physical cores
 *   smt      = logical / cores
 */
extern int cpu_logical, cpu_cores, cpu_smt, cpu_htt;
extern char cpu_vendor[13];
int	cpu_topology_detect(void);
void	cpuinfo_print(void);
/* S415: SMT placement hints (APIC-id based). */
int	smt_phys_core(int apic_id);	/* physical core of a logical CPU */
int	smt_sibling(int apic_id);	/* other logical thread, or -1 if no SMT */

/* boot / AP startup */
void	smp_init(void);		/* called by BSP after pmap+GDT ready */
void	ap_start(int id);	/* C entry point for each AP */
void	smp_start_aps(int count); /* BSP: INIT/SIPI the APs */
int	mp_detect(void);	/* parse MP table / ACPI, return #CPUs */

/* AP trampoline symbols (from ap_tramp.s) */
extern char ap_tramp_start[], ap_tramp_end[];
extern unsigned ap_gdtr[], ap_pd[], ap_stack[], ap_cpuid[];

/* local APIC */
void	lapic_init(void);
void	lapic_eoi(void);
void	lapic_timer_init(void);
unsigned lapic_id(void);
void	lapic_send_ipi(int target, int vector);
#define	IPI_TLB_SHOOTDOWN	0x40
#define	IPI_RESCHEDULE		0x41
#define	IPI_STOP		0x42

/* spinlock (test-and-set) */
struct spinlock { volatile int locked; };
void	spin_init(struct spinlock *lk);
void	spin_lock(struct spinlock *lk);
void	spin_unlock(struct spinlock *lk);

/*
 * S413: atomic primitives.
 *
 * The GCC __sync_* builtins expand to libatomic calls on this -march=i386
 * target ("undefined reference to __sync_fetch_and_add_4"), which the kernel
 * does not link, so they are hand-written.  S432 moved those from inline
 * assembly in this header to arch/i386/atomic.c; the instruction sequences
 * (lock cmpxchg/xadd, xchg) are 486+ features this port already relies on
 * elsewhere (CR0.WP, invlpg).
 * S413：GCC 的 __sync_* 在 -march=i386 下会变成 libatomic 调用（链接时
 * "undefined reference to __sync_fetch_and_add_4"），因此手写。S432 把原先
 * 写在本头文件里的内联汇编移到 arch/i386/atomic.c，只留函数声明。
 *
 * Every translation unit that needs atomics pulls them in through this
 * header, so the ARCH gate lives here rather than in each caller.
 * 所有需要原子的编译单元都通过本头文件获得，因此 ARCH 选择放在这里。
 */
#if defined(ARCH_i386)
#include "arch/i386/atomic.h"
#else
#error "smp.h: no atomic.h for this ARCH; add one under kernel/arch/<arch>/"
#endif

/* per-CPU current thread via GS: we store a small header at GS:0 */
struct kthread *curthread_cpu(void);

/* run queue helpers (per-CPU) */
void	smp_balance(void);	/* migrate threads from busy to idle CPU */

/* S413: per-CPU TSS.  CPU 0 reuses proc0's u-area TSS (built by init386);
 * APs get a descriptor at GPROC0_SEL + id pointing at their own TSS. */
struct i386tss;
struct i386tss *cpu_tss(int id);	/* 0 when id is out of range */
unsigned cpu_tss_sel(int id);		/* ready-to-ltr selector */
int	cpu_tss_install(int id, unsigned stack_top);

/* S413: per-CPU scheduling (implemented in kernel/sys/usermode.c) */
void	sched_percpu_init(int id, unsigned stack_top);
void	ap_sched(void);			/* AP idle/scheduler loop, no return */
void	smp_need_resched(void);		/* set this CPU's reschedule flag */
int	smp_resched_pending(void);
void	smp_resched_cpu(int id);	/* IPI one CPU to re-run the scheduler */
unsigned sched_cpu_switches(int id);	/* diagnostics: switches on CPU id */

/*
 * S413: big kernel lock.  User code runs on all CPUs in parallel, but the
 * kernel (proc table, VFS, pipes, address spaces) is not yet fine-grained
 * locked, so syscalls and page faults serialize on one lock - the same
 * contract Linux 2.0 used.  It is dropped whenever the CPU leaves kernel
 * code (blocks or returns to user mode) so a sleeping syscall cannot stall
 * the other CPUs.
 */
void	smp_bkl_enter(void);
void	smp_bkl_exit(void);
void	smp_bkl_drop(void);		/* release before a context switch */
int	smp_bkl_held(void);

/* S200: synchronization + IPI helpers */
extern int smp_enabled;
/*
 * S413: second switch, on top of smp_enabled.  smpon brings the APs up
 * (per-CPU TSS, scheduler state, IPI path) but leaves them idle; this flag
 * lets the APs dispatch user threads too.  It is a separate switch because
 * kernel-thread-side work is still BSP-only and because a crash here is
 * localised to user scheduling.
 */
extern int smp_user_dispatch;
int	smp_enable(void);		/* runtime opt-in: map APIC, start APs */
void	smp_tlb_shootdown(unsigned va);
void	smp_reschedule(void);
void	smp_handle_ipi(int vector);
void	smp_print(void);
int	ipi_selftest(void);
#endif /* !ASSEMBLER */

#endif /* !_SMP_H_ */
