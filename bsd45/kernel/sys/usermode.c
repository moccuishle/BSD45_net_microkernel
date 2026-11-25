/*
 * usermode.c - S3: ramfs + ELF exec with argv/envp + int 0x80 syscalls.
 *
 * A deliberately small VFS: a read-only ramfs table built from the
 * userland images embedded in the kernel.  execve() builds a fresh page
 * directory for the new image, copies argv/envp onto the new user stack
 * and switches CR3 from the syscall path.  This is the bridge to the
 * real 4.4BSD VFS/vm_map work later.
 *
 * Syscall convention (matches 4.4BSD i386 numbers):
 *	%eax	number
 *	%ebx %ecx %edx %esi %edi	arguments
 *	int	$0x80
 * On error %eax holds -errno; the libc stubs translate that to -1/errno.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/kprocess.h>
#include <sys/user.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/utsname.h>
#include <sys/time.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>
#include <vm/mem_plan.h>

#include <machine/cpu.h>
#include <machine/pmap.h>

#include "atapi.h"
#include "isofs.h"
#include "vfs.h"
#include "sys/platform.h"
#include "thread/kthread.h"
#include "sys/smp.h"		/* S413: cpu_id(), per-CPU TSS, BKL */
#include "sys/kipc.h"		/* S300: IPC / shared-page syscalls */
#include "sys/srvproto.h"	/* KVFS: kernel-side fs_server protocol */
/* context-switch hooks are declared in sys/platform.h */
#include "sys/platform.h"
#include "sys/platform.h"
#include "sys/platform.h"
#include "vm/zcomp.h"
#include "fs/uring.h"

#define	USER_LOAD	0x08048000
#define	USER_STACK_TOP	0xBFC00000
#define	USER_STACK_SIZE	0x200000	/* S411: fixed 2 MB user stack */
#define	PG_COW		0x00000200	/* software COW marker (PG_FOD bit) */
#define	PG_ZCOMP	0x00000400	/* not-present compressed-page entry */
#define	USER_ARG_PAGE	(USER_STACK_TOP - PAGE_SIZE)
#define	USER_ARGS_MAX	4096

#define	USER_HEAP_START	0x09000000
/*
 * S411: per-thread user stacks are 2 MB each and live in a dedicated 32 MB
 * window (16 tid slots, matching TID_STRIDE).  Stack pages are demand-paged.
 * S411：每线程 2MB 用户栈，放在 32MB 专用窗口（16 个 tid 槽，对应
 * TID_STRIDE），页面按需分配。
 */
#define	UTHREAD_STACK_BASE	0xB0000000
#define	UTHREAD_STACK_SIZE	0x200000
#define	UTHREAD_STACKS		16
/*
 * User address space ends at 0xC0000000 (PDE 0x300).  Everything at or
 * above 0x300 is a kernel window shared into every user PD, so fork/exit
 * must neither copy nor free those PDEs.  S411 moved the process table and
 * the stack pool below the old 0x3f6 boundary, which made this distinction
 * load-bearing.
 * 用户地址空间到 0xC0000000（PDE 0x300）为止；0x300 以上全是内核共享窗口，
 * fork/exit 都不能复制或释放。S411 把进程表/栈池移到旧 0x3f6 边界以下，
 * 这条边界因此变得关键。
 */
#define	USER_PDE_LIMIT		0x300

/*
 * Scratch mapping window.  These three pages are rewritten by map_page() on
 * every physical-page access, so they must stay above the kernel image and
 * clear of every other fixed kernel object in the same 4 MB SYSTEM page
 * table (PDE 0x3F8): the process table at 0xFE340000 and the TCB pool at
 * 0xFE3C0000 live higher up in that very page table.  An overlap corrupts
 * kernel state silently instead of faulting (next_pa was zeroed that way
 * once), so main() checks it and panics.
 * 临时映射窗口：这三页每次访问物理页都会被重写，必须在内核镜像之上，且避开
 * 同一 4MB 系统页表（PDE 0x3F8）里的其他固定对象——进程表 0xFE340000、
 * TCB 池 0xFE3C0000 都在同一张页表里更高处。重叠只会静默踩坏内核状态
 * （next_pa 曾被清零），所以 main() 里带显式检查。
 */
/*
 * S420: scratch windows are PER-CPU.  They are fixed VAs rewritten by
 * map_page() on every physical-page access; with one shared set, an AP's
 * map_page() silently repointed the BSP's window mid-use (467 BKL-free
 * maps out of 54k in a proctest run), scribbling page tables and TCBs.
 * Each CPU now gets its own group of 6 pages in PDE 0x3F8 (16 CPUs x 6
 * pages = 384 KB, ending at 0xFE260000, below the process table at
 * 0xFE340000).  The PTEs are installed on demand by pmap_map().
 */
#define	SCRATCH_GRP	(6 * cpu_id())
#define	SCRATCH_PD	(0xFE200000u + SCRATCH_GRP * 0x1000u)
#define	SCRATCH_PT	(0xFE201000u + SCRATCH_GRP * 0x1000u)
#define	SCRATCH_PAGE	(0xFE202000u + SCRATCH_GRP * 0x1000u)

#define	NFD		16
#define	WNOHANG		1
#define	WUNTRACED	2
#define	MAXARG		16
#define	MAXENV		8

/* 4.4BSD errno values we return */
#define	EPERM		1
#define	ENOENT		2
#define	ESRCH		3
#define	EINTR		4
#define	ENOEXEC		8
#define	EBADF		9
#define	ECHILD		10
#define	ENOMEM		12
#define	ERANGE		34
#define	EACCES		13
#define	EFAULT		14
#define	ENOTDIR		20
#define	EISDIR		21
#define	EINVAL		22
#define	EMFILE		24
#define	ENOTTY		25
#define	EROFS		30
#define	EAGAIN		35
#define	ENOSYS		78

extern const unsigned char hello_elf[], init_elf[];
extern const unsigned int hello_elf_len, init_elf_len;
extern int _ucodesel, _udatasel;
extern void enter_user(unsigned pd, unsigned entry, unsigned esp);
extern void syscall_entry(void);
extern int IdlePTD;		/* physical address of the kernel page dir */
extern int cngetc(void);
extern void cnputc(int c);
/* Dynamic physical-memory ceiling: physmem is the page count reported by the
 * BIOS multiboot path (machdep.c: physmem = btoc(...)).  Deriving the bump
 * allocator's limit from it lets a VMware VM with more RAM host more
 * processes, instead of being pinned to the old hard-coded 0x3f000000.
 * 动态物理内存上界：physmem 是 BIOS 探测到的物理页数。据此推导 bump 分配器
 * 上界，使 VMware 中加大内存后可创建更多进程，不再被写死的 0x3f000000 限制。 */
extern int physmem;

#ifndef	O_RDONLY
#define	O_RDONLY	0x0000
#define	O_WRONLY	0x0001
#define	O_RDWR		0x0002
#define	O_ACCMODE	0x0003
#define	O_APPEND	0x0008
#define	O_CREAT		0x0200
#define	O_TRUNC		0x0400
#define	O_EXCL		0x0800
#endif

struct file {
	const char		*f_name;
	const unsigned char	*f_data;
	const unsigned int	*f_len;
};

static struct file ramfs[] = {
	{ "/sbin/init",	init_elf,	&init_elf_len },
	{ "/hello",	hello_elf,	&hello_elf_len },
	{ "/etc/profile", 0, 0 },
	{ 0, 0, 0 }
};

#define	FD_FILE	0
#define	FD_DIR	1
#define	FD_PIPE	2
#define	FD_TMPFILE	3
#define	FD_TMPDIR	4
#define	FD_CONSOLE	5
#define	FD_NULL		6	/* /dev/null */
#define	FD_ZERO		7	/* /dev/zero */
#define	FD_MOUNTS	8	/* /dev/mounts */
#define	FD_PS		9	/* /dev/ps */
#define	FD_VMSTAT	10	/* /dev/vmstat: S17 cache counters */

/*
 * S409: /dev/ps snapshot.  File scope (not function-static) so the read path
 * can re-render it at the start of every open; see the FD_PS branch.
 * S410: 32 KB, not 8 KB - NPROC is 1024 now and a row is ~24 bytes, so a
 * full table is ~24.6 KB and 8 KB would have truncated `ps` at ~340 rows.
 * /dev/ps 快照：放在文件作用域，读取路径才能在每次 open 时重新渲染。
 * S410：NPROC 已到 1024，单行约 24 字节，全表约 24.6KB，8KB 会在 340 行处
 * 截断，所以扩到 32KB。
 */
static char	ps_snap_buf[32768];
static int	ps_snap_len;

/*
 * S17: mmap(2).  This port has no demand paging (a page fault stops the
 * machine), so mappings are populated eagerly:
 *	MAP_ANON            - fresh zeroed pages
 *	MAP_PRIVATE + file  - copy of each page cache page (writes are private)
 *	MAP_SHARED + file   - the page cache page itself; msync() writes back
 * The 4.5BSD argument list is (addr,len,prot,flags,fd,pad,pos); this port
 * takes the first five in registers and only supports pos == 0.
 */
#define	MAP_SHARED	0x0001
#define	MAP_PRIVATE	0x0002
#define	MAP_FIXED	0x0010
#define	MAP_ANON	0x1000
#define	PROT_READ	0x01
#define	PROT_WRITE	0x02
#define	PROT_EXEC	0x04
#define	MMAP_MAXLEN	0x400000	/* 4 MB per mapping */
#define	NMAP		KPROC_NMAP

/* struct kvma / struct kfd are defined in sys/kproc.h (via sys/kprocess.h). */

/* saved user context (struct sysregs) is declared in sys/platform.h */

/*
 * Process table capacity (S407).  The old limit of 24 was a *loader* limit:
 * .bss had to end below stage 2 at 0x90000 (INT 13h itself refuses to write
 * from 0xA0000 up, so the staged image is capped at 576 KB), and 24 x
 * sizeof(struct process) fitted exactly.  Both constraints are gone:
 *
 *   - procs[] is no longer part of .bss: it lives in the PROC_TABLE_VA
 *     window, backed by pages taken from the bump allocator, so the table
 *     costs zero bytes in the image the BIOS has to stage;
 *   - stage 2's ELF-header snapshot moved above 1 MB (0x8F000 -> 0x600000)
 *     and the kernel itself is linked at physical 0x101000 (S408), so .bss
 *     is no longer confined to the 0x10000..0x90000 hole at all;
 *   - zero-initialised statics really do land in .bss now (build_kernel.ps1
 *     used to silently skip tools/pe_bss_fix.py because `Get-Command python`
 *     resolved to the Microsoft Store stub, inflating the image to 618 KB -
 *     more than the 576 KB the BIOS can stage).
 *
 * NPROC x sizeof(struct process) = 1024 x 1808 = 1.77 MB of kernel VA (not image).
 * Two ceilings remain: the slot table (fork returns -EAGAIN when it is full)
 * and physical memory (page_alloc() panics - there is no swapper to page
 * out to, so running out of RAM is a kernel bug, not a per-process error;
 * with S408's BIOS/E801 detection a 512 MB guest really does hand out
 * 511 MB, which proctest's 75 MB phase exercises).
 * S410: 256 -> 1024 slots.  1024 x 1808 = 1.77 MB does not fit in the
 * 0xFE340000..0xFE3FFFFF gap any more, so the table moved up to 0xFED00000,
 * above every existing kernel window (see the VA map in the header comment
 * of this file).  A thousand live processes also needs a 16-bit COW
 * refcount - a byte saturates at 255 and page_put() would then free a page
 * that 745 children still map.
 * 进程表容量（S407）。旧的 24 是**引导器**上限：.bss 必须在 0x90000 的 stage2
 * 之下（INT 13h 也不写 0xA0000 以上，暂存镜像上限 576KB），24 x 1808 刚好塞满。
 * 现在两个约束都没了：procs[] 不再属于 .bss，而在 PROC_TABLE_VA 窗口里（物理页
 * 由 bump 分配器预留，镜像里零字节）；stage2 的 ELF 头快照搬到 1MB 以上
 * （0x8F000 → 0x600000），内核本身也链接在物理 0x101000（S408），.bss 完全不
 * 再受 0x10000..0x90000 那块空洞限制；而且零初始化静态量现在真的进 .bss
 * （过去 `Get-Command python` 命中应用商店桩，pe_bss_fix.py 被静默跳过，镜像
 * 涨到 618KB，超过 BIOS 能暂存的 576KB）。
 * 仍有两个上限：槽位表满时 fork 返回 -EAGAIN；物理内存耗尽时 page_alloc 会
 * panic——本端口没有 swap，内存耗尽是内核缺陷而不是单进程错误。S408 的
 * BIOS/E801 探测让 512MB guest 真的能发放 511MB，proctest 的 75MB 阶段就是
 * 在验证这一点。
 * S410 把 256 提到 1024：1.77MB 已塞不进 0xFE340000..0xFE3FFFFF 那 768KB 空档，
 * 表搬到 0xFED00000（见本文件头部 VA 图）；同时 COW 引用计数必须改成 16 位，
 * 单字节在 255 饱和，1000 个子进程共享一页时 page_put 会释放别人还映射的页。
 */
#define	NPROC		1024		/* process slots (was 24: loader layout) */
/* TID namespace constants (TID_USER_BASE/STRIDE/KERN_BASE) live in kthread.h. */
#define	P_FREE		0
#define	P_RUN		1
#define	P_BLOCKED	2
#define	P_ZOMBIE	3
#define	P_STOPPED	4
/*
 * S413: a slot claimed by proc_alloc() but not yet published by its caller.
 * It must not look free to a second allocator on another CPU, and it has no
 * TCB yet, so no scheduler scan can reach it.
 * S413：被 proc_alloc() 预留、但调用者还没发布出去的槽位。它在另一个核看来
 * 不能是空闲的；此时它还没有 TCB，所以任何调度扫描都碰不到它。
 */
#define	P_ALLOC		5
/*
 * S425: the process is tearing itself down.  It must not be scheduled, not
 * be reaped by wait4() and not be picked up by a second proc_exit() while
 * the address space is being quiesced - proc_exit() has to drop the BKL for
 * that wait (a sibling can be spinning in smp_bkl_enter() with our CR3
 * loaded), so other CPUs really do run in the middle of it.
 * S425：进程正在拆除。此时不能被调度、不能被 wait4 回收、也不能被第二个
 * proc_exit 重入——拆除期间要放掉 BKL 等其它核离开本地址空间。
 */
#define	P_EXITING	6

/*
 * Signal numbers / defaults.  These are the 4.5BSD values; only the ones we
 * actually deliver are listed.
 */
#define	NSIG		32
#define	SIGHUP		1
#define	SIGINT		2
#define	SIGQUIT		3
#define	SIGKILL		9
#define	SIGALRM		14
#define	SIGTERM		15
#define	SIGSTOP		17
#define	SIGTSTP		18
#define	SIGCONT		19
#define	SIGCHLD		20
#define	SIGTTIN		21
#define	SIGTTOU		22
#define	SIGUSR1		30
#define	SIGUSR2		31
/* fault-generated signals (os.md mechanism 1: a bad app must not wedge us) */
#define	SIGILL		4
#define	SIGTRAP		5
#define	SIGFPE		8
#define	SIGBUS		10
#define	SIGSEGV		11

#define	SIG_DFL		0
#define	SIG_IGN		1

#define	SIG_TO_MASK(s)	(1u << (s))

/* struct process (sys/kprocess.h) is the authoritative kernel process object;
 * its tail carries the process runtime state (uk_ prefix). */

/*
 * Process table lives in a kernel VA window, not in .bss (that is what frees
 * NPROC from the loader's 0x90000 ceiling).  procs[i] / &procs[i] keep their
 * meaning: kern_table_alloc() hands back one contiguous block.
 *
 * The address matters twice over:
 *   - it must not sit inside CD_IMAGE_VA (0xFE400000 + up to 4 MB), which
 *     cd_map() re-maps on every exec.  Parking the table at 0xFE400000 made
 *     execve read argc 0, which is why S408 moved it to 0xFE340000;
 *   - 1024 slots need 1.77 MB, which does not fit the 768 KB gap between the
 *     zcomp pool and the CD window any more (S410).
 * So the table now lives at 0xFB000000, below the kernel image, in a kernel
 * window whose page table is shared into every user page directory (see
 * user_pd_create).  S411 moved it down from 0xFED00000 to make room for the
 * 2 MB COW table that covers 4 GB of RAM:
 *
 *   0xFB000000  1.77M  this process table (1024 x 1808)
 *   0xFC000000  16 MB  kernel stacks, 1024 x 16 KB (kthread.h)
 *   0xFEF00000  1 MB   TCB pool (kernel/thread/kthread.h)
 *   0xFEC00000  2 MB   COW refcount table (16-bit entries, 4 GB of RAM)
 *
 * None of these can collide with a CD image, a page-cache page or a user
 * mapping, and they sit above the 12 MB identity window locore builds.
 * 进程表放在内核 VA 窗口而不是 .bss，正是这一点让 NPROC 摆脱了引导器 0x90000
 * 的天花板；kern_table_alloc() 返回一段连续内存，procs[i] 语义不变。
 * S411 把表从 0xFED00000 下移到 0xFB000000，腾出空间给覆盖 4GB 的 2MB COW 表；
 * 这些窗口的页表都会拷进每个用户页目录（见 user_pd_create）。
 */
#define	PROC_TABLE_VA	0xFB000000
static struct process	*procs;
/* S413: serialises slot allocation in the shared process table. */
static struct spinlock	proc_table_lock;

/*
 * S413: the scheduler used to keep "who is running" in four globals.  That
 * cannot survive SMP: each CPU is simultaneously running a different thread
 * from a different address space.  All of it now lives in one struct per
 * CPU, indexed by cpu_id() (decoded from that CPU's TSS selector), so the
 * code below still reads `cur`, `cur_thread`, `cur_pd` and `need_resched`
 * but each CPU sees its own copy.
 *
 * sc_idle is the carrier an AP runs the scheduler loop on; it is not in the
 * TCB pool, so no other CPU can ever pick it.
 * S413：调度器原来用四个全局变量记录"谁在跑"，这在 SMP 下不成立——每个核同时
 * 跑的是不同线程、不同地址空间。现在这些状态按 CPU 分开放，用 cpu_id() 索引；
 * 下面代码仍然写 cur / cur_thread / cur_pd / need_resched，但每个核看到的是
 * 自己那份。sc_idle 是 AP 跑调度循环用的载体，不在 TCB 池里，别的核选不中。
 */
struct sched_cpu {
	struct process	*sc_cur;	/* current process on this CPU */
	struct kthread	*sc_curthread;	/* current execution flow */
	unsigned	sc_cur_pd;	/* page directory currently in CR3 */
	int		sc_need_resched;/* safe-point reschedule request */
	struct kthread	*sc_idle;	/* this CPU's scheduler carrier */
	/*
	 * S424: the TCB this CPU switched away from and must release once it
	 * is certain it is no longer executing on that kernel stack.  t_oncpu
	 * is a stack mutex, not affinity: holding it after the switch pins
	 * the thread to this CPU for its whole life, so an idle CPU cannot
	 * pick up work that this (busy) CPU has queued behind a running
	 * thread.  The release happens on the next scheduler entry, by which
	 * time this CPU runs on a different stack.
	 */
	struct kthread	*sc_prev;
	unsigned	sc_tick;	/* S424: per-CPU tick counter (aging cadence) */
	int		sc_parked;	/* S424: parked in hlt with nothing to run */
	/*
	 * S428: this CPU's own run queue.
	 *
	 * The scheduler used to pick work by walking all nkthread TCB slots on
	 * every context switch and every tick, arbitrated only by the claim
	 * (t_oncpu).  With 1024 slots that is 1024 loads per decision per CPU,
	 * all of them on cache lines other CPUs are writing, and it gets worse
	 * with more CPUs - the exact opposite of what SMP is for.
	 *
	 * Now every runnable user thread sits on exactly one CPU's queue
	 * (t_runq_cpu), the local pick is O(1) amortised, and an idle CPU
	 * *steals* from a busy one instead of scanning the whole table.  The
	 * table scan survives only as sched_rq_sweep(), the repair pass for a
	 * state transition that forgot to enqueue - it runs on the idle path,
	 * where the cost is free and a lost thread would otherwise hang
	 * forever.
	 *
	 * Locking: mutations (put/del) happen on state transitions, which all
	 * run under the BKL, so they never race each other; the lock exists
	 * because the *consumer* side (pick/steal) runs from the scheduler and
	 * from the timer interrupt without the BKL.
	 */
	struct kthread	*sc_rqhead;	/* runnable user threads, best-effort */
	struct kthread	*sc_rqtail;
	struct spinlock	sc_rqlock;
	int		sc_nrun;	/* queue length: steal hint */
	int		sc_online;
	unsigned	sc_switches;	/* context switches done here */
};
static struct sched_cpu	sched_cpu[MAXCPU];
static struct kthread	per_cpu_idle[MAXCPU];	/* AP carriers, not pooled */
/* S417: caller TCB of each proc_sched(), kept off the (clobberable) stack. */
static struct kthread	*ps_mt[MAXCPU];
/* S413: print one per-CPU summary each time the machine goes idle again. */
static int		idle_reported;

#define	cur		(sched_cpu[cpu_id()].sc_cur)
#define	cur_thread	(sched_cpu[cpu_id()].sc_curthread)
#define	cur_pd		(sched_cpu[cpu_id()].sc_cur_pd)
#define	need_resched	(sched_cpu[cpu_id()].sc_need_resched)

/*
 * S424: give up the TCB this CPU switched away from.
 *
 * t_oncpu started as "this CPU may still be executing on that TCB's kernel
 * stack", which is what keeps two CPUs out of one stack.  It was never
 * released on a plain context switch, so it silently became a hard affinity:
 * a thread that once ran on CPU 3 could only ever be resumed by CPU 3, and
 * the other CPUs skipped it in fair_pick_next() and went back to hlt even
 * while it sat runnable.  Releasing it here keeps the mutex semantics (we
 * only do it once this CPU is demonstrably on a different stack) and lets an
 * idle CPU pick the thread up.
 */
static void	sched_wake_idle(void);

static void
sched_release_prev(void)
{
	int id = cpu_id();
	struct kthread *prev = sched_cpu[id].sc_prev;

	if (prev == 0)
		return;
	if (prev == cur_thread)
		return;			/* we are back on that very stack */
	sched_cpu[id].sc_prev = 0;
	kthread_release_cpu(prev);
	/*
	 * S424: if the thread we just released is still runnable, an idle
	 * CPU can take it - but an idle CPU sits in hlt and only looks again
	 * on its next tick, by which time this CPU has usually re-claimed
	 * the thread.  Nudge one idle CPU so the work actually spreads
	 * instead of ping-ponging on a single core.
	 */
	if (prev->t_state == KT_RUN)
		sched_wake_idle();
}

/*
 * S424: wake one CPU that is parked in the idle loop, so it can claim a
 * runnable thread instead of waiting up to a full tick.  Called only on
 * transitions where a thread became available, never per tick.
 */
static void
sched_wake_idle(void)
{
	int i, id = cpu_id();

	for (i = 0; i < MAXCPU; i++) {
		if (i == id || !sched_cpu[i].sc_online || !sched_cpu[i].sc_parked)
			continue;
		smp_resched_cpu(i);
		return;
	}
}

/* ------------------------------------------------------------ S428 queues */
/* Defined further down with the scheduler; the queue code below needs them. */
static int tcb_runnable(struct kthread *t);
static int smt_affinity_score(struct kthread *t, int cid);

/*
 * Per-CPU run queues.  Every user TCB that is KT_RUN (and whose process may
 * run) sits on exactly one CPU's queue; t_runq_cpu names it, -1 means "not
 * queued".  Kernel threads keep their own global queue (rq_head in
 * kthread.c) because they are runnable regardless of any process state.
 *
 * The invariants are enforced by three call sites rather than by every writer:
 * sched_rq_put() on a transition to KT_RUN, sched_rq_del() on a transition
 * away from it, and sched_rq_sweep() as the repair pass.  Queue mutation is
 * serialised by the BKL (all state transitions hold it); the spinlock only
 * keeps the scheduler's pick/steal from tearing a list another CPU is
 * walking.  Nobody takes the BKL while holding a queue lock, so the order
 * BKL -> sc_rqlock is acyclic.
 */
#define	RQ_SCAN		32	/* candidates examined per pick (bounded cost) */

static void
sched_rq_put(struct kthread *t, int cpu)
{
	struct sched_cpu *sc;

	if (t == 0 || t->t_proc == 0)	/* kernel threads use rq_head */
		return;
	if (cpu < 0 || cpu >= MAXCPU || !sched_cpu[cpu].sc_online)
		cpu = cpu_id();
	if (cpu < 0 || cpu >= MAXCPU)
		cpu = 0;
	sc = &sched_cpu[cpu];
	spin_lock(&sc->sc_rqlock);
	if (t->t_runq_cpu < 0) {
		t->t_runq = 0;
		if (sc->sc_rqtail != 0)
			sc->sc_rqtail->t_runq = t;
		else
			sc->sc_rqhead = t;
		sc->sc_rqtail = t;
		t->t_runq_cpu = cpu;
		sc->sc_nrun++;
	}
	spin_unlock(&sc->sc_rqlock);
}

static void
sched_rq_del(struct kthread *t)
{
	struct sched_cpu *sc;
	struct kthread *p, *prev;
	int cpu;

	if (t == 0 || (cpu = t->t_runq_cpu) < 0)
		return;
	if (cpu >= MAXCPU)
		cpu = 0;
	sc = &sched_cpu[cpu];
	spin_lock(&sc->sc_rqlock);
	for (p = sc->sc_rqhead, prev = 0; p != 0; prev = p, p = p->t_runq) {
		if (p != t)
			continue;
		if (prev != 0)
			prev->t_runq = t->t_runq;
		else
			sc->sc_rqhead = t->t_runq;
		if (sc->sc_rqtail == t)
			sc->sc_rqtail = prev;
		if (sc->sc_nrun > 0)
			sc->sc_nrun--;
		break;
	}
	t->t_runq = 0;
	t->t_runq_cpu = -1;
	spin_unlock(&sc->sc_rqlock);
}

/*
 * Take the best candidate off one CPU's queue.  Scans at most RQ_SCAN entries
 * so the cost stays bounded even when everything sits at the same priority;
 * threads owned by another CPU stay queued (their owner will resume them),
 * and threads whose process may no longer run are left for sched_rq_del().
 */
static struct kthread *
sched_rq_pick(int cpu)
{
	struct sched_cpu *sc = &sched_cpu[cpu];
	struct kthread *p, *best = 0, *bestprev = 0, *prev = 0;
	int n, best_prio = -1, best_aff = -1;

	spin_lock(&sc->sc_rqlock);
	for (p = sc->sc_rqhead, n = 0; p != 0 && n < RQ_SCAN;
	     prev = p, p = p->t_runq, n++) {
		int prio, aff;

		if (!tcb_runnable(p))
			continue;
		if (kthread_owned_by_other(p, cpu))
			continue;
		prio = sched_effective_prio(p);
		aff = smt_affinity_score(p, cpu);
		if (best == 0 || prio > best_prio ||
		    (prio == best_prio && aff > best_aff)) {
			best = p;
			bestprev = prev;
			best_prio = prio;
			best_aff = aff;
		}
	}
	if (best != 0) {
		if (bestprev != 0)
			bestprev->t_runq = best->t_runq;
		else
			sc->sc_rqhead = best->t_runq;
		if (sc->sc_rqtail == best)
			sc->sc_rqtail = bestprev;
		if (sc->sc_nrun > 0)
			sc->sc_nrun--;
		best->t_runq = 0;
		best->t_runq_cpu = -1;
	}
	spin_unlock(&sc->sc_rqlock);
	return (best);
}

/* Work stealing: one pass over the other CPUs, stopping at the first hit. */
static struct kthread *
sched_rq_steal(int cid)
{
	struct kthread *t;
	int i;

	for (i = 0; i < MAXCPU; i++) {
		if (i == cid || !sched_cpu[i].sc_online)
			continue;
		if (sched_cpu[i].sc_nrun <= 0)
			continue;	/* unlocked hint only */
		if ((t = sched_rq_pick(i)) != 0)
			return (t);
	}
	return (0);
}

/*
 * Repair pass.  A transition that returned a thread to KT_RUN without
 * enqueueing it (or one that left it queued after it stopped being runnable)
 * would otherwise make it invisible: the thread would never be picked again.
 * Only the idle path calls this, so the O(nkthread) walk costs nothing.
 */
static void
sched_rq_sweep(int cid)
{
	int i;

	for (i = 0; i < nkthread; i++) {
		struct kthread *t = &kthreads[i];

		if (!t->t_used)
			continue;
		if (t->t_state == KT_RUN && t->t_proc != 0) {
			if (t->t_runq_cpu < 0 && tcb_runnable(t))
				sched_rq_put(t, cid);
		} else if (t->t_runq_cpu >= 0)
			sched_rq_del(t);
	}
}

/* Transition helpers: the only places that flip runnability of a user TCB. */
static void
sched_runnable(struct kthread *t, int cpu)
{

	if (t == 0)
		return;
	t->t_state = KT_RUN;
	sched_rq_put(t, cpu);
}

static void
sched_unrunnable(struct kthread *t, int state)
{

	if (t == 0)
		return;
	sched_rq_del(t);
	t->t_state = state;
}

/*
 * S411: user VA layout, derived from the boot memory plan instead of the
 * old fixed 48 MB heap / 512 MB mmap constants.  Defaults match a 128 MB
 * machine so the values are sane before proc_table_init() runs.
 * S411：用户地址空间布局由启动内存规划推导，默认值对应 128MB。
 */
static unsigned	user_heap_max = USER_HEAP_START + (64u * 1024u * 1024u);
static unsigned	mmap_base = USER_HEAP_START + (64u * 1024u * 1024u);
static unsigned	mmap_limit = USER_HEAP_START + (80u * 1024u * 1024u);
static void	user_layout_init(void);
/* KVFS: forward declaration of the in-flight forwarding waiter (defined in
 * the KVFS section below) so port-release fault isolation can reach it. */
static struct process *kvfs_waiter;

/* EN: Accessor for the current user thread's TCB (used by preemption core). */
struct kthread *
cur_proc_kthread(void)
{

	return (cur_thread);
}
static int		next_pid = 2;

/*
 * KLT blocking primitives.  A block affects only the calling TCB, never the
 * whole process: sibling threads sharing the address space stay runnable.
 * proc->uk_state is kept in sync as a coarse "any thread blocked/waiting"
 * marker so the existing wait/wake code paths keep working, but runnability
 * is decided per TCB (t_state) by the scheduler.
 */

/* EN: Block the calling TCB with reason wchan; its process stays alive for siblings. */
static void
kthread_block_tcb(void *wchan)
{
	struct kthread *t = cur_thread;

	/*
	 * Rule: a single-threaded process blocks as a whole (classic BSD
	 * wait/pipe semantics, all waiters/wakers key off proc state).  Only
	 * a process with >1 live threads blocks just the calling TCB, leaving
	 * its siblings runnable -- the KLT advantage over ULT.
	 */
	if (cur != 0 && cur->uk_p_nthreads <= 1) {
		cur->uk_state = P_BLOCKED;
		/*
		 * KVFS fix: also block the main TCB.  If t_state stays KT_RUN,
		 * proc_sched()'s fall-back-to-self branch (mt->t_state==KT_RUN)
		 * fires and immediately resumes the just-blocked process, so a
		 * kernel-side forward would "return" with a stale eax instead of
		 * waiting for the service reply.  proc_wake() restores KT_RUN.
		 * KVFS 修复：同时阻塞主 TCB。若 t_state 仍为 KT_RUN，proc_sched()
		 * 末尾的"回落自己"分支会立即恢复刚阻塞的进程，使内核态转发不等
		 * 回复就带着旧 eax 返回。proc_wake() 会恢复 KT_RUN。
		 */
		if (t != 0)
			sched_unrunnable(t, KT_BLOCK);
		return;
	}
	if (t != 0) {
		sched_unrunnable(t, KT_BLOCK);
		t->t_wchan = wchan;
	}
	/* multi-threaded: keep the process P_RUN so siblings keep running */
}

/* EN: Wake a process and restore its blocked TCB(s) to runnable. */
static void
proc_wake(struct process *p)
{
	struct kthread *t;
	int i;

	if (p == 0)
		return;
	/*
	 * S425: never resurrect a process that is in the middle of proc_exit()
	 * (the teardown drops the BKL while it waits for other CPUs, so a
	 * concurrent wakeup really can land here) or one that is already gone.
	 */
	if (p->uk_state == P_FREE || p->uk_state == P_EXITING)
		return;
	p->uk_state = P_RUN;
	for (i = 0; i < nkthread; i++) {
		t = &kthreads[i];
		if (t->t_used && t->t_proc == p && t->t_state == KT_BLOCK) {
			t->t_sleep_ticks++;	/* account wake from sleep/I/O */
			sched_on_wake(t);	/* boost interactivity, pick quantum */
			/*
			 * S428: queue it where it last ran when that CPU still
			 * owns the TCB, otherwise locally and let stealing move
			 * it.  sched_runnable() is the only place that makes a
			 * user thread visible to the scheduler again.
			 */
			sched_runnable(t, t->t_oncpu >= 0 ? t->t_oncpu : cpu_id());
			t->t_wchan = 0;
			/*
			 * S413: the woken thread keeps running on the CPU that
			 * owns it.  Nudge that CPU so it re-runs its scheduler
			 * now instead of at the next tick.
			 */
			if (t->t_oncpu >= 0)
				smp_resched_cpu(t->t_oncpu);
		}
	}
}

/*
 * KLT address-space switch: only reload CR3 when the selected thread belongs
 * to a different process.  Threads of one process share the page directory,
 * so a same-process switch updates only the TCB (kernel stack / registers).
 * resume_user() treats a pd of 0 as "keep current CR3".
 */
static unsigned
switch_pd_for(struct process *p)
{
	unsigned live_pd = hal_address_space_current();

	/*
	 * S421: compare against the LIVE CR3, not the cached cur_pd.
	 * proc_exit() switches to IdlePTD with a direct
	 * hal_address_space_activate() that does not touch cur_pd, so the
	 * cache can claim "already on p's PD" while CR3 actually points at
	 * the kernel directory.  Returning 0 then makes resume_user keep the
	 * wrong CR3 and the first user instruction faults #SS (seen only on
	 * APs running fork children under proctest).
	 */
	if (p->uk_pd != live_pd) {
		cur_pd = p->uk_pd;
		return (cur_pd);
	}
	cur_pd = live_pd;		/* resync cache with reality */
	return (0);			/* same address space: do not touch CR3 */
}

static unsigned next_pa;	/* first free physical page, set by main() */
static unsigned page_alloc(void);
static void *map_page(unsigned va, unsigned pa);
static void page_free(unsigned pa);
static int ucopyin(void *udaddr, void *kaddr, unsigned len);
static int ucopyout(void *kaddr, void *udaddr, unsigned len);

/* 中文：由 main() 在启动时调用，把 bump 分配器的起点定在内核镜像末尾之后 */
/* EN: Called by main() at boot: start the bump allocator after the kernel image */
void
page_alloc_set_floor(unsigned pa)
{

	pa = (pa + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (pa < 0x200000)
		pa = 0x200000;		/* keep the old floor for a small kernel */
	if (pa > next_pa)
		next_pa = pa;
}

/*
 * Reserve `size` bytes of contiguous physical memory from the bump allocator
 * and map it at `va` in the kernel's SYSTEM window (VA = KERNBASE + PA is an
 * identity map, but these tables need a *fixed* VA, so each page is mapped
 * explicitly).  The process table and the TCB pool are allocated this way so
 * their capacity is a memory question, not an image-layout question.
 * 从 bump 分配器预留一段连续物理内存并映射到固定内核 VA。进程表与 TCB 池
 * 都这样分配，容量只受内存限制，不再受镜像布局限制。
 */
/*
 * S410: install the page table for one 4 MB kernel window.
 *
 * locore only identity-maps 12 MB above KERNBASE (PDEs 0x3f8..0x3fa), and
 * pmap_enter() does *not* build page tables on demand - it calls pmap_pte(),
 * which returns 0 for a missing PDE, and then dereferences that 0 (reported
 * as TRAP 12 with cr2=0, which is exactly how the first attempt at this
 * layout died).  Every kernel window above 0xFEC00000 therefore has to be
 * opened explicitly before anything is mapped into it.
 * 为一个 4MB 内核窗口建页表。locore 只恒等映射 KERNBASE 以上 12MB
 * （PDE 0x3f8..0x3fa），而 pmap_enter() 不会按需建页表——它调 pmap_pte()，
 * 缺 PDE 时返回 0，随后解引用 0（表现为 TRAP 12 + cr2=0，S410 第一次尝试就是
 * 这么死的）。因此 0xFEC00000 以上的每个窗口都必须先显式打开。
 */
static int
kern_window_install(unsigned va)
{
	unsigned pdi = va >> 22;
	unsigned *pd = (unsigned *)kernel_pmap->pm_pdir;
	unsigned ptpa, *pt;
	int i;

	if (pdi >= 1024)
		return (-1);
	if (pd[pdi] & PG_V)
		return (0);		/* already installed */
	ptpa = page_alloc();
	pt = map_page(SCRATCH_PT, ptpa);
	for (i = 0; i < PAGE_SIZE / 4; i++)
		pt[i] = 0;
	/* supervisor-only: user page directories copy this PDE but the PTEs
	 * pmap_map() installs are supervisor as well. */
	pd[pdi] = ptpa | PG_V | PG_RW;
	return (0);
}

void *
kern_table_alloc(unsigned va, unsigned size)
{
	unsigned n = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	unsigned pa0, pa;
	unsigned i;

	if (n == 0)
		return ((void *)va);
	pa0 = page_alloc();
	for (i = 1; i < n; i++) {
		pa = page_alloc();
		if (pa != pa0 + i * PAGE_SIZE)
			panic("kern_table_alloc: non-contiguous pages at %x", pa);
	}
	for (i = 0; i < n; i++) {
		/* open a new 4 MB kernel window when the range starts one */
		if (((va + i * PAGE_SIZE) & ((4u * 1024 * 1024) - 1)) == 0)
			(void)kern_window_install(va + i * PAGE_SIZE);
		pmap_map((vm_offset_t)(va + i * PAGE_SIZE),
		    (vm_offset_t)(pa0 + i * PAGE_SIZE),
		    (vm_offset_t)(pa0 + (i + 1) * PAGE_SIZE),
		    VM_PROT_READ | VM_PROT_WRITE);
	}
	bzero((void *)va, n * PAGE_SIZE);
	return ((void *)va);
}

static void sig_deliver(struct process *, struct sysregs *);
/* S424: the single thread-selection policy (defined with sched_tick below). */
static struct kthread *fair_pick_next(struct kthread *me);
static int console_fg_pgrp;

/*
 * S430: guard for the page allocator.  The free list, the bump pointer and
 * the counters are global, and user_page_fault() now has a branch that runs
 * *without* the big kernel lock, so they can no longer rely on it.  This is
 * the first piece of the BKL split (#10): a subsystem lock that is correct on
 * its own, so the BKL can be dropped path by path instead of all at once.
 */
static struct spinlock	page_lock;
static unsigned page_free_list;
static unsigned page_nalloc, page_nfree, page_ninuse;

/*
 * S431: guard for the user page tables (step 1 of the S430 roadmap: a lock
 * that is correct on its own, so the BKL can be dropped path by path).
 *
 * Scope: every write to a user page directory, a user page table or a user
 * PTE, plus the page-table page allocation that goes with it.  It is one
 * global lock rather than one per address space: page tables are walked from
 * more than one address space at a time (fork copies the parent while the
 * child's CR3 is live) and the hot path is a single PTE store, so per-space
 * locking would buy nothing but a lookup.
 *
 * It is deliberately *recursive on the owning CPU*.  user_map() has to
 * allocate a page-table page in the middle of an edit, page_alloc() may call
 * zcomp_reclaim(), and that walks the very page tables the caller is
 * editing.  Forbidding that re-entry would mean restructuring every helper
 * into "allocate first, edit second", and the reclaim would still be free to
 * compress the PTE we are about to overwrite.  Recursion makes the re-entry
 * harmless: same CPU, same edit sequence, no second lock.
 *
 * Ordering: pmap_lock -> page_lock (user_map allocates a table page while
 * holding it) and pmap_lock -> zcomp lock (free_address_space/zcomp_reclaim).
 * Nothing takes either of those and then asks for pmap_lock.
 *
 * 用户页表锁（S430 路线图第 1 步）。范围：用户页目录/页表/PTE 的任何写入，
 * 以及随之而来的页表页分配。用一把全局锁而不是每个地址空间一把：fork 会一边
 * 拷父进程一边让子进程的 CR3 生效，热点又只是一条 PTE 存储，按地址空间分锁
 * 只多一次查找。这把锁**对持有它的 CPU 可递归**：user_map 在编辑中途要分配
 * 页表页，page_alloc 可能进入 zcomp_reclaim，而后者恰好会遍历调用方正改的
 * 页表。禁止重入就得把每个辅助函数改成“先分配后编辑”，而且回收依然可以压缩
 * 我们即将覆盖的那个 PTE。递归让重入无害：同一个 CPU、同一段编辑、不取第二
 * 把锁。锁序：pmap_lock -> page_lock、pmap_lock -> zcomp 锁，反向没有路径。
 */
static struct spinlock	pmap_lock;
static volatile int	pmap_owner = -1;	/* cpu_id() of the holder */
static int		pmap_depth;		/* recursion depth (owner only) */

static void
pmap_lock_enter(void)
{
	int me = cpu_id();

	if (pmap_owner == me) {
		pmap_depth++;
		return;
	}
	spin_lock(&pmap_lock);
	pmap_owner = me;
	pmap_depth = 1;
}

static void
pmap_lock_exit(void)
{

	if (--pmap_depth > 0)
		return;
	pmap_owner = -1;
	spin_unlock(&pmap_lock);
}

/*
 * COW reference counts (os.md mechanism 2/3).  One 16-bit count per physical
 * page (S410; it was a byte, which could not describe 1000 sharers).
 *
 * The table used to be a .bss array covering only the first 64 MB
 * (1 << 14 entries): with the kernel staged below 1 MB, .bss had to end
 * below stage 2 at 0x90000, so a bigger table would not have fitted.  That
 * silently broke COW on a 512 MB guest once allocations crossed 64 MB -
 * page_index() returned 0, page_ref_inc() became a no-op and page_put()
 * freed a page another process still mapped.
 *
 * Now the kernel is linked at physical 0x101000 (see boot/i386/kernel.ld),
 * so .bss no longer constrains the table; but a fixed 4 GB table would cost
 * 2 MB of RAM in every guest.  Instead page_ref_init() sizes it from the
 * BIOS-reported physmem and maps it into PAGE_REF_VA, so the table grows
 * with the machine (512 MB guest -> 256 KB).
 * Calls made before page_ref_init() runs (bootstrap tables) see page_ref ==
 * 0 and are simply not refcounted; nothing is COW-shared that early.
 * COW 引用计数表，每物理页一个 16 位计数（S410；原来是字节，描述不了 1000 个
 * 共享者）。过去是 .bss 里只覆盖前 64MB 的数组（内核暂存在 1MB 以下，.bss 必须
 * 在 0x90000 的 stage2 之前结束，更大的表放不下），512MB guest 一旦分配超过
 * 64MB，page_index() 返回 0，page_ref_inc() 变成空操作，page_put() 就会释放别的
 * 进程还在映射的页。现在内核链接在物理 0x101000，.bss 不再限制表的大小；但固定
 * 4GB 表会在每个 guest 里白占 2MB，所以改由 page_ref_init() 按 BIOS 探测到的
 * physmem 定容并映射到 PAGE_REF_VA：表随机器大小增长（512MB guest 256KB）。
 * 在它之前发生的分配（引导期表）看到 page_ref == 0，不做引用计数——那个阶段还
 * 没有任何 COW 共享。
 */
/*
 * S410: 16-bit entries, and a window of their own.
 *
 * Two independent bugs forced this:
 *   - the counts were bytes.  page_ref_inc() saturated at 255, so with 1000
 *     children sharing the parent's pages the count stopped rising, and once
 *     745 of them had exited page_put() saw 1 and freed a page the remaining
 *     254 still mapped.  1024 slots therefore need a 16-bit counter.
 *   - PAGE_REF_VA was 0xFE900000, the same address as the page cache's
 *     PCACHE_VA.  Reading a file through tmpfs/ffs/jlfs would map cache pages
 *     straight over the refcount table, so page_ref_get() returned file data.
 *     The table moved to 0xFEC00000, which nothing else uses.
 *
 * The window is 1 MB, so 1 MB / 2 bytes = 524288 pages = 2 GB of RAM.  A
 * larger guest would need a 2 MB window (see the VA map at PROC_TABLE_VA -
 * there is no room left above 0xFEC00000 for one).
 * COW 引用计数表。16 位 + 独占窗口，两个独立缺陷逼出来的：
 *   - 原来是字节计数，page_ref_inc() 在 255 饱和；1000 个子进程共享父进程的页
 *     时计数不再增长，等到 745 个退出后 page_put 看到 1，就把剩下 254 个还在
 *     映射的页释放掉了。1024 槽必须用 16 位。
 *   - PAGE_REF_VA 原来是 0xFE900000，与页缓存的 PCACHE_VA 同址：经
 *     tmpfs/ffs/jlfs 读文件会把缓存页直接映射到引用计数表上，page_ref_get()
 *     读回的就是文件内容。现在搬到没人用的 0xFEC00000。
 * 窗口 1MB，即 1MB/2 = 524288 页 = 2GB 内存；更大的 guest 需要 2MB 窗口，而
 * 0xFEC00000 以上已经没有空间（见 PROC_TABLE_VA 处的表的 VA 图）。
 */
#define	PAGE_REF_VA	0xFEC00000	/* S411: 2 MB window: 2 B/page -> 4 GB */
#define	PAGE_REF_MAX	(2u * 1024 * 1024 / 2)	/* entries, not bytes */
static unsigned short	*page_ref;	/* one refcount per physical page */
static unsigned	page_ref_pages;		/* entries actually mapped */
unsigned	cow_faults;
unsigned	cow_copies;
unsigned	cow_shared;

/* 中文：把物理地址换算为 COW 引用计数表索引，超出表覆盖范围返回 0 */
/* EN: Convert a physical address into an index in the COW reference-count table, returning 0 when out of range */
static unsigned
page_index(unsigned pa)
{
	unsigned i = pa >> 12;

	if (page_ref == 0 || i >= page_ref_pages)
		return (0);
	return (i);
}

/* 中文：设置物理页的 COW 引用计数值，超出表范围则忽略 */
/* EN: Set the COW reference count for a physical page, ignoring pages outside the table */
static void
page_ref_set(unsigned pa, unsigned v)
{
	unsigned i = page_index(pa);

	if (i != 0)
		page_ref[i] = (unsigned short)v;
}

/* 中文：读取物理页的 COW 引用计数，超出表范围的页视作独占返回 1 */
/* EN: Get the COW reference count of a physical page, returning 1 (treated as unshared) for pages outside the table */
static unsigned
page_ref_get(unsigned pa)
{
	unsigned i = page_index(pa);

	return (i != 0 ? page_ref[i] : 1);
}

/*
 * S431: atomic 16-bit add.  The refcounts are read and written from more
 * than one CPU now (a fork on CPU A bumps the count while the other process
 * on CPU B drops it in exit), and a plain ++ compiles to load/add/store, so
 * two CPUs could lose an increment and page_put() would then free a page
 * that is still mapped.  `lock xaddw` is one instruction and returns the old
 * value, which is what the "am I the last user" decision needs.
 * 16 位原子加。引用计数现在会被多个 CPU 触碰（A 核 fork 时加，B 核 exit 时减），
 * 普通 ++ 会编译成 load/add/store，两个 CPU 可能丢掉一次自增，page_put() 就会
 * 释放仍在映射的页。`lock xaddw` 一条指令并返回旧值，正是"我是不是最后一个"
 * 所需的语义。
 */
static unsigned
page_ref_add(unsigned pa, int delta)
{
	unsigned i = page_index(pa);
	unsigned short d;

	if (i == 0)
		return (0);
	d = (unsigned short)delta;
	__asm __volatile("lock; xaddw %0,%1"
	    : "+r" (d), "+m" (page_ref[i])
	    :
	    : "memory");
	return ((unsigned)d);		/* the value before the add */
}

/* 中文：递增物理页的 COW 引用计数（原子），超出表范围则忽略 */
/* EN: Atomically increment a physical page's COW reference count, ignoring pages outside the table */
static void
page_ref_inc(unsigned pa)
{

	(void)page_ref_add(pa, 1);
}

/* 中文：释放对物理页的一次引用（原子）：旧值大于 1 只递减，否则清零并放回空闲链表 */
/* EN: Release one reference to a physical page (atomically): decrement when the old value was above 1, otherwise clear it and return the page to the free list */
static void
page_put(unsigned pa)
{
	unsigned old = page_ref_add(pa, -1);

	if (old > 1)
		return;			/* another mapping still uses it */
	page_ref_set(pa, 0);
	page_free(pa);
}

/*
 * Size and map the COW refcount table for this machine.  Called once from
 * main() right after page_alloc_set_floor(), i.e. before the first user
 * page can be shared.  The window is 1 MB (4 GB of RAM, the 32-bit limit);
 * the table costs one byte per page, so a 512 MB guest maps 128 KB and a
 * 4 GB guest maps the full 1 MB.  Pages allocated while the table is being
 * set up are not refcounted (page_ref is still 0), which is exactly right:
 * they are permanent kernel tables and are never COW-shared.
 * 按本机内存为 COW 引用计数表定容并映射。由 main() 在 page_alloc_set_floor()
 * 之后调用一次，早于任何用户页被共享。窗口 1MB（覆盖 4GB，即 32 位上限），
 * 表每页 1 字节：512MB guest 映射 128KB，4GB guest 恰好用满 1MB。建表期间分配
 * 的页不做引用计数（此时 page_ref 仍为 0），这正是期望行为——它们是永不共享
 * 的内核表。
 */
void
page_ref_init(void)
{
	unsigned pages, bytes;

	pages = (physmem > 0) ? (unsigned)physmem : (0x4000000u >> 12);
	if (pages > PAGE_REF_MAX)
		pages = PAGE_REF_MAX;
	bytes = pages * (unsigned)sizeof(unsigned short);
	bytes = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	(void)kern_table_alloc((unsigned)PAGE_REF_VA, bytes);
	page_ref_pages = pages;
	page_ref = (unsigned short *)(unsigned)PAGE_REF_VA;
	printf("usermode: COW refcount %d pages (%d KB, 16-bit) at %x\n",
	    pages, bytes / 1024, PAGE_REF_VA);
}

#define	CD_IMAGE_VA	0xFE400000	/* kernel window for a file read from CD */
static unsigned cd_mapped;		/* bytes mapped at CD_IMAGE_VA */

/*
 * Map (lazily) a kernel window big enough for one image read from the
 * ISO9660 root.  Physical pages come from the bump allocator, so they are
 * contiguous, but we map them one page at a time.
 */
/* 中文：惰性映射一块内核窗口，用于容纳从 ISO9660 根读取的单个映像；超限返回 0，否则返回窗口虚拟地址 */
/* EN: Lazily map a kernel window large enough for one image read from the ISO9660 root; returns 0 if too large, else the window VA */
static unsigned char *
cd_map(unsigned size)
{
	unsigned need = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	unsigned i;

	if (need == 0 || need > 4 * 1024 * 1024)
		return (0);
	for (i = cd_mapped; i < need; i += PAGE_SIZE) {
		unsigned pa = page_alloc();

		pmap_map((vm_offset_t)(CD_IMAGE_VA + i), (vm_offset_t)pa,
		    (vm_offset_t)pa + PAGE_SIZE,
		    VM_PROT_READ | VM_PROT_WRITE);
	}
	cd_mapped = need;
	return ((unsigned char *)CD_IMAGE_VA);
}
static unsigned umask_val = 022;

/* 中文：VFS 物理页分配包装：分配一页并返回其物理地址 */
/* EN: VFS physical page allocation wrapper: allocate one page and return its physical address */
unsigned
vfs_page_alloc(void)
{

	return (page_alloc());
}

/* 中文：VFS 物理页释放包装：归还一页物理内存 */
/* EN: VFS physical page free wrapper: return one physical page to the allocator */
void
vfs_page_free(unsigned pa)
{

	page_free(pa);
}

/*
 * Drop a kernel-window mapping (TMPFS_VA / scratch style).  The PTE lives in
 * kernel_pmap, so clearing it and flushing the TLB is enough.
 */
/* 中文：解除一个内核虚拟地址处的页映射：清 PTE 并刷新 TLB */
/* EN: Unmap the page at a kernel virtual address: clear its PTE and flush the TLB */
void
vfs_page_unmap(unsigned va)
{
	struct pte *pte;

	pte = pmap_pte(kernel_pmap, (vm_offset_t)va);
	if (pte != 0)
		*(int *)pte = 0;
	tlbflush();
}

/* 中文：返回页分配器的统计：已分配、已释放与当前在用页数 */
/* EN: Return page allocator statistics: pages allocated, freed and currently in use */
void
vfs_page_stats(unsigned *nalloc, unsigned *nfreed, unsigned *ninuse)
{

	*nalloc = page_nalloc;
	*nfreed = page_nfree;
	*ninuse = page_ninuse;
}

static int zcomp_reclaim(int n);
static int in_zcomp;
/*
 * S410: set while a user page fault is being handled.
 *
 * page_alloc() reclaims by compressing private anonymous pages of the current
 * process.  Doing that inside the fault path livelocks: the fault allocates a
 * page (triggering reclaim), reclaim compresses the page the handler is about
 * to write or the page the retried instruction is about to touch, the
 * instruction faults again, and the two chase each other.  With 1000 children
 * this wedged the machine at child 417 with CR2 pinned on one stack page.
 * 用户缺页处理期间置位。page_alloc() 的回收会压缩当前进程的私有匿名页，若在
 * 缺页路径里做，就会自锁：缺页分配一页（触发回收），回收又压缩了处理程序即将
 * 写入的页或重试指令要访问的页，指令再次缺页，两者互相追逐。1000 个子进程时
 * 机器就卡在第 417 个，CR2 固定在某一个栈页上。
 */
/*
 * S431: per CPU.  It used to be one global flag, which meant a fault on CPU
 * A also silenced reclaim for a faulting CPU B - and, worse, CPU B clearing
 * the flag when it finished re-enabled reclaim inside CPU A's handler, which
 * is exactly the livelock described above.  Only the CPU that is actually
 * inside its own fault handler may suppress its own reclaim.
 * 改为每 CPU 一份。原来是全局标志：A 核缺页会让 B 核的缺页也失去保护；更糟的是
 * B 核退出处理时把标志清掉，A 核的处理程序里回收又被放开，正是上面那种活锁。
 * 只有真正处在自己缺页处理中的 CPU 才能抑制自己的回收。
 */
static int in_fault_cpu[MAXCPU];
#define	in_fault	(in_fault_cpu[cpu_id()])
static int user_ok(unsigned p, unsigned len);
static int user_ok_w(unsigned p, unsigned len);
static void proc_sched(void);

/*
 * Reclaim watermark, in pages.
 *
 * It used to be a flat `page_ninuse > 4096`, i.e. 16 MB.  On a 512 MB guest
 * that made *every* allocation past 16 MB attempt to compress 8 pages, which
 * turned ordinary fork churn into a compression storm (each fork allocates a
 * page directory, page tables and a kernel stack).  Reclaim is meant for
 * genuine pressure, so the trigger is now three quarters of physical memory,
 * with the old 4096-page floor kept for tiny machines.
 * 回收水位（页）。原来是固定的 `page_ninuse > 4096`（16MB）：512MB guest 一旦用到
 * 16MB，之后每次分配都要压缩 8 页，普通的 fork 抖动就变成压缩风暴（每次 fork
 * 都要分配页目录、页表和内核栈）。回收本该只在真正吃紧时发生，因此改成物理内存
 * 的 3/4，小机器仍保留 4096 页的下限。
 */
#define	RECLAIM_FLOOR	4096u
static int
reclaim_watermark(void)
{
	unsigned hi = (physmem > 0) ? ((unsigned)physmem / 4u * 3u) : 0;

	return ((int)(hi > RECLAIM_FLOOR ? hi : RECLAIM_FLOOR));
}

/* 中文：分配一页物理内存：内存紧张时先尝试压缩回收私有匿名页，否则从空闲链表取或推进 bump 指针 */
/* EN: Allocate a physical page: under memory pressure first try to reclaim private anonymous pages via compression, then pop the free list or bump the pointer */
static unsigned
page_alloc(void)
{
	unsigned pa;

	/*
	 * S430: pop under page_lock.  The reclaim hook stays *outside* it -
	 * zcomp_reclaim() frees pages through page_free(), and taking the lock
	 * there would self-deadlock on a non-recursive spinlock.
	 */
	spin_lock(&page_lock);
	if (page_free_list != 0) {
		unsigned *p = map_page(SCRATCH_PAGE, page_free_list);

		pa = page_free_list;
		page_free_list = *p;
		page_nalloc++;
		page_ninuse++;
		page_ref_set(pa, 1);
		spin_unlock(&page_lock);
		return (pa);
	}
	spin_unlock(&page_lock);

	/*
	 * Memory-pressure hook: before bumping into more RAM, try to compress
	 * a few private anonymous pages.  The compressed pages stay in RAM,
	 * so this trades CPU for headroom instead of going to swap (there is
	 * no swap device in this port).
	 * 内存压力钩子：在继续向物理内存推进之前，先压缩少量进程私有匿名页。
	 * 压缩后的页仍在 RAM 中，用 CPU 换空间，而不是写 swap（本端口没有
	 * swap 设备）。
	 */
	if (page_ninuse > (unsigned)reclaim_watermark() &&
	    !in_zcomp && !in_fault && cur != 0)
		(void)zcomp_reclaim(8);

	spin_lock(&page_lock);
	if (page_free_list != 0) {
		unsigned *p = map_page(SCRATCH_PAGE, page_free_list);

		pa = page_free_list;
		page_free_list = *p;
		page_nalloc++;
		page_ninuse++;
		page_ref_set(pa, 1);
		spin_unlock(&page_lock);
		return (pa);
	}
	pa = next_pa;
	if (pa == 0)
		panic("usermode: page_alloc before page_alloc_set_floor");
	next_pa += PAGE_SIZE;
	/*
	 * Ceiling comes from the boot memory plan (S411), which uses 64-bit
	 * arithmetic on the BIOS/E801 page count.  The old `physmem << 12`
	 * expression wrapped to 0 on a 4 GB guest and disabled the check.
	 * 上界来自启动内存规划（S411），用 64 位运算处理 BIOS/E801 页数；
	 * 旧的 `physmem << 12` 在 4GB guest 上会回绕成 0，等于取消检查。
	 */
	if ((unsigned long long)next_pa >= (unsigned long long)mem_phys_limit)
		panic("usermode: out of physical memory");
	page_nalloc++;
	page_ninuse++;
	page_ref_set(pa, 1);
	spin_unlock(&page_lock);
	return (pa);
}

/* 中文：释放一页物理内存：忽略非法地址，把页压入 LIFO 空闲链表并更新引用计数与统计 */
/* EN: Free a physical page: ignore invalid addresses, push it on the LIFO free list and update refcount and statistics */
static void
page_free(unsigned pa)
{
	unsigned *p;

	if (pa == 0 || (pa & (PAGE_SIZE - 1)) != 0)
		return;
	spin_lock(&page_lock);
	p = map_page(SCRATCH_PAGE, pa);
	*p = page_free_list;
	page_free_list = pa;
	page_ref_set(pa, 0);
	page_nfree++;
	if (page_ninuse > 0)
		page_ninuse--;
	spin_unlock(&page_lock);
}

/*
 * Physical-page accounting snapshot, printed when the kernel goes idle.
 * The interesting number is page_ninuse: it must stay flat across repeated
 * fork/exit churn (kernel stacks, page tables and TCBs are all recycled),
 * and `next free' only moves when the bump allocator really had to grow.
 * 内核转入空闲时打印的物理页账目。关键是 page_ninuse：反复 fork/exit 之后它
 * 必须保持平稳（内核栈、页表、TCB 都会被回收），只有 bump 分配器真正扩张时
 * “next free” 才会前进。
 */
static void
mem_report(void)
{

	printf("mem: %d pages in use, %d allocated, next free %x\n",
	    page_ninuse, page_nalloc, next_pa);
}

/*
 * Release a whole user address space: every user page, every page table and
 * the page directory itself.  The caller must have switched CR3 away from
 * this directory first.  Entries at/above PPDROFF are kernel mappings and
 * are never touched.
 */
/* 中文：释放整个用户地址空间：逐个释放用户页、压缩页槽、页表与页目录，内核映射区不碰；调用前须已切换 CR3 */
/* EN: Release a whole user address space: free every user page, compressed-page slot, page table and the directory; kernel mappings untouched; caller must have switched CR3 away first */
static void
free_address_space(unsigned pdpa)
{
	unsigned *pd = map_page(SCRATCH_PD, pdpa);
	int i, j;

	pmap_lock_enter();
	for (i = 0; i < USER_PDE_LIMIT; i++) {
		unsigned *pt;

		if ((pd[i] & PG_V) == 0)
			continue;
		pt = map_page(SCRATCH_PT, pd[i] & PG_FRAME);
		for (j = 0; j < PAGE_SIZE / 4; j++) {
			if (pt[j] & PG_ZCOMP) {
				/* compressed page: release its pool slot */
				zcomp_free_slot((pt[j] & PG_FRAME) >> 12);
				pt[j] = 0;
			} else if (pt[j] & PG_V)
				page_put(pt[j] & PG_FRAME);
		}
		page_free(pd[i] & PG_FRAME);
		pd[i] = 0;
	}
	page_free(pdpa);
	pmap_lock_exit();
}

/* 中文：把物理页 pa 映射可读写到内核临时虚拟地址 va，并返回该虚拟地址 */
/* EN: Map physical page pa read-write at the kernel scratch virtual address va and return that address */
static void *
map_page(unsigned va, unsigned pa)
{

	pmap_map((vm_offset_t)va, (vm_offset_t)pa,
	    (vm_offset_t)pa + PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE);
	return ((void *)va);
}

/* 中文：把物理页 pa 映射到临时窗口并整页清零 */
/* EN: Map physical page pa into the scratch window and zero the entire page */
static void
zero_page(unsigned pa)
{
	char *p = map_page(SCRATCH_PAGE, pa);
	int i;

	for (i = 0; i < PAGE_SIZE; i++)
		p[i] = 0;
}

/* 中文：创建用户页目录：分配一页并清零，仅共享内核高端与 proc0 内核栈映射，返回其物理地址 */
/* EN: Create a user page directory: allocate and zero a page, share only the kernel high-half and proc0 stack mappings, return its physical address */
static unsigned
user_pd_create(void)
{
	unsigned pdpa;
	unsigned *pd;
	int i;

	pmap_lock_enter();
	pdpa = page_alloc();
	pd = map_page(SCRATCH_PD, pdpa);

	for (i = 0; i < PAGE_SIZE / 4; i++)
		pd[i] = 0;
	/*
	 * Share the kernel windows: 0x3f6 was the original recursive-PTD
	 * window, S410's tables live in 0x3fb, and S411 adds 0x3ec (process
	 * table at 0xfb000000) and 0x3f0..0x3f3 (16 MB kernel stack pool at
	 * 0xfc000000).  A user page directory that did not get those PDEs
	 * would fault on its first user thread or process.
	 * 共享内核窗口：0x3f6 是原有递归 PTD 窗口，0x3fb 是 S410 的表；
	 * S411 增加 0x3ec（0xfb000000 进程表）与 0x3f0..0x3f3（0xfc000000 起的
	 * 16MB 内核栈池）。用户页目录漏拷任何一个，第一次创建用户线程就会缺页。
	 */
	for (i = 0x3ec; i <= 0x3fd; i++)
		pd[i] = ((unsigned *)kernel_pmap->pm_pdir)[i];
	pmap_lock_exit();
	return (pdpa);
}

/*
 * Map one user page in a *specific* page directory.  The caller passes the
 * directory because after a fork/exec SCRATCH_PD no longer points at the
 * process we are editing (that bug silently unmapped the parent's heap).
 */
/* 中文：在指定页目录中把用户虚拟地址 va 映射到物理页 pa（始终可写、用户态可访问），必要时分配页表 */
/* EN: Map user virtual address va to physical page pa in a specific page directory (always writable, user accessible), allocating a page table if needed */
static void
user_map(unsigned pdpa, unsigned va, unsigned pa)
{
	unsigned pdi = va >> 22, pti = (va >> 12) & 0x3ff;
	unsigned *pd, *pt;
	unsigned ptpa;
	int i;

	pmap_lock_enter();
	pd = map_page(SCRATCH_PD, pdpa);
	if ((pd[pdi] & PG_V) == 0) {
		ptpa = page_alloc();
		pt = map_page(SCRATCH_PT, ptpa);
		for (i = 0; i < PAGE_SIZE / 4; i++)
			pt[i] = 0;
		pd[pdi] = ptpa | PG_V | PG_RW | PG_u;
	} else {
		ptpa = pd[pdi] & PG_FRAME;
		pt = map_page(SCRATCH_PT, ptpa);
	}
	pt[pti] = pa | PG_V | PG_RW | PG_u;
	pmap_lock_exit();
}

/*
 * S17: map/unmap a single user page with explicit write permission and
 * inspect an existing PTE.  These are used by mmap()/munmap(); user_map()
 * above stays the "always writable" helper used by exec/brk/stack.
 */
/* 中文：查出用户地址 va 对应页表项指针，通过 ptp 回传；无有效页表返回 0 并把 *ptp 置空 */
/* EN: Look up the page-table entry pointer for user address va via ptp; returns 0 and sets *ptp to NULL when no page table exists */
static unsigned
user_pte_ptr(unsigned pdpa, unsigned va, unsigned **ptp)
{
	unsigned pdi = va >> 22, pti = (va >> 12) & 0x3ff;
	unsigned *pd = map_page(SCRATCH_PD, pdpa);
	unsigned *pt;

	if ((pd[pdi] & PG_V) == 0) {
		*ptp = 0;
		return (0);
	}
	pt = map_page(SCRATCH_PT, pd[pdi] & PG_FRAME);
	*ptp = pt;
	return (pti);
}

/* 中文：在指定页目录中把 va 映射到 pa 并按 prot 设置写权限，供 mmap 使用（区别于始终可写的 user_map） */
/* EN: Map va to pa in a specific page directory with write permission per prot, used by mmap (unlike the always-writable user_map) */
static void
user_map_prot(unsigned pdpa, unsigned va, unsigned pa, int prot)
{
	unsigned pdi = va >> 22, pti = (va >> 12) & 0x3ff;
	unsigned *pd, *pt;
	unsigned ptpa;
	int i;

	pmap_lock_enter();
	pd = map_page(SCRATCH_PD, pdpa);
	if ((pd[pdi] & PG_V) == 0) {
		ptpa = page_alloc();
		pt = map_page(SCRATCH_PT, ptpa);
		for (i = 0; i < PAGE_SIZE / 4; i++)
			pt[i] = 0;
		pd[pdi] = ptpa | PG_V | PG_RW | PG_u;
	} else {
		ptpa = pd[pdi] & PG_FRAME;
		pt = map_page(SCRATCH_PT, ptpa);
	}
	pt[pti] = pa | PG_V | PG_u |
	    ((prot & PROT_WRITE) ? PG_RW : 0);
	pmap_lock_exit();
}

/*
 * S431: install pa at va only when the PTE is still absent.  Two threads of
 * one process can take a fault on the same not-present page at the same
 * time (the per-thread stacks are per thread, but the heap is shared); with
 * a plain user_map() the second writer overwrote the first PTE and leaked a
 * page on every such race.  Returns 1 when the mapping was installed; 0
 * means somebody else got there first and the caller must free pa.
 * 仅在 PTE 仍为 absent 时安装 pa。同一进程的两个线程可能同时对同一个未映射页
 * 缺页（线程栈各一份，但堆是共享的）；用普通 user_map() 时后写者覆盖先写者的
 * PTE，每次竞争都泄漏一页。安装成功返回 1；返回 0 表示别人先到，调用方须
 * 释放 pa。
 */
static int
user_map_if_absent(unsigned pdpa, unsigned va, unsigned pa, int prot)
{
	unsigned pdi = va >> 22, pti = (va >> 12) & 0x3ff;
	unsigned *pd, *pt;
	unsigned ptpa;
	int i, installed = 0;

	pmap_lock_enter();
	pd = map_page(SCRATCH_PD, pdpa);
	if ((pd[pdi] & PG_V) == 0) {
		ptpa = page_alloc();
		pt = map_page(SCRATCH_PT, ptpa);
		for (i = 0; i < PAGE_SIZE / 4; i++)
			pt[i] = 0;
		pd[pdi] = ptpa | PG_V | PG_RW | PG_u;
	} else {
		pt = map_page(SCRATCH_PT, pd[pdi] & PG_FRAME);
	}
	if ((pt[pti] & PG_V) == 0) {
		pt[pti] = pa | PG_V | PG_u |
		    ((prot & PROT_WRITE) ? PG_RW : 0);
		installed = 1;
	}
	pmap_lock_exit();
	return (installed);
}

/* 中文：解除用户地址 va 的映射：清对应页表项并刷新 TLB，供 munmap 使用 */
/* EN: Unmap user address va: clear its page-table entry and flush the TLB, used by munmap */
static void
user_unmap(unsigned pdpa, unsigned va)
{
	unsigned *pt;
	unsigned pti;

	pmap_lock_enter();
	pti = user_pte_ptr(pdpa, va, &pt);
	if (pt != 0) {
		pt[pti] = 0;
		tlbflush();
	}
	pmap_lock_exit();
}

/*
 * 中文：S300 KP-7 桥接——把物理页映射/解除映射到任意进程地址空间。
 * kipc.c 调用它们；包装静态 mmap 帮助函数，使共享页逻辑无需复制页表代码。
 * EN: S300 KP-7 bridge: map/unmap a physical page into an arbitrary process
 * address space.  kipc.c calls these; they wrap the static mmap helpers so
 * the shared-page logic can live outside usermode.c without duplicating
 * page-table code.
 */
void
user_map_shared(unsigned pdpa, unsigned va, unsigned pa, int prot)
{

	user_map_prot(pdpa, va & ~(PAGE_SIZE - 1), pa, prot);
	/* 中文：若映射进的是当前进程地址空间，重载 CR3 刷新 TLB，使新映射立即生效；
	 * 这与 mmap/uring 路径一致。映射到别的进程时不刷（对方切换时会自然加载新页表）。
	 * EN: If the mapping targets the current process, reload CR3 so the new mapping
	 * takes effect immediately (same as the mmap/uring paths).  For a different
	 * process we do not flush here; it will pick up the new page table on switch. */
	if (cur != 0 && cur->uk_pd == pdpa)
		hal_address_space_activate(cur->uk_pd);
}

/* 中文：解除由 user_map_shared 建立的映射 */
/* EN: Undo a mapping created by user_map_shared. */
void
user_unmap_shared(unsigned pdpa, unsigned va)
{

	user_unmap(pdpa, va & ~(PAGE_SIZE - 1));
	/* 中文：同样在映射属于当前进程时刷新 TLB（user_unmap 的 tlbflush 之外再保险）。
	 * EN: Likewise reload CR3 when the mapping belongs to the current process. */
	if (cur != 0 && cur->uk_pd == pdpa)
		hal_address_space_activate(cur->uk_pd);
}

/* 中文：S300 KP-7 桥接——把物理页暂映射到内核 scratch 窗口，返回可访问的内核虚址；
 * 供生产者在发送前填充共享页内容。调用方须先使用再让窗口被下一次调用覆盖。
 * EN: S300 KP-7 bridge: temporarily map a physical page into the kernel scratch
 * window and return an accessible kernel VA, so the producer can fill a shared
 * page before sending.  The caller must use it before the next call overwrites
 * the window.
 */
void *
user_scratch_window(unsigned pa)
{

	if (pa == 0)
		return (0);
	return (map_page(SCRATCH_PAGE, pa));
}

/* 中文：查出用户地址 va 当前映射的物理页，未映射或不存在返回 0 */
/* EN: Return the physical page currently mapped at user address va, or 0 if unmapped/absent */
static unsigned
user_getpa(unsigned pdpa, unsigned va)
{
	unsigned *pt;
	unsigned pti = user_pte_ptr(pdpa, va, &pt);

	if (pt == 0 || (pt[pti] & PG_V) == 0)
		return (0);
	return (pt[pti] & PG_FRAME);
}

/* ---------------------------------------------------------------- mmap */

/* 中文：为进程 p 分配一个空闲的 VMA 槽位，用完返回 0 */
/* EN: Allocate a free VMA slot for process p, returning 0 when none is available */
static struct kvma *
vma_alloc(struct process *p)
{
	int i;

	for (i = 0; i < NMAP; i++)
		if (!p->uk_p_vma[i].used)
			return (&p->uk_p_vma[i]);
	return (0);
}

/* 中文：查找包含地址 addr 的 VMA 槽位，找到返回指针，否则返回 0 */
/* EN: Find the VMA slot containing address addr, returning its pointer or 0 if none */
static struct kvma *
vma_find(struct process *p, unsigned addr)
{
	int i;

	for (i = 0; i < NMAP; i++)
		if (p->uk_p_vma[i].used && addr >= p->uk_p_vma[i].start &&
		    addr < p->uk_p_vma[i].start + p->uk_p_vma[i].len)
			return (&p->uk_p_vma[i]);
	return (0);
}

/* 中文：检查 [start, start+len) 是否与进程 p 的任何已有 VMA 区间重叠 */
/* EN: Check whether [start, start+len) overlaps any existing VMA of process p */
static int
vma_overlaps(struct process *p, unsigned start, unsigned len)
{
	int i;

	for (i = 0; i < NMAP; i++) {
		struct kvma *v = &p->uk_p_vma[i];

		if (!v->used)
			continue;
		if (start < v->start + v->len && v->start < start + len)
			return (1);
	}
	return (0);
}

/*
 * mmap(addr, len, prot, flags, fd): returns the user address or -errno.
 */
/* 中文：mmap 核心实现：校验参数与文件描述符，确定映射起始地址并登记一个 VMA；页面在首次访问时才分配 */
/* EN: mmap core: validate arguments and fd, choose the mapping start address and register a VMA; pages are allocated lazily on first touch */
static int
do_mmap(unsigned addr, unsigned len, int prot, int flags, int fd)
{
	struct kvma *v;
	struct vnode *vp = 0;
	unsigned vid = 0, start;
	int share = flags & (MAP_SHARED | MAP_PRIVATE);
	int file = (flags & MAP_ANON) == 0;

	if (len == 0 || len > MMAP_MAXLEN)
		return (-EINVAL);
	len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (share == 0 || share == (MAP_SHARED | MAP_PRIVATE))
		return (-EINVAL);
	if ((flags & MAP_FIXED) && (addr & (PAGE_SIZE - 1)))
		return (-EINVAL);
	if (file) {
		if (fd < 0 || fd >= NFD || !cur->uk_fds[fd].used ||
		    cur->uk_fds[fd].vp == 0 ||
		    cur->uk_fds[fd].vp->type != VREG)
			return (-EBADF);
		vp = cur->uk_fds[fd].vp;
		if (share == MAP_SHARED && vp->ops->write == 0 && prot &
		    PROT_WRITE)
			return (-EROFS);
		vn_ref(vp);
		vid = pagecache_key(vp);
	}
	if (flags & MAP_FIXED) {
		start = addr;
		if (start < mmap_base || start + len > mmap_limit ||
		    vma_overlaps(cur, start, len)) {
			if (vp)
				vn_put(vp);
			return (-EINVAL);
		}
	} else {
		start = (cur->uk_p_mmap_base + PAGE_SIZE - 1) &
		    ~(PAGE_SIZE - 1);
		if (start + len > mmap_limit) {
			if (vp)
				vn_put(vp);
			return (-ENOMEM);
		}
	}
	v = vma_alloc(cur);
	if (v == 0) {
		if (vp)
			vn_put(vp);
		return (-ENOMEM);
	}
	/*
	 * Pages are populated on the first fault by mmap_fault(), not here:
	 * mmap only records the VMA so a large mapping costs no memory until
	 * it is touched (os.md mechanism 3, demand paging).
	 * 页面在首次访问时由 mmap_fault() 填充；mmap 只记录 VMA，大映射在
	 * 未触碰前不占物理页（os.md 机制 3，按需分页）。
	 */
	bzero(v, sizeof(*v));
	v->used = 1;
	v->start = start;
	v->len = len;
	v->off = 0;
	v->vid = vid;
	v->vp = vp;
	v->prot = prot;
	v->flags = flags;
	if (!(flags & MAP_FIXED))
		cur->uk_p_mmap_base = start + len;
	return ((int)start);
}

/* 中文：按需填充 mmap 映射页：匿名映射分配零页，共享文件映射页缓存页本身，私有映射拷贝一份并解除页缓存引用 */
/* EN: Demand-fill an mmap page: anonymous mappings get a zeroed page, shared file mappings map the cache page itself, private mappings copy it and drop the cache reference */
static int
mmap_fault(unsigned va)
{
	struct kvma *v = vma_find(cur, va);
	unsigned idx, pa;
	unsigned char *src;

	if (v == 0)
		return (0);
	idx = (v->off + (va - v->start)) / PAGE_SIZE;
	if (v->vp == 0) {
		/* anonymous mapping: zero page on first touch */
		pa = page_alloc();
		zero_page(pa);
		/* S431: a concurrent touch of the same page may have won */
		if (!user_map_if_absent(cur->uk_pd, va, pa, v->prot))
			page_free(pa);
		return (1);
	}
	pa = pagecache_get(v->vp, idx, &src);
	if (pa == 0)
		return (0);
	if (v->flags & MAP_SHARED) {
		/* shared file page: map the page-cache page itself */
		user_map_prot(cur->uk_pd, va, pa, v->prot);
		return (1);
	}
	/* private file page: copy the cache page into a process-owned page */
	{
		unsigned pa2 = page_alloc();
		char *dst = map_page(SCRATCH_PAGE, pa2);

		zero_page(pa2);
		bcopy(src, dst, PAGE_SIZE);
		/* S431: idempotent install; on a lost race drop our copy but
		 * still give back the page-cache reference taken above. */
		if (!user_map_if_absent(cur->uk_pd, va, pa2, v->prot))
			page_free(pa2);
		pagecache_unref(v->vp, idx);
	}
	return (1);
}

/* 中文：压缩页缺页重填：把 PG_ZCOMP 压缩槽的页解压回物理页，重装 PTE 并激活地址空间；失败返回 0 */
/* EN: Refill a compressed-page fault: decompress a PG_ZCOMP slot back into a physical page, reinstall the PTE and activate the address space; returns 0 on failure */
static int
zcomp_refault(unsigned va)
{
	unsigned *pt;
	unsigned pti, slot, pa;
	char *dst;

	/*
	 * S431: check the compressed PTE and install the new one under the
	 * pmap lock.  Without it two CPUs faulting the same slot could both
	 * decompress it and the loser would overwrite the winner's PTE,
	 * leaking a page and (because zcomp_load() frees the slot) losing the
	 * data of the mapping that had just been made present.
	 * 在 pmap 锁内检查压缩 PTE 并安装新 PTE。否则两个 CPU 同时缺页到同一个
	 * 槽时会各解压一遍，后写者覆盖先写者的 PTE，泄漏一页；而 zcomp_load()
	 * 已经释放了槽，刚变成 present 的那个映射的数据也随之丢失。
	 */
	pmap_lock_enter();
	pti = user_pte_ptr(cur->uk_pd, va & ~(PAGE_SIZE - 1), &pt);
	if (pt == 0 || (pt[pti] & PG_V) != 0 ||
	    (pt[pti] & PG_ZCOMP) == 0) {
		pmap_lock_exit();
		return (0);
	}
	slot = (pt[pti] & PG_FRAME) >> 12;
	pa = page_alloc();
	dst = map_page(SCRATCH_PAGE, pa);
	if (zcomp_load(slot, dst) < 0) {
		page_put(pa);
		pt[pti] = 0;
		pmap_lock_exit();
		return (0);
	}
	pt[pti] = pa | PG_V | PG_RW | PG_u;
	pmap_lock_exit();
	hal_address_space_activate(cur->uk_pd);
	return (1);
}

/*
 * Compress up to n private anonymous pages of the current process.  Only
 * present, writable, user, non-COW pages outside a VMA are candidates;
 * file-backed and shared pages are left alone (they can be re-read).
 * 压缩当前进程最多 n 个私有匿名页。候选必须 present、可写、用户态、
 * 非 COW，且不在 VMA 内；文件页与共享页不压缩（可以重新读取）。
 */
/*
 * User-space asynchronous I/O ABI (os.md mechanism 5).
 *
 * submit returns a token immediately; the VFS aio worker runs the shared
 * read_iter/write_iter backend and completes through a deferred softirq callback.
 * A read completes into the per-slot kernel page and is copied out before
 * the waiting process is woken; a write copies the user buffer in at submit.
 * 用户态异步 I/O ABI：submit 立即返回 token；VFS aio worker 执行共享的
 * read_iter/write_iter 后端，并通过延迟软中断回调完成。读先落到每槽内核
 * 页，在唤醒等待进程前拷回用户；写在提交时先把用户缓冲拷入。
 */
#define	NAIO		8
#define	AIO_VA		0xFE500000

struct aio_slot {
	int		 a_used;
	int		 a_token;
	int		 a_op;
	int		 a_pid;
	int		 a_done;
	int		 a_woken;
	int		 a_result;
	unsigned	 a_ubuf;
	unsigned	 a_len;
	unsigned	 a_off;
	unsigned a_user_data;	/* ring CQE payload */
	void		*a_ring;	/* struct uring_ring * when from a ring */
	unsigned char	*a_kbuf;
	struct process	*a_proc;
	struct kiocb	 a_ki;
};

#define	NURING		2
#define	URING_KVA	0xFE700000
#define	URING_UVA	0x31000000

struct uring_ring {
	int		 r_used;
	struct process	*r_proc;
	unsigned	 r_pd;
	unsigned	 r_uva;
	unsigned	 r_pa;
	unsigned	 r_entries;
	int		 r_wait_pid;
	int		 r_wait_result;
	int		 r_expected;	/* completions this batch must produce */
	int		 r_done;	/* completions seen for this batch */
	struct uring_header *r_hdr;
	struct uring_sqe *r_sq;
	struct uring_cqe *r_cq;
};

static struct uring_ring rings[NURING];
static int		uring_inited;

static void uring_wake(struct uring_ring *r);
static int uring_cqe_push(struct uring_ring *r, unsigned user_data, int res);

static struct aio_slot	aios[NAIO];
static int		aio_next_token;

static void aio_soft(void *arg);

/* 中文：初始化用户态异步 I/O：清零 aio 槽、为每个槽分配并映射内核缓冲页，注册延迟软中断处理 */
/* EN: Initialize userland async I/O: clear aio slots, allocate and map a kernel buffer page per slot, and register the deferred softirq handler */
void
aio_user_init(void)
{
	int i;
	unsigned pa;

	bzero(aios, sizeof(aios));
	aio_next_token = 1;
	for (i = 0; i < NAIO; i++) {
		pa = vfs_page_alloc();
		pmap_map((vm_offset_t)(AIO_VA + i * PAGE_SIZE),
		    (vm_offset_t)pa, (vm_offset_t)pa + PAGE_SIZE,
		    VM_PROT_READ | VM_PROT_WRITE);
		aios[i].a_kbuf = (unsigned char *)(AIO_VA + i * PAGE_SIZE);
	}
	printf("aio: %d submission slots at %x\n", NAIO, AIO_VA);
}

/* 中文：异步 I/O 完成回调：记录结果，读操作把内核缓冲拷回用户，若来自 ring 则压 CQE 并唤醒，否则触发 aio_soft */
/* EN: Async I/O completion callback: record the result, copy kernel buffer back to user for reads, push a CQE and wake if from a ring, else raise aio_soft */
static void
aio_complete_kiocb(struct kiocb *ki, int r)
{
	struct aio_slot *s = (struct aio_slot *)ki->ki_private;
	struct process *save = cur;
	unsigned savepd = hal_address_space_current();

	if (s == 0)
		return;
	s->a_result = r;
	s->a_done = 1;
	if (s->a_op == KI_READ && r > 0) {
		cur = s->a_proc;
		if (cur != 0) {
			hal_address_space_activate(cur->uk_pd);
			if (user_ok_w(s->a_ubuf, (unsigned)r))
				bcopy(s->a_kbuf, (void *)s->a_ubuf,
				    (unsigned)r);
		}
		cur = save;
		hal_address_space_activate(savepd);
	}
	if (s->a_ring != 0) {
		struct uring_ring *rg = (struct uring_ring *)s->a_ring;
		(void)uring_cqe_push(rg, s->a_user_data, r);
		rg->r_done++;
		uring_wake(rg);
		return;
	}
	softirq_defer(aio_soft, s);
}

/* 中文：aio 完成软中断：找到等待该 token 的进程，把结果写入其 regs.eax、置为可运行并释放槽位；ring 完成不走此路 */
/* EN: aio completion softirq: find the process waiting on the token, write the result into regs.eax, make it runnable and free the slot; ring completions skip this */
static void
aio_soft(void *arg)
{
	struct aio_slot *s = (struct aio_slot *)arg;
	struct process *p = 0;
	int i;

	if (s == 0 || s->a_woken)
		return;
	if (s->a_ring != 0)
		return;			/* ring completion woke the caller */
	for (i = 0; i < NPROC; i++)
		if (procs[i].uk_state != P_FREE && procs[i].uk_pid == s->a_pid) {
			p = &procs[i];
			break;
		}
	if (p != 0 && p->uk_state == P_BLOCKED &&
	    p->uk_aio_wait_token == s->a_token) {
		p->uk_aio_wait_token = 0;
		p->uk_regs.eax = (unsigned)s->a_result;
		proc_wake(p);
		vn_put(s->a_ki.ki_vp);
		s->a_used = 0;
		s->a_woken = 1;
	}
}

/* 中文：提交一次异步读写：校验 fd、长度与用户缓冲，占用一个 aio 槽，写时拷入用户数据并发起异步 VFS 读写，成功返回 token */
/* EN: Submit one async read/write: validate fd, length and user buffer, reserve an aio slot, copy in user data for writes and start async VFS I/O, returning a token on success */
static int
aio_submit_sys(int fd, int op, unsigned ubuf, unsigned len, unsigned off)
{
	struct aio_slot *s = 0;
	struct vnode *vp;
	int i, token, r;

	if (fd < 0 || fd >= NFD || !cur->uk_fds[fd].used ||
	    cur->uk_fds[fd].vp == 0)
		return (-EBADF);
	vp = cur->uk_fds[fd].vp;
	if (len == 0 || len > PAGE_SIZE)
		return (-EINVAL);
	if (op == KI_READ ? !user_ok_w(ubuf, len) : !user_ok(ubuf, len))
		return (-EFAULT);
	for (i = 0; i < NAIO; i++)
		if (!aios[i].a_used) {
			s = &aios[i];
			break;
		}
	if (s == 0)
		return (-EAGAIN);
	{
		unsigned char *kbuf = s->a_kbuf;

		bzero(s, sizeof(*s));
		s->a_kbuf = kbuf;
	}
	s->a_used = 1;
	token = aio_next_token++;
	if (aio_next_token <= 0)
		aio_next_token = 1;
	s->a_token = token;
	s->a_op = op;
	s->a_pid = cur->uk_pid;
	s->a_proc = cur;
	s->a_ubuf = ubuf;
	s->a_len = len;
	s->a_off = off;
	if (op == KI_WRITE) {
		if (ucopyin((void *)ubuf, s->a_kbuf, len) < 0) {
			s->a_used = 0;
			return (-EFAULT);
		}
	} else
		bzero(s->a_kbuf, len);

	s->a_ki.ki_vp = vp;
	s->a_ki.ki_op = op;
	s->a_ki.ki_flags = KI_ASYNC;
	s->a_ki.ki_off = off;
	s->a_ki.ki_buf = s->a_kbuf;
	s->a_ki.ki_len = len;
	s->a_ki.ki_complete = aio_complete_kiocb;
	s->a_ki.ki_private = s;
	vn_ref(vp);
	r = (op == KI_READ) ? vfs_read_async(&s->a_ki) :
	    vfs_write_async(&s->a_ki);
	if (r != EIOCBQUEUED) {
		vn_put(vp);
		s->a_used = 0;
		return (r < 0 ? r : -EIO);
	}
	return (token);
}

/* 中文：等待异步 I/O token：已完成则立即返回结果并释放槽，否则保存上下文、阻塞进程并调度，由 aio_soft 唤醒 */
/* EN: Wait for an async I/O token: return the result and free the slot if done, else save context, block the process and schedule, to be woken by aio_soft */
static int
aio_wait_sys(int token, struct sysregs *r)
{
	struct aio_slot *s;
	int i, res;

	for (i = 0; i < NAIO; i++)
		if (aios[i].a_used && aios[i].a_token == token) {
			s = &aios[i];
			goto found;
		}
	return (-EINVAL);
found:
	if (s->a_done) {
		res = s->a_result;
		vn_put(s->a_ki.ki_vp);
		s->a_used = 0;
		return (res);
	}
	/*
	 * Block the calling process; aio_soft() writes the result into
	 * p->uk_regs.eax and makes it runnable again.
	 * 阻塞调用进程；aio_soft() 把结果写进 p->uk_regs.eax 并让它重新可运行。
	 */
	cur->uk_aio_wait_token = token;
	cur->uk_regs = *r;
	kthread_block_tcb(0);
	proc_sched();
	return (0);
}

/* 中文：轮询异步 I/O token：完成则返回结果并释放槽，未完成返回 -EAGAIN，token 无效返回 -EINVAL */
/* EN: Poll an async I/O token: return result and free the slot if done, -EAGAIN if still pending, -EINVAL for an invalid token */
static int
aio_poll_sys(int token)
{
	struct aio_slot *s;
	int i, res;

	for (i = 0; i < NAIO; i++)
		if (aios[i].a_used && aios[i].a_token == token) {
			s = &aios[i];
			if (!s->a_done)
				return (-EAGAIN);
			res = s->a_result;
			vn_put(s->a_ki.ki_vp);
			s->a_used = 0;
			return (res);
		}
	return (-EINVAL);
}

/* 中文：向 io_uring 环的完成队列压入一个 CQE（user_data/result），队列满则增加 overflow 并返回 0 */
/* EN: Push a CQE (user_data/result) onto an io_uring ring's completion queue; on overflow increment the counter and return 0 */
static int
uring_cqe_push(struct uring_ring *rg, unsigned user_data, int result)
{
	struct uring_cqe *cq;

	if ((rg->r_hdr->cq_tail - rg->r_hdr->cq_head) >= rg->r_entries) {
		rg->r_hdr->cq_overflow++;
		return (0);
	}
	cq = &rg->r_cq[rg->r_hdr->cq_tail % rg->r_entries];
	cq->cq_user_data = user_data;
	cq->cq_result = result;
	cq->cq_flags = 0;
	rg->r_hdr->cq_tail++;
	return (1);
}

/* 中文：唤醒等待 io_uring 的进程：当本批完成数达到期望值时把结果写入其 regs.eax 并置为可运行 */
/* EN: Wake the process waiting on an io_uring ring: when this batch's completions reach the expected count, write the result into regs.eax and make it runnable */
static void
uring_wake(struct uring_ring *rg)
{
	struct process *p = 0;
	int i;

	if (rg->r_wait_pid == 0)
		return;
	if (rg->r_done < rg->r_expected)
		return;
	for (i = 0; i < NPROC; i++)
		if (procs[i].uk_state != P_FREE && procs[i].uk_pid == rg->r_wait_pid) {
			p = &procs[i];
			break;
		}
	if (p != 0 && p->uk_state == P_BLOCKED) {
		p->uk_uring_wait = 0;
		p->uk_regs.eax = (unsigned)rg->r_wait_result;
		proc_wake(p);
	}
	rg->r_wait_pid = 0;
}

/* 中文：创建 io_uring 环：分配空闲环与一页共享内存，映射内核/用户视图，初始化 SQE/CQ 头并返回用户虚拟地址 */
/* EN: Set up an io_uring ring: allocate a free ring and a shared page, map kernel/user views, initialize SQE/CQ headers and return the user VA */
static int
uring_setup_sys(unsigned entries)
{
	struct uring_ring *rg = 0;
	struct uring_header *h;
	unsigned pa, kva, uva;
	int i;

	if (!uring_inited) {
		bzero(rings, sizeof(rings));
		uring_inited = 1;
	}
	if (entries == 0 || entries > URING_ENTRIES)
		entries = URING_ENTRIES;
	for (i = 0; i < NURING; i++)
		if (!rings[i].r_used) {
			rg = &rings[i];
			break;
		}
	if (rg == 0)
		return (-ENOMEM);
	pa = vfs_page_alloc();
	kva = URING_KVA + (unsigned)i * PAGE_SIZE;
	pmap_map((vm_offset_t)kva, (vm_offset_t)pa,
	    (vm_offset_t)pa + PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE);
	bzero((void *)kva, PAGE_SIZE);
	uva = URING_UVA + (unsigned)i * 0x10000;
	user_map_prot(cur->uk_pd, uva, pa, PROT_READ | PROT_WRITE);
	hal_address_space_activate(cur->uk_pd);

	rg->r_used = 1;
	rg->r_proc = cur;
	rg->r_pd = cur->uk_pd;
	rg->r_uva = uva;
	rg->r_pa = pa;
	rg->r_entries = entries;
	rg->r_wait_pid = 0;
	rg->r_hdr = (struct uring_header *)kva;
	rg->r_sq = (struct uring_sqe *)(kva + URING_OFF_SQ);
	rg->r_cq = (struct uring_cqe *)(kva + URING_OFF_CQ);
	h = rg->r_hdr;
	h->magic = URING_MAGIC;
	h->entries = entries;
	h->sq_head = h->sq_tail = 0;
	h->cq_head = h->cq_tail = 0;
	printf("uring: ring %d entries %d uva %x\n", i, entries, uva);
	return ((int)uva);
}

/* 中文：进入/提交 io_uring：取出提交队列中的 SQE 逐个发起 aio 操作并关联到环，可选阻塞等待本批完成，返回提交数 */
/* EN: Enter/submit an io_uring: take SQEs from the submission queue, start aio operations tied to the ring, optionally block waiting for the batch, returning the submitted count */
static int
uring_enter_sys(unsigned to_submit, int wait, struct sysregs *r)
{
	struct uring_ring *rg = 0;
	struct uring_header *h;
	unsigned avail, submitted = 0;
	int i;

	for (i = 0; i < NURING; i++)
		if (rings[i].r_used && rings[i].r_proc == cur) {
			rg = &rings[i];
			break;
		}
	if (rg == 0)
		return (-EINVAL);
	h = rg->r_hdr;
	if (h->magic != URING_MAGIC)
		return (-EINVAL);
	avail = h->sq_tail - h->sq_head;
	if (to_submit < avail)
		avail = to_submit;
	rg->r_expected = 0;
	rg->r_done = 0;
	while (avail > 0) {
		struct uring_sqe *sqe;
		int token, j;

		if ((h->cq_tail - h->cq_head) >= rg->r_entries) {
			h->sq_dropped += avail;
			break;
		}
		sqe = &rg->r_sq[h->sq_head % rg->r_entries];
		token = aio_submit_sys((int)sqe->sq_fd, (int)sqe->sq_op,
		    sqe->sq_buf, sqe->sq_len, sqe->sq_off);
		if (token > 0) {
			for (j = 0; j < NAIO; j++)
				if (aios[j].a_used &&
				    aios[j].a_token == token) {
					aios[j].a_ring = rg;
					aios[j].a_user_data =
					    sqe->sq_user_data;
					break;
				}
		} else {
			(void)uring_cqe_push(rg, sqe->sq_user_data, token);
		}
		h->sq_head++;
		submitted++;
		avail--;
	}
	if (wait && rg->r_done < submitted) {
		rg->r_expected = (int)submitted;
		rg->r_wait_pid = cur->uk_pid;
		rg->r_wait_result = (int)submitted;
		cur->uk_uring_wait = 1;
		cur->uk_regs = *r;
		kthread_block_tcb(0);
		proc_sched();
	}
	return ((int)submitted);
}

/* 中文：拆除 io_uring 环：解除用户映射与内核映射、释放物理页并标记环空闲 */
/* EN: Tear down an io_uring ring: unmap user and kernel views, free the physical page and mark the ring unused */
static int
uring_teardown_sys(void)
{
	struct uring_ring *rg = 0;
	int i;

	for (i = 0; i < NURING; i++)
		if (rings[i].r_used && rings[i].r_proc == cur) {
			rg = &rings[i];
			break;
		}
	if (rg == 0)
		return (-EINVAL);
	user_unmap(cur->uk_pd, rg->r_uva);
	vfs_page_unmap(URING_KVA + (unsigned)i * PAGE_SIZE);
	page_free(rg->r_pa);
	rg->r_used = 0;
	hal_address_space_activate(cur->uk_pd);
	return (0);
}

/* 中文：回收最多 n 个当前进程的私有匿名页：把候选页压缩进 zcomp 池并以 PG_ZCOMP 项替换 PTE，释放原物理页 */
/* EN: Reclaim up to n private anonymous pages of the current process: compress candidates into the zcomp pool, replace their PTEs with PG_ZCOMP entries and free the pages */
static int
zcomp_reclaim(int n)
{
	unsigned *pd;
	int i, j, done = 0;

	if (in_zcomp || cur == 0)
		return (0);
	in_zcomp = 1;
	/*
	 * S431: the candidate scan and the PTE rewrite are one critical
	 * section with the pmap lock, and page_put() inside it can recurse
	 * back into page_alloc() - which is why pmap_lock is recursive on the
	 * owning CPU.  in_zcomp still keeps that recursion from looping.
	 * 候选扫描与 PTE 改写合成一段 pmap 临界区；里面的 page_put() 可能递归回到
	 * page_alloc()，这正是 pmap_lock 对持有 CPU 可递归的原因。in_zcomp 仍然
	 * 阻止这种递归无限套下去。
	 */
	pmap_lock_enter();
	pd = map_page(SCRATCH_PD, cur->uk_pd);
	for (i = 0; i < USER_PDE_LIMIT && n > 0; i++) {
		unsigned *pt;
		unsigned pte;

		if ((pd[i] & PG_V) == 0)
			continue;
		pt = map_page(SCRATCH_PT, pd[i] & PG_FRAME);
		for (j = 0; j < (int)(PAGE_SIZE / 4) && n > 0; j++) {
			unsigned va = ((unsigned)i << 22) |
			    ((unsigned)j << 12);
			unsigned pa, slot, len;
			unsigned char *src;

			pte = pt[j];
			if ((pte & PG_V) == 0 || (pte & PG_RW) == 0 ||
			    (pte & PG_u) == 0)
				continue;
			if (pte & PG_COW)
				continue;
			if (vma_find(cur, va) != 0)
				continue;
			pa = pte & PG_FRAME;
			if (page_ref_get(pa) > 1)
				continue;
			src = map_page(SCRATCH_PAGE, pa);
			if (zcomp_store(src, &slot, &len) < 0)
				continue;
			pt[j] = (slot << 12) | PG_ZCOMP | PG_u;
			page_put(pa);
			done++;
			n--;
		}
		hal_address_space_activate(cur->uk_pd);
	}
	in_zcomp = 0;
	pmap_lock_exit();
	return (done);
}

/* 中文：munmap 核心：找到起始地址处的 VMA，逐页解除映射；共享页释放页缓存引用，私有页释放物理页，并收缩或清空 VMA */
/* EN: munmap core: find the VMA at addr, unmap pages; shared pages drop page-cache references, private pages are freed, then shrink or clear the VMA */
static int
do_munmap(unsigned addr, unsigned len)
{
	struct kvma *v = vma_find(cur, addr);
	unsigned i;

	if (v == 0 || addr != v->start)
		return (-EINVAL);
	if (len == 0 || len > v->len)
		len = v->len;
	len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	for (i = 0; i < len; i += PAGE_SIZE) {
		unsigned va = addr + i;

		if (v->vp != 0 && (v->flags & MAP_SHARED)) {
			unsigned pa = user_getpa(cur->uk_pd, va);

			user_unmap(cur->uk_pd, va);
			if (pa)
				pagecache_unref_key(v->vid, i / PAGE_SIZE);
		} else {
			/* private/anon page is owned by the process */
			unsigned pa = user_getpa(cur->uk_pd, va);

			user_unmap(cur->uk_pd, va);
			if (pa)
				page_free(pa);
		}
	}
	if (len >= v->len) {
		if (v->vp)
			vn_put(v->vp);
		bzero(v, sizeof(*v));
	} else {
		v->start += len;
		v->len -= len;
		v->off += len;
	}
	return (0);
}

/*
 * msync(addr, len): write a MAP_SHARED file mapping back through the vnode.
 * The ISO9660 root is read-only, so only tmpfs mappings can succeed.
 */
/* 中文：msync 核心：把 MAP_SHARED 文件映射的脏页经 vnode write 写回，仅 tmpfs 可写成功，ISO9660 只读则跳过 */
/* EN: msync core: write back dirty MAP_SHARED file-mapping pages via vnode write; only tmpfs succeeds, read-only ISO9660 is skipped */
static int
do_msync(unsigned addr, unsigned len)
{
	struct kvma *v = vma_find(cur, addr);
	unsigned i;

	if (v == 0)
		return (-EINVAL);
	if (v->vp == 0 || !(v->flags & MAP_SHARED))
		return (0);
	if (len == 0 || len > v->len)
		len = v->len;
	len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	for (i = 0; i < len; i += PAGE_SIZE) {
		unsigned pindex = v->off / PAGE_SIZE + i / PAGE_SIZE;
		unsigned char *src;
		unsigned mode = 0, size = 0, n;
		unsigned pa = pagecache_get(v->vp, pindex, &src);

		if (pa == 0)
			continue;
		v->vp->ops->getattr(v->vp, &mode, &size);
		n = 0;
		if (size > pindex * PAGE_SIZE) {
			n = size - pindex * PAGE_SIZE;
			if (n > PAGE_SIZE)
				n = PAGE_SIZE;
		}
		if (n > 0 && v->vp->ops->write != 0)
			(void)v->vp->ops->write(v->vp, pindex * PAGE_SIZE,
			    src, n);
		pagecache_unref(v->vp, pindex);
	}
	return (0);
}

/*
 * Drop all mappings before the address space goes away.  Shared pages belong
 * to the page cache: unmap the PTE (free_address_space() must not free them)
 * and drop the page reference.  Private pages are owned by the process and
 * are left for free_address_space().
 */
/* 中文：地址空间销毁前清理所有映射：共享页解除映射并释放页缓存引用，私有页留给 free_address_space 处理 */
/* EN: Drop all mappings before the address space goes away: unmap shared pages and drop cache references, leaving private pages to free_address_space */
static void
mmap_teardown(struct process *p)
{
	int i;
	unsigned k;

	for (i = 0; i < NMAP; i++) {
		struct kvma *v = &p->uk_p_vma[i];

		if (!v->used)
			continue;
		if (v->vp != 0 && (v->flags & MAP_SHARED)) {
			for (k = 0; k < v->len; k += PAGE_SIZE) {
				if (user_getpa(p->uk_pd, v->start + k) == 0)
					continue;
				user_unmap(p->uk_pd, v->start + k);
				pagecache_unref_key(v->vid,
				    v->off / PAGE_SIZE + k / PAGE_SIZE);
			}
		}
		if (v->vp)
			vn_put(v->vp);
		bzero(v, sizeof(*v));
	}
	p->uk_p_mmap_base = mmap_base;
}

/* 中文：把 ELF 映像的 PT_LOAD 段按程序头逐页映射到指定页目录，设置保护权限并回写入口地址 */
/* EN: Map the ELF image's PT_LOAD segments page by page into a page directory with proper protections, writing back the entry address */
static void
load_elf(unsigned pdpa, const unsigned char *img, unsigned len, unsigned *entry)
{
	const unsigned *eh = (const unsigned *)img;
	unsigned phoff = eh[7];
	unsigned phnum = *(const unsigned short *)(img + 0x2c);
	unsigned i;

	*entry = eh[6];
	for (i = 0; i < phnum; i++) {
		const unsigned *ph = (const unsigned *)(img + phoff + i * 32);
		unsigned type = ph[0], off = ph[1], vaddr = ph[2];
		unsigned filesz = ph[4], memsz = ph[5];
		unsigned prot = PROT_READ |
		    ((ph[6] & 2) ? PROT_WRITE : 0) |
		    ((ph[6] & 1) ? PROT_EXEC : 0);
		unsigned poff = vaddr & (PAGE_SIZE - 1);
		unsigned base = vaddr - poff;
		unsigned fbase = off - poff;	/* file offset of vaddr's page */
		unsigned n;

		if (type != 1)
			continue;
		/*
		 * A PT_LOAD segment is page-congruent but not necessarily
		 * page-aligned: ld -m elf_i386 gives .data a p_vaddr like
		 * 0x080550e0 (the PE/objcopy path happens to align it, which
		 * is why this only showed up on the Linux build).  Map whole
		 * pages from the page containing p_vaddr and let the file
		 * bytes land at their in-page offset.
		 */
		for (n = 0; n < memsz + poff; n += PAGE_SIZE) {
			unsigned pa = page_alloc();
			char *dst;
			unsigned chunk = memsz + poff - n;
			unsigned pos = fbase + n;	/* file offset of this page */
			unsigned fend = off + filesz;	/* segment's file end */

			if (chunk > PAGE_SIZE)
				chunk = PAGE_SIZE;
			zero_page(pa);
			dst = map_page(SCRATCH_PAGE, pa);
			if (pos < fend) {
				unsigned c = fend - pos;

				if (c > chunk)
					c = chunk;
				bcopy(img + pos, dst, c);
			}
			user_map_prot(pdpa, base + n, pa, prot);
		}
	}
}

/*
 * Lay out argc/argv/envp on a fresh page mapped at USER_ARG_PAGE.
 * Returns the user ESP to start with.
 */
/* 中文：在新用户栈上布局 argc/argv/envp 与指针数组，映射参数页并返回初始 ESP */
/* EN: Lay out argc/argv/envp and pointer arrays on a fresh user stack, map the argument page and return the initial ESP */
static unsigned
build_args(unsigned pdpa, char **argv, int argc, char **envp, int envc)
{
	unsigned pa = page_alloc();
	char *pg;
	unsigned esp = USER_STACK_TOP;
	unsigned aptr[MAXARG], eptr[MAXENV];
	int i, n;

	zero_page(pa);
	pg = map_page(SCRATCH_PAGE, pa);

	for (i = 0; i < envc; i++) {
		n = strlen(envp[i]) + 1;
		esp -= n;
		bcopy(envp[i], pg + (esp - USER_ARG_PAGE), n);
		eptr[i] = esp;
	}
	for (i = 0; i < argc; i++) {
		n = strlen(argv[i]) + 1;
		esp -= n;
		bcopy(argv[i], pg + (esp - USER_ARG_PAGE), n);
		aptr[i] = esp;
	}
	esp &= ~3;

	esp -= 4;
	*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = 0;		/* envp NULL */
	for (i = envc - 1; i >= 0; i--) {
		esp -= 4;
		*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = eptr[i];
	}
	esp -= 4;
	*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = 0;		/* argv NULL */
	for (i = argc - 1; i >= 0; i--) {
		esp -= 4;
		*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = aptr[i];
	}
	esp -= 4;
	*(unsigned *)(pg + (esp - USER_ARG_PAGE)) = argc;

	user_map(pdpa, USER_ARG_PAGE, pa);
	return (esp);
}

/* 中文：从 ELF 映像构建可执行用户映像：创建页目录、加载 ELF 段、压入 argv/envp，返回页目录与入口/栈指针 */
/* EN: Build an executable user image from an ELF blob: create a page directory, load ELF segments, push argv/envp, returning the PD and entry/stack pointers */
static unsigned
build_image(const unsigned char *img, unsigned len, unsigned *entry,
    char **argv, int argc, char **envp, int envc, unsigned *uesp)
{
	unsigned pd = user_pd_create();

	load_elf(pd, img, len, entry);
	/*
	 * The stack is demand-paged too: only the argument page is mapped
	 * here; the first push below USER_STACK_TOP faults in a zero page.
	 * 栈也按需分页：这里只映射参数页；首次向 USER_STACK_TOP 以下压栈时
	 * 由缺页处理分配零页。
	 */
	*uesp = build_args(pd, argv, argc, envp, envc);
	return (pd);
}

/* ------------------------------------------------------------------ ramfs */

/* 中文：安全读取用户字符串到内核缓冲 dst：校验地址已映射并逐字节拷贝，遇 NUL 返回长度，越界或无映射返回 -1 */
/* EN: Safely read a user string into kernel buffer dst: verify pages are mapped and copy bytes, return length at NUL or -1 if invalid */
static int
user_str(unsigned p, char *dst, int max)
{
	int i;

	if (p < USER_LOAD || p >= 0xC0000000 || max <= 1)
		return (-1);
	for (i = 0; i < max - 1; i++) {
		if (user_getpa(cur->uk_pd, (p + i) & ~(PAGE_SIZE - 1)) == 0)
			return (-1);
		dst[i] = ((const char *)p)[i];
		if (dst[i] == 0)
			return (i);
	}
	dst[max - 1] = 0;
	return (-1);
}

/* 中文：从用户地址安全读取一个机器字到 vp，成功返回 0，失败返回 -1 */
/* EN: Safely read one machine word from a user address into vp, returning 0 on success or -1 on failure */
static int
user_word(unsigned p, unsigned *vp)
{

	if (ucopyin((void *)p, vp, sizeof(*vp)) < 0)
		return (-1);
	return (0);
}

/* 中文：在只读 ramfs 表中按名字查找文件，找到返回 struct file 指针，否则 0 */
/* EN: Look up a file by name in the read-only ramfs table, returning its struct file pointer or 0 */
static struct file *
fs_lookup(const char *name)
{
	struct file *f;

	for (f = ramfs; f->f_name; f++)
		if (!strcmp(f->f_name, name) && f->f_data != 0)
			return (f);
	return (0);
}

/* 中文：判断 ISO9660 上是否存在给定路径的文件：已挂载且 iso_lookup 成功返回 1 */
/* EN: Check whether a path exists on the ISO9660 image: returns 1 if mounted and iso_lookup succeeds */
static int
isofile_exists(const char *path)
{
	unsigned lba, size;

	return (iso_mounted() && iso_lookup(path, &lba, &size) == 0);
}

/* 中文：为 ramfs 文件 f 在当前进程分配一个 FD_FILE 描述符并初始化其数据/长度/偏移，返回 fd 号或 -1 */
/* EN: Allocate an FD_FILE descriptor for ramfs file f in the current process and init its data/len/offset, returning the fd or -1 */
static int
fd_alloc(struct file *f)
{
	int i;

	for (i = 3; i < NFD; i++) {
		if (!cur->uk_fds[i].used) {
			cur->uk_fds[i].used = 1;
			cur->uk_fds[i].type = FD_FILE;
			cur->uk_fds[i].d = f->f_data;
			cur->uk_fds[i].len = *f->f_len;
			cur->uk_fds[i].off = 0;
			cur->uk_fds[i].lba = 0;
			cur->uk_fds[i].dsize = 0;
			cur->uk_fds[i].dpos = 0;
			cur->uk_fds[i].pipeidx = -1;
			cur->uk_fds[i].rw = 0;
			cur->uk_fds[i].node = -1;
			return (i);
		}
	}
	return (-1);
}

/* 中文：为 vnode vp 分配一个文件描述符（FD_FILE 或 FD_DIR），设置 rw 与偏移并增加引用，返回 fd 号或 -1 */
/* EN: Allocate a file descriptor for vnode vp (FD_FILE or FD_DIR), set rw and offset and take a reference, returning the fd or -1 */
static int
fd_alloc_vnode(struct vnode *vp, int rw, unsigned off)
{
	int i;

	for (i = 3; i < NFD; i++) {
		if (!cur->uk_fds[i].used) {
			bzero(&cur->uk_fds[i], sizeof(cur->uk_fds[i]));
			cur->uk_fds[i].used = 1;
			cur->uk_fds[i].type = (vp->type == VDIR) ? FD_DIR :
			    FD_FILE;
			cur->uk_fds[i].vp = vp;
			cur->uk_fds[i].rw = rw;
			cur->uk_fds[i].off = off;
			cur->uk_fds[i].pipeidx = -1;
			cur->uk_fds[i].node = -1;
			return (i);
		}
	}
	return (-1);
}

/* 中文：为 ISO9660 目录分配 FD_DIR 描述符，记录其起始 lba 与大小，返回 fd 号或 -1 */
/* EN: Allocate an FD_DIR descriptor for an ISO9660 directory, recording its start lba and size, returning the fd or -1 */
static int
fd_alloc_dir(unsigned lba, unsigned size)
{
	int i;

	for (i = 3; i < NFD; i++) {
		if (!cur->uk_fds[i].used) {
			cur->uk_fds[i].used = 1;
			cur->uk_fds[i].type = FD_DIR;
			cur->uk_fds[i].d = 0;
			cur->uk_fds[i].len = size;
			cur->uk_fds[i].off = 0;
			cur->uk_fds[i].lba = lba;
			cur->uk_fds[i].dsize = size;
			cur->uk_fds[i].dpos = 0;
			cur->uk_fds[i].pipeidx = -1;
			cur->uk_fds[i].rw = 0;
			cur->uk_fds[i].node = -1;
			return (i);
		}
	}
	return (-1);
}



/* ---------------------------------------------------------------- tmpfs */

#define	NTMPFS		32
#define	TMPFS_NAMELEN	32
#define	TMPFS_DATASZ	4096
#define	TMPFS_VA	0xFE600000	/* kernel window, one page per node */

static int	tmpfs_mapped[NTMPFS];

struct tmpfs_node {
	int		used;
	int		isdir;
	char		name[TMPFS_NAMELEN];
	unsigned char	*data;		/* one page per file */
	unsigned	size;
};

static struct tmpfs_node tmpfs[NTMPFS];

/* 中文：按名字查找 tmpfs 节点，找到返回节点指针，否则 0 */
/* EN: Find a tmpfs node by name, returning its pointer or 0 */
static struct tmpfs_node *
tmpfs_find(const char *name)
{
	int i;

	for (i = 0; i < NTMPFS; i++)
		if (tmpfs[i].used && strcmp(tmpfs[i].name, name) == 0)
			return (&tmpfs[i]);
	return (0);
}

/* 中文：按名字创建 tmpfs 文件/目录节点：已存在同名同类则返回它，否则分配槽位，文件则映射一页数据并清零 */
/* EN: Create a tmpfs file/directory node by name: return an existing matching node, else allocate a slot and, for files, map and zero a data page */
static struct tmpfs_node *
tmpfs_create(const char *name, int isdir)
{
	struct tmpfs_node *n;
	int i, len;

	len = strlen(name);
	if (len == 0 || len >= TMPFS_NAMELEN)
		return (0);
	if ((n = tmpfs_find(name)) != 0)
		return (n->isdir == isdir ? n : 0);
	for (i = 0; i < NTMPFS; i++) {
		if (!tmpfs[i].used) {
			n = &tmpfs[i];
			bzero(n, sizeof(*n));
			n->used = 1;
			n->isdir = isdir;
			bcopy(name, n->name, len + 1);
			if (!isdir) {
				unsigned off = i * PAGE_SIZE;

				n->data = (unsigned char *)(TMPFS_VA + off);
				if (!tmpfs_mapped[i]) {
					unsigned pa = page_alloc();

					pmap_map((vm_offset_t)n->data,
					    (vm_offset_t)pa,
					    (vm_offset_t)pa + PAGE_SIZE,
					    VM_PROT_READ | VM_PROT_WRITE);
					tmpfs_mapped[i] = 1;
				}
				bzero(n->data, TMPFS_DATASZ);
			}
			return (n);
		}
	}
	return (0);
}

/* 中文：删除 tmpfs 节点：清 used 标志、大小与数据指针 */
/* EN: Remove a tmpfs node: clear its used flag, size and data pointer */
static void
tmpfs_remove(struct tmpfs_node *n)
{

	n->used = 0;
	n->size = 0;
	n->data = 0;
}

/*
 * "/tmp" is the tmpfs root; "/tmp/name" is a node.  Anything deeper is not
 * supported (one flat level is enough for redirection and scratch files).
 */
/* 中文：解析 /tmp 或 /tmp/name 路径：根路径返回 isdir=1、np=0，单层节点返回对应节点及类型，非法路径返回 -1 */
/* EN: Parse a /tmp or /tmp/name path: the root yields isdir=1 and np=0, a single-level node returns its pointer and type, invalid paths return -1 */
static int
tmpfs_lookup(const char *path, struct tmpfs_node **np, int *isdir)
{
	const char *name;

	if (strcmp(path, "/tmp") == 0 || strcmp(path, "/tmp/") == 0) {
		*np = 0;
		*isdir = 1;
		return (0);
	}
	if (strncmp(path, "/tmp/", 5) != 0)
		return (-1);
	name = path + 5;
	if (strchr(name, '/') != 0)
		return (-1);
	if ((*np = tmpfs_find(name)) == 0)
		return (-1);
	*isdir = (*np)->isdir;
	return (0);
}

/* 中文：判断路径是否位于 tmpfs（/tmp、/tmp/ 或 /tmp/...），是则返回 1 */
/* EN: Check whether a path is under tmpfs (/tmp, /tmp/ or /tmp/...), returning 1 if so */
static int
tmpfs_is_tmp(const char *path)
{

	return (strcmp(path, "/tmp") == 0 || strcmp(path, "/tmp/") == 0 ||
	    strncmp(path, "/tmp/", 5) == 0);
}

/* 中文：为 tmpfs 节点 n 分配 FD_TMPFILE 描述符，设置读写方式、偏移和长度，返回 fd 号或 -1 */
/* EN: Allocate an FD_TMPFILE descriptor for tmpfs node n, set access mode, offset and length, returning the fd or -1 */
static int
fd_alloc_tmpfile(struct tmpfs_node *n, int rw, unsigned off)
{
	int i;

	for (i = 3; i < NFD; i++) {
		if (!cur->uk_fds[i].used) {
			bzero(&cur->uk_fds[i], sizeof(cur->uk_fds[i]));
			cur->uk_fds[i].used = 1;
			cur->uk_fds[i].type = FD_TMPFILE;
			cur->uk_fds[i].node = n - tmpfs;
			cur->uk_fds[i].rw = rw;
			cur->uk_fds[i].off = off;
			cur->uk_fds[i].len = n->size;
			cur->uk_fds[i].pipeidx = -1;
			return (i);
		}
	}
	return (-1);
}

/* 中文：分配一个 FD_TMPDIR 描述符用于读取 tmpfs 根目录，返回 fd 号或 -1 */
/* EN: Allocate an FD_TMPDIR descriptor for reading the tmpfs directory, returning the fd or -1 */
static int
fd_alloc_tmpdir(void)
{
	int i;

	for (i = 3; i < NFD; i++) {
		if (!cur->uk_fds[i].used) {
			bzero(&cur->uk_fds[i], sizeof(cur->uk_fds[i]));
			cur->uk_fds[i].used = 1;
			cur->uk_fds[i].type = FD_TMPDIR;
			cur->uk_fds[i].dpos = 0;
			cur->uk_fds[i].node = -1;
			cur->uk_fds[i].pipeidx = -1;
			return (i);
		}
	}
	return (-1);
}

/* ---------------------------------------------------------------- pipes */

static struct process *proc_find(int pid);
static void proc_sched(void);

#define	NPIPE	8
#define	PIPESZ	4096

struct pipe {
	int		used;
	int		readers;
	int		writers;
	unsigned	rpos, wpos, count;
	int		rpid;		/* pid blocked in read */
	int		wpid;		/* pid blocked in write */
	unsigned char	buf[PIPESZ];
};

static struct pipe pipes[NPIPE];

/*
 * P2: one lock for the eight-slot pipe table.  It covers slot allocation,
 * reader/writer counts, the ring positions and the blocked pid fields.
 * Rule: never call proc_sched()/block while holding it; mark the blocker,
 * drop the lock, then schedule.  Lock order: pipe_lock -> sc_rqlock (via
 * proc_wake/sched_runnable).  No path takes sc_rqlock and then pipe_lock.
 */
static struct spinlock pipe_lock;

/*
 * P2/§7.2: file descriptor table lock.
 *
 * The fd table lives in struct process (uk_fds[NFD]) and is shared by every
 * thread of that process, so pthreads can open/close/read the same table on
 * different CPUs once the BKL stops covering the whole syscall.  One global
 * lock is enough for this port (16 fds per process, NPROC processes).
 *
 * Rules:
 *   - it is a leaf lock: only fd-array scans, validations and offset
 *     updates happen inside it, never a backend call;
 *   - taking a vnode reference inside it is allowed and is the documented
 *     order fd_lock -> vfs_lock; nothing takes fd_lock while holding
 *     vfs_lock, pipe_lock or the BKL-internal locks;
 *   - the read(2) path below is the first user of it, together with the
 *     first BKL-dropped syscall path.
 */
/* 中文：P2 fd 表锁：fd 表在 struct process 内、由同进程所有线程共享；BKL 一旦不再覆盖整条系统调用，pthread 就会在不同核上并发读写它。本移植用一把全局锁。规则：只做 fd 数组扫描/校验/偏移更新，绝不跨后端调用；锁内可递增 vnode 引用，锁序 fd_lock -> vfs_lock，反向不存在。 */
static struct spinlock fd_lock;

/* 中文：分配一个空闲管道槽位并清零，返回管道指针，无空闲则返回 0 */
/* EN: Allocate and clear a free pipe slot, returning its pointer or 0 if none */
static struct pipe *
pipe_alloc(void)
{
	struct pipe *pp = 0;
	int i;

	spin_lock(&pipe_lock);
	for (i = 0; i < NPIPE; i++) {
		if (!pipes[i].used) {
			bzero(&pipes[i], sizeof(pipes[i]));
			pipes[i].used = 1;
			pp = &pipes[i];
			break;
		}
	}
	spin_unlock(&pipe_lock);
	return (pp);
}

/* EN: Add one reader/writer reference to a pipe under pipe_lock. */
/* 中文：在 pipe_lock 保护下给管道增加一个读端/写端引用。 */
static void
pipe_addref(struct pipe *pp, int rw)
{

	if (pp == 0)
		return;
	spin_lock(&pipe_lock);
	if (rw == 1)
		pp->readers++;
	else if (rw == 2)
		pp->writers++;
	spin_unlock(&pipe_lock);
}

/* 中文：在指定进程页目录下在内核缓冲与用户地址 va 之间拷贝 n 字节，临时切换 CR3 后恢复 */
/* EN: Copy n bytes between a kernel buffer and user address va under a given page directory, temporarily switching CR3 then restoring */
static void
pipe_copy(unsigned pd, unsigned va, const void *kbuf, unsigned n, int touser)
{
	unsigned save;

	save = hal_address_space_current();
	hal_address_space_activate(pd);
	if (touser)
		bcopy(kbuf, (void *)va, n);
	else
		bcopy((const void *)va, (void *)kbuf, n);
	hal_address_space_activate(save);
}

/* ==================================================================== */
/* S400: IPC 会合（rendezvous）——用户态 recv 真正阻塞、send 直接投递      */
/* S400: IPC rendezvous - user recv blocks, send delivers directly        */
/* ==================================================================== */

/*
 * 中文：找到阻塞在 port 上的用户接收者并把消息写进它的用户缓冲，填好返回值后
 * 唤醒。返回 1 表示已投递。这类接收者在 ipc_recv  syscall 里保存了
 * uk_ipc_waiting/port/msguser 与完整 uk_regs，恢复后就像 syscall 正常返回。
 * EN: Find a user receiver blocked on port, write the message into its user
 * buffer, set the return value and wake it.  Returns 1 if delivered.  Such a
 * receiver saved uk_ipc_waiting/port/msguser plus full uk_regs in the recv
 * syscall; on resume it looks like the syscall returned normally.
 */
static int
ipc_deliver_waiter(unsigned port, const struct ipc_msg *m)
{
	struct process *p;
	int i;

	for (i = 0; i < NPROC; i++) {
		p = &procs[i];
		if (p->uk_state != P_BLOCKED || !p->uk_ipc_waiting ||
		    p->uk_ipc_port != port)
			continue;
		/* 中文：在等待者页目录上下文把消息拷进它的用户缓冲 */
		/* EN: copy the message into the waiter's user buffer under its PD */
		pipe_copy(p->uk_pd, p->uk_ipc_msguser, m, sizeof(*m), 1);
		p->uk_ipc_waiting = 0;
		p->uk_regs.eax = 0;
		proc_wake(p);
		return (1);
	}
	return (0);
}

/*
 * 中文：端口释放/服务退出时，给所有阻塞在该端口的用户接收者返回 -EIO，
 * 实现"服务崩溃 → 在飞请求返回 EIO"的故障隔离契约。
 * EN: On port release / service exit, fail all user receivers blocked on the
 * port with -EIO, implementing the "service crash -> in-flight EIO" contract.
 */
void
ipc_fail_waiters(unsigned port)
{
	struct process *p;
	int i;

	/* KVFS: 服务退出时，给在途内核转发事务返回 -EIO，实现故障隔离。
	 * KVFS: on service exit, fail the in-flight kernel forward with -EIO. */
	if (port == SRV_PORT_KERNEL && kvfs_waiter != 0 &&
	    kvfs_waiter->uk_state == P_BLOCKED && kvfs_waiter->uk_kvfs_waiting) {
		struct process *kw = kvfs_waiter;

		kw->uk_regs.eax = (unsigned)-EIO;
		kw->uk_kvfs_waiting = 0;
		kvfs_waiter = 0;
		proc_wake(kw);
	}
	for (i = 0; i < NPROC; i++) {
		p = &procs[i];
		if (p->uk_state != P_BLOCKED || !p->uk_ipc_waiting ||
		    p->uk_ipc_port != port)
			continue;
		p->uk_ipc_waiting = 0;
		p->uk_regs.eax = (unsigned)-EIO;
		proc_wake(p);
	}
}

/* ==================================================================== */
/* KVFS: kernel-side /srv/ transparent forwarding (serialized transaction) */
/* KVFS: 内核态 /srv/ 透明转发（串行化事务）                              */
/* ==================================================================== */
/*
 * 中文：内核是协作式调度——一个 syscall 在转发处 proc_sched() 阻塞后，整个 CPU
 * 才切到 fs_server；fs_server 回复时 SYS_IPC_SEND 在 fs_server 上下文执行回调。
 * 因此同一时刻最多只有一个内核转发事务在途，用一个全局等待者指针即可定位回复
 * 该回填给谁。回调在 fs_server 的上下文运行，需切到等待者页目录回填用户缓冲。
 * EN: The kernel is cooperatively scheduled - once a forwarding syscall blocks
 * in proc_sched(), the CPU switches to fs_server; its reply triggers the
 * callback inside SYS_IPC_SEND running in fs_server's context.  At most one
 * kernel forwarding transaction is in flight, so one global waiter pointer
 * locates the reply target.  The callback copies into the waiter's buffer
 * under the waiter's page directory.
 */

/*
 * 中文：在等待者进程里分配一个 /srv/ 虚拟 fd（存服务 handle+1），返回虚拟
 * fd 或负 errno。虚拟 fd 从 KVFS_FD_BASE 起，不与内核 fd 冲突。
 * EN: Allocate a /srv/ virtual fd in the waiter (stores handle+1), returning
 * the virtual fd or a negative errno.  Virtual fds start at KVFS_FD_BASE.
 */
static int
kvfs_fd_alloc(struct process *p, unsigned handle)
{
	int i;

	for (i = 0; i < KVFS_FD_MAX; i++)
		if (p->uk_kvfs_fds[i] == 0) {
			p->uk_kvfs_fds[i] = handle + 1;
			return (KVFS_FD_BASE + i);
		}
	return (-EMFILE);
}

/*
 * 中文：虚拟 fd -> handle（槽位索引），非 KVFS fd 返回 -1。
 * EN: virtual fd -> handle (slot index); -1 if not a KVFS fd.
 */
static int
kvfs_fd_handle(struct process *p, int fd)
{
	int slot = fd - KVFS_FD_BASE;

	if (slot < 0 || slot >= KVFS_FD_MAX || p->uk_kvfs_fds[slot] == 0)
		return (-1);
	return ((int)(p->uk_kvfs_fds[slot] - 1));
}

/*
 * 中文：回复到达时回填内核转发事务。OPEN 成功时登记虚拟 fd 并把虚拟 fd 作为
 * 返回值；READ 回填用户缓冲并返回字节数；其它操作直接返回 handle 里的结果。
 * 写完 eax 后唤醒等待进程。返回 1 表示已消费此回复。
 * EN: Complete a kernel forwarding transaction on reply.  OPEN registers a
 * virtual fd and returns it; READ copies into the user buffer and returns the
 * byte count; other ops return the handle field.  Wake the waiter after
 * writing eax.  Returns 1 if the reply was consumed.
 */
static int
kvfs_reply_deliver(unsigned port, const struct ipc_msg *m)
{
	struct process *p = kvfs_waiter;
	struct srv_args_kernel *wa;
	int result, data_len;

	if (port != SRV_REPLY_PORT_KERNEL || p == 0 ||
	    p->uk_state != P_BLOCKED || !p->uk_kvfs_waiting)
		return (0);

	wa = (struct srv_args_kernel *)m->inline_data;
	result = (int)wa->handle;	/* handle 字段承载返回值/错误码 */

	if (result >= 0) {
		if (p->uk_kvfs_op == SRV_OPEN_KERNEL) {
			/* OPEN：把服务 handle 登记成虚拟 fd，作为 open 返回值 */
			result = kvfs_fd_alloc(p, (unsigned)result);
		} else if (p->uk_kvfs_op == SRV_READ_KERNEL) {
			/* READ：inline_data[8] 起的数据回填到等待者用户缓冲 */
			data_len = (int)m->len - (int)sizeof(struct srv_args_kernel);
			if (data_len < 0)
				data_len = 0;
			if ((unsigned)data_len > p->uk_kvfs_len)
				data_len = (int)p->uk_kvfs_len;
			if (data_len > 0)
				pipe_copy(p->uk_pd, p->uk_kvfs_buf,
				    m->inline_data + sizeof(struct srv_args_kernel),
				    (unsigned)data_len, 1);
			result = data_len;	/* read 返回实际字节数 */
		} else if (p->uk_kvfs_op == SRV_CLOSE_KERNEL) {
			/* CLOSE：服务已确认关闭，释放等待者的虚拟 fd 槽位。
			 * close 的 fd 参数保存在 uk_regs.ebx（kvfs_forward 保存了完整
			 * syscall 上下文），据此定位 KVFS_FD_BASE 起的槽位。返回 0。
			 * EN: service acknowledged close; release the waiter's virtual
			 * fd slot (fd is in saved uk_regs.ebx) and return 0. */
			int slot = (int)p->uk_regs.ebx - KVFS_FD_BASE;

			if (slot >= 0 && slot < KVFS_FD_MAX)
				p->uk_kvfs_fds[slot] = 0;
			result = 0;
		}
	}

	p->uk_regs.eax = (unsigned)result;
	p->uk_kvfs_waiting = 0;
	kvfs_waiter = 0;
	proc_wake(p);
	return (1);
}

/*
 * 中文：内核发起一次 /srv/ 转发：打包 srv 请求发到请求端口，登记在途事务与
 * 续传状态，保存完整 syscall 上下文后阻塞；回复由 kvfs_reply_deliver 回填。
 * 返回 TRUE 表示已阻塞并交出 CPU（恢复后从 uk_regs 继续用户态）；FALSE 表示
 * 请求入队失败（如服务端口无属主），调用方需自行在 r->eax 写错误码。
 * EN: Issue one /srv/ forward: pack the srv request to the request port,
 * register the in-flight transaction and resume state, save the full syscall
 * context and block; the reply is completed by kvfs_reply_deliver.  Returns
 * TRUE if blocked (execution resumes in user mode via uk_regs); FALSE if the
 * send failed, in which case the caller writes the error into r->eax.
 */
static int
kvfs_forward(struct sysregs *r, unsigned op, unsigned handle,
    unsigned buf, unsigned arg, unsigned len,
    const void *payload, unsigned payload_len)
{
	struct ipc_msg m;
	struct srv_args_kernel *a = (struct srv_args_kernel *)m.inline_data;
	struct process *p = cur;
	int n;

	if (kvfs_waiter != 0)
		return (0);		/* 已有在途事务（不应发生）: caller -> EBUSY */
	/* 服务未启动/已退出：直接返回，调用方写 -EIO（故障隔离）。
	 * Service absent/gone: return so the caller yields -EIO. */
	if (port_owner(SRV_PORT_KERNEL) == 0)
		return (0);

	memset(&m, 0, sizeof(m));
	m.type = op;
	a->handle = handle;
	a->arg = arg;
	m.len = sizeof(struct srv_args_kernel);
	if (payload != 0 && payload_len > 0) {
		if (payload_len > SRV_DATA_MAX_KERNEL)
			payload_len = SRV_DATA_MAX_KERNEL;
		bcopy(payload, m.inline_data + sizeof(struct srv_args_kernel),
		    payload_len);
		m.len += payload_len;
	}
	/* 会合优先：若 fs_server 正阻塞在 ipc_recv(端口0)，直接投递并唤醒，
	 * 避免"入队但对方已从 proc_sched 恢复、不再读队列"竞态。
	 * Rendezvous first: if fs_server is blocked in ipc_recv(port 0),
	 * deliver directly to avoid the queue-vs-resume race. */
	if (ipc_deliver_waiter(SRV_PORT_KERNEL, &m))
		goto registered;
	n = ipc_send(SRV_PORT_KERNEL, &m);
	if (n < 0)
		return (0);		/* 服务端口无属主等；调用方写 -EIO */

registered:

	/* 登记在途事务：完整 syscall 上下文存 uk_regs，回复回填参数存 uk_* */
	p->uk_regs = *r;
	p->uk_kvfs_waiting = 1;
	p->uk_kvfs_op = op;
	p->uk_kvfs_buf = buf;
	p->uk_kvfs_len = len;
	p->uk_kvfs_handle = handle;
	kvfs_waiter = p;
	kthread_block_tcb(0);
	proc_sched();
	/* 唤醒后从 uk_regs 恢复用户态，不会执行到这里 */
	return (1);
}

/*
 * Hand pending data to a reader blocked inside read().  The copy happens in
 * the writer's context, so the reader's page directory is installed for the
 * duration; the kernel is mapped in both.
 */
/* 中文：把管道中待读数据交给阻塞的读者：在读者页目录上下文把数据拷入其缓冲，处理环形回绕，置结果并唤醒；eof 时返回 0 */
/* EN: Hand pending pipe data to a blocked reader: copy into its buffer under its page directory handling ring wrap, set result and wake; EOF yields 0 */
static void
pipe_wake_reader_locked(struct pipe *pp, int eof)
{
	struct process *p;
	unsigned n, first;

	if (pp->rpid == 0)
		return;
	p = proc_find(pp->rpid);
	pp->rpid = 0;
	if (p == 0 || p->uk_state != P_BLOCKED || p->uk_pipeidx != pp - pipes)
		return;
	if (eof) {
		p->uk_regs.eax = 0;
		proc_wake(p);
		return;
	}
	n = pp->count;
	if (n > p->uk_pipelen)
		n = p->uk_pipelen;
	first = PIPESZ - pp->rpos;
	if (first > n)
		first = n;
	pipe_copy(p->uk_pd, p->uk_pipeat, pp->buf + pp->rpos, first, 1);
	if (n > first)
		pipe_copy(p->uk_pd, p->uk_pipeat + first, pp->buf, n - first, 1);
	pp->rpos = (pp->rpos + n) % PIPESZ;
	pp->count -= n;
	p->uk_regs.eax = n;
	proc_wake(p);
}

/* 中文：把管道数据在写者上下文拷入阻塞读者缓冲，处理环形回绕并置结果唤醒；epipe 时返回 -EPIPE */
/* EN: Copy pipe data into a blocked writer's buffer in the writer's context handling ring wrap, set result and wake; epipe yields -EPIPE */
static void
pipe_wake_writer_locked(struct pipe *pp, int epipe)
{
	struct process *p;
	unsigned space, n, first;

	if (pp->wpid == 0)
		return;
	p = proc_find(pp->wpid);
	pp->wpid = 0;
	if (p == 0 || p->uk_state != P_BLOCKED || p->uk_pipeidx != pp - pipes)
		return;
	if (epipe) {
		p->uk_regs.eax = (unsigned)-EPIPE;
		proc_wake(p);
		return;
	}
	space = PIPESZ - pp->count;
	n = p->uk_pipelen;
	if (n > space)
		n = space;
	first = PIPESZ - pp->wpos;
	if (first > n)
		first = n;
	pipe_copy(p->uk_pd, p->uk_pipeat, pp->buf + pp->wpos, first, 0);
	if (n > first)
		pipe_copy(p->uk_pd, p->uk_pipeat + first, pp->buf, n - first, 0);
	pp->wpos = (pp->wpos + n) % PIPESZ;
	pp->count += n;
	p->uk_regs.eax = n;
	proc_wake(p);
}

/* 中文：关闭文件描述符：管道端减少读/写者计数并按需唤醒对端，释放 vnode 引用，最后标记 fd 空闲 */
/* EN: Close a file descriptor: decrement pipe reader/writer counts and wake peers as needed, drop the vnode reference and mark the fd free */
static void
fd_close(int fd)
{
	struct kfd *fp;
	struct pipe *pp;

	if (fd < 0 || fd >= NFD || !cur->uk_fds[fd].used)
		return;
	fp = &cur->uk_fds[fd];
	if (fp->type == FD_PIPE && fp->pipeidx >= 0 &&
	    fp->pipeidx < NPIPE) {
		pp = &pipes[fp->pipeidx];
		spin_lock(&pipe_lock);
		if (fp->rw == 1)
			pp->readers--;
		else if (fp->rw == 2)
			pp->writers--;
		if (pp->readers <= 0)
			pipe_wake_writer_locked(pp, 1);
		if (pp->writers <= 0)
			pipe_wake_reader_locked(pp, 1);
		if (pp->readers <= 0 && pp->writers <= 0 &&
		    pp->wpid == 0 && pp->rpid == 0) {
			pp->used = 0;
			fp->pipeidx = -1;
		}
		spin_unlock(&pipe_lock);
	}
	if (fp->vp != 0) {
		vn_put(fp->vp);
		fp->vp = 0;
	}
	fp->used = 0;
}

/* 中文：为 fd 结构持有的 vnode 增加一次引用计数 */
/* EN: Increment the reference count of the vnode held by an fd structure */
static void
fd_ref(struct kfd *fp)
{

	if (fp->vp != 0)
		vn_ref(fp->vp);
}

/* --------------------------------------------------------------- process */

static int user_ok(unsigned p, unsigned len);
extern void resume_user(unsigned pd, struct sysregs *r);
extern int vfs_absolutize(const char *, const char *, char *, unsigned);

/* S420: second per-CPU scratch group (see SCRATCH_GRP above). */
#define	SCRATCH2_PD	(0xFE203000u + SCRATCH_GRP * 0x1000u)
#define	SCRATCH2_PT	(0xFE204000u + SCRATCH_GRP * 0x1000u)
#define	SCRATCH2_PAGE	(0xFE205000u + SCRATCH_GRP * 0x1000u)

/*
 * Deep copy of a user address space: new page directory, new page tables
 * and new pages with the same contents.  The kernel's high mappings are
 * shared, as usual.
 */
/* 中文：深拷贝用户地址空间：新建页目录与页表并逐页复制内容，内核高端映射共享 */
/* EN: Deep-copy a user address space: create new page directory and tables and copy each page, sharing kernel high mappings */
static unsigned
copy_address_space(unsigned srcpd)
{
	unsigned dstpd;
	unsigned *spd, *dpd;
	int i, j;

	pmap_lock_enter();
	dstpd = user_pd_create();
	spd = map_page(SCRATCH_PD, srcpd);
	dpd = map_page(SCRATCH2_PD, dstpd);
	for (i = 0; i < USER_PDE_LIMIT; i++) {
		unsigned *spt, *dpt;
		unsigned dptpa;

		if ((spd[i] & PG_V) == 0)
			continue;
		dptpa = page_alloc();
		spt = map_page(SCRATCH_PT, spd[i] & PG_FRAME);
		dpt = map_page(SCRATCH2_PT, dptpa);
		for (j = 0; j < PAGE_SIZE / 4; j++)
			dpt[j] = 0;
		for (j = 0; j < PAGE_SIZE / 4; j++) {
			unsigned spa, dpa;
			char *s, *d;

			if ((spt[j] & PG_V) == 0)
				continue;
			spa = spt[j] & PG_FRAME;
			dpa = page_alloc();
			s = map_page(SCRATCH_PAGE, spa);
			d = map_page(SCRATCH2_PAGE, dpa);
			bcopy(s, d, PAGE_SIZE);
			dpt[j] = dpa | (spt[j] & 0xfff);
		}
		dpd[i] = dptpa | (spd[i] & 0xfff);
	}
	pmap_lock_exit();
	return (dstpd);
}

/*
 * os.md mechanism 2/3: fork() installs copy-on-write mappings instead of
 * copying every page.  Both parent and child point at the same physical
 * page (PG_COW set, PG_RW clear); the first write in either one takes a
 * page fault and gets a private copy.  Pages that were already read-only
 * (text) are shared read-only as before.
 */
/* 中文：写时复制方式克隆用户地址空间：父子共享物理页并置 PG_COW、清 PG_RW，增加引用计数，首次写入再私有复制 */
/* EN: Clone a user address space copy-on-write: parent and child share pages with PG_COW set and PG_RW cleared, bumping refcounts until first write */
static unsigned
copy_address_space_cow(unsigned srcpd)
{
	unsigned dstpd;
	unsigned *spd, *dpd;
	int i, j;

	/*
	 * S431: this loop *writes the parent's PTEs* (clears PG_RW, sets
	 * PG_COW) while the parent may be running on another CPU, and the
	 * refcount bump it pairs with must be atomic.  Both happen under the
	 * pmap lock, so a concurrent fault on the parent either sees the page
	 * still writable (and does nothing) or sees PG_COW (and copies).
	 * 这段循环**修改父进程的 PTE**（清 PG_RW、置 PG_COW），而父进程可能正在
	 * 另一个 CPU 上跑；与之配对的引用计数自增也必须是原子的。两者都在 pmap
	 * 锁内完成，因此父进程的并发缺页要么看到页仍可写（什么都不做），要么看到
	 * PG_COW（去复制）。
	 */
	pmap_lock_enter();
	dstpd = user_pd_create();
	spd = map_page(SCRATCH_PD, srcpd);
	dpd = map_page(SCRATCH2_PD, dstpd);
	for (i = 0; i < USER_PDE_LIMIT; i++) {
		unsigned *spt, *dpt;
		unsigned dptpa;

		if ((spd[i] & PG_V) == 0)
			continue;
		dptpa = page_alloc();
		spt = map_page(SCRATCH_PT, spd[i] & PG_FRAME);
		dpt = map_page(SCRATCH2_PT, dptpa);
		for (j = 0; j < PAGE_SIZE / 4; j++)
			dpt[j] = 0;
		for (j = 0; j < PAGE_SIZE / 4; j++) {
			unsigned pa;

			if ((spt[j] & PG_V) == 0)
				continue;
			pa = spt[j] & PG_FRAME;
			if (spt[j] & PG_RW) {
				spt[j] &= ~PG_RW;
				spt[j] |= PG_COW;
			}
			dpt[j] = spt[j];
			page_ref_inc(pa);
			cow_shared++;
		}
		dpd[i] = dptpa | (spd[i] & 0xfff);
	}
	pmap_lock_exit();
	return (dstpd);
}

/*
 * Demand-zero heap page: a not-present access below the current brk gets a
 * fresh zero page mapped read/write.  Anything outside [heap_start, brk)
 * is still an invalid access and becomes SIGSEGV.
 * 按需零页：访问当前 brk 以下但尚未映射的地址时，分配并映射一个零页；
 * 堆范围之外的访问仍是非法访问，最终投递 SIGSEGV。
 */
/* 中文：按需零页缺页处理：堆或栈范围内的未映射访问分配一个零页映射并激活地址空间，范围外返回 0 */
/* EN: Handle demand-zero faults: map a fresh zeroed page for unmapped heap/stack accesses and activate the address space, returning 0 outside those ranges */
static int
demand_page(unsigned va)
{
	unsigned page = va & ~(PAGE_SIZE - 1);
	unsigned pa;

	if (cur == 0)
		return (0);
	if (page >= USER_HEAP_START && page < cur->uk_brk_mapped)
		;			/* heap */
	else if (page >= USER_STACK_TOP - USER_STACK_SIZE &&
	    page < USER_STACK_TOP)
		;			/* main 2 MB stack (S411) */
	else if (page >= UTHREAD_STACK_BASE &&
	    page < UTHREAD_STACK_BASE + UTHREAD_STACKS * UTHREAD_STACK_SIZE)
		;			/* per-thread 2 MB stack (S411) */
	else
		return (0);
	pa = page_alloc();
	zero_page(pa);
	if (!user_map_if_absent(cur->uk_pd, page, pa,
	    PROT_READ | PROT_WRITE)) {
		page_free(pa);		/* S431: lost a race, drop our copy */
	}
	hal_address_space_activate(cur->uk_pd);
	return (1);
}

/*
 * Write fault on a COW page: if we are the last user just make it
 * writable, otherwise allocate and copy.  Returns 1 when handled.
 */
/* 中文：用户态缺页入口：非 present 页交给压缩重填/按需零页/mmap 缺页处理，COW 写故障则独占或复制页，返回是否处理成功 */
/* EN: User page-fault entry: route not-present faults to zcomp/demand-zero/mmap handlers and handle COW write faults by taking ownership or copying, returning whether handled */
static int user_page_fault_locked(unsigned, unsigned);

int
user_page_fault(unsigned va, unsigned err)
{
	int r;

	/*
	 * S410: mark the fault path so page_alloc() does not try to reclaim
	 * (compress) pages out from under the handler.  See in_fault.
	 * S410：标记缺页路径，避免 page_alloc() 在处理过程中压缩页面（见 in_fault）。
	 */
	/*
	 * S413: a page fault edits page tables, the zcomp cache and the COW
	 * tables - all still single-writer structures.  Take the BKL like a
	 * syscall; nesting is counted, so a fault inside a syscall (ucopyin)
	 * is free.
	 * S413：缺页要改页表、zcomp 缓存和 COW 表，这些都还是单写者结构，
	 * 所以和系统调用一样进 BKL；嵌套会计数，系统调用内部缺页不额外加锁。
	 */
	smp_bkl_enter();
	in_fault = 1;
	r = user_page_fault_locked(va, err);
	in_fault = 0;
	smp_bkl_exit();
	return (r);
}

static int
user_page_fault_locked(unsigned va, unsigned err)
{
	unsigned *pt;
	unsigned pti, pa, npa;
	char *s, *d;

	if (cur == 0)
		return (0);
	if ((err & 4) == 0)		/* not a user access */
		return (0);
	if ((err & 1) == 0)		/* not present: compressed/heap/stack/mmap */
		return (zcomp_refault(va) || demand_page(va) ||
		    mmap_fault(va & ~(PAGE_SIZE - 1)));
	if ((err & 7) != 7)		/* present + write + user */
		return (0);
	/*
	 * S431: read the COW PTE, take ownership and install the result as
	 * one pmap critical section.  Two threads of the same process faulting
	 * on one shared page used to be serialized by the BKL; with the pmap
	 * lock the loser now sees a PTE that is already writable and private
	 * and simply returns, instead of copying the page a second time and
	 * overwriting the winner's mapping.
	 * 把"读 COW PTE、取得所有权、写回结果"合成一段 pmap 临界区。同一进程的两个
	 * 线程对同一个共享页缺页原本靠 BKL 串行；现在后到者会看到 PTE 已经私有且
	 * 可写，直接返回，不会再复制一次并覆盖先到者的映射。
	 */
	pmap_lock_enter();
	pti = user_pte_ptr(cur->uk_pd, va & ~(PAGE_SIZE - 1), &pt);
	if (pt == 0 || (pt[pti] & PG_V) == 0 || (pt[pti] & PG_COW) == 0) {
		pmap_lock_exit();
		return (0);
	}
	pa = pt[pti] & PG_FRAME;
	cow_faults++;
	if (page_ref_get(pa) <= 1) {
		/* sole owner: keep the page, drop the COW marker */
		pt[pti] = (pt[pti] & ~PG_COW) | PG_RW;
	} else {
		npa = page_alloc();
		s = map_page(SCRATCH_PAGE, pa);
		d = map_page(SCRATCH2_PAGE, npa);
		bcopy(s, d, PAGE_SIZE);
		page_put(pa);
		pt[pti] = npa | PG_V | PG_RW | PG_u;
		cow_copies++;
	}
	pmap_lock_exit();
	hal_address_space_activate(cur->uk_pd);
	return (1);
}

/* 中文：打印 COW 统计信息：共享页数、缺页次数与私有复制次数 */
/* EN: Print copy-on-write statistics: shared pages, fault count and private copy count */
void
cow_print(void)
{

	printf("cow: shared %d faults %d copies %d\n",
	    cow_shared, cow_faults, cow_copies);
}

/* 中文：为进程 p 生成用户态内核执行载体线程（主线程），初始化线程与线程链表指针 */
/* EN: Spawn the main user-mode kernel thread for process p and initialize its thread list pointers */
static struct kthread *
proc_add_thread(struct process *p, int tid)
{
	struct kthread *t;

	t = kthread_user_spawn(p, tid);
	if (t == 0)
		return (0);
	/* kthread_user_spawn already linked t into p->uk_p_threads and
	 * bumped uk_p_nthreads; just record it as the running thread. */
	p->uk_p_thread = t;
	return (t);
}

/* 中文：分配一个空闲进程槽并初始化：设置 cwd、mmap 基址及 0/1/2 三个控制台 fd，返回进程指针或 0 */
/* EN: Allocate and init a free process slot: set cwd, mmap base and console fds 0/1/2, returning its pointer or 0 */
/*
 * S411: derive the user heap and mmap windows from the boot memory plan.
 *
 * heap   = [USER_HEAP_START, USER_HEAP_START + addressable/2)
 * mmap   = [heap_end, heap_end + addressable/8)
 * stacks = [UTHREAD_STACK_BASE, +32 MB) and the main 2 MB stack at the top
 *
 * All three are address-space caps; pages are demand-paged.  The clamp
 * below UTHREAD_STACK_BASE keeps a 24 GB policy from producing a 12 GB
 * heap that this 32-bit port could never map.
 * S411：由内存规划推导用户堆与 mmap 窗口，全部是地址空间上限，页按需分配。
 */
static void
user_layout_init(void)
{
	unsigned base_kb, heap_bytes, mmap_bytes, max_heap;

	base_kb = mem_plan.addressable_kb;
	heap_bytes = (base_kb / 2u) * 1024u;
	mmap_bytes = (base_kb / 8u) * 1024u;
	max_heap = UTHREAD_STACK_BASE - USER_HEAP_START - 0x100000u;
	if (heap_bytes > max_heap)
		heap_bytes = max_heap;
	user_heap_max = USER_HEAP_START + heap_bytes;
	mmap_base = (user_heap_max + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (mmap_base >= UTHREAD_STACK_BASE)
		mmap_bytes = 0;
	else if (mmap_bytes > UTHREAD_STACK_BASE - mmap_base)
		mmap_bytes = UTHREAD_STACK_BASE - mmap_base;
	mmap_limit = mmap_base + mmap_bytes;
	printf("usermode: user VA heap %x-%x (%d MB), mmap %x-%x (%d MB)\n",
	    USER_HEAP_START, user_heap_max, (int)(heap_bytes >> 20),
	    mmap_base, mmap_limit, (int)(mmap_bytes >> 20));
}

/* 中文：进程表初始化：清零静态进程表（保留此入口以便引导顺序不变） */
/* EN: Process table init: zero the static table (kept as an entry point so the boot order is unchanged). */
void
proc_table_init(void)
{

	user_layout_init();
	spin_init(&proc_table_lock);
	spin_init(&page_lock);
	spin_init(&pipe_lock);
	spin_init(&fd_lock);
	procs = (struct process *)kern_table_alloc((unsigned)PROC_TABLE_VA,
	    (unsigned)NPROC * (unsigned)sizeof(struct process));
	printf("usermode: %d proc slots at %x (%d KB)\n", NPROC,
	    PROC_TABLE_VA, (NPROC * sizeof(struct process)) / 1024);
}

static struct process *
proc_alloc(void)
{
	int i;

	/*
	 * S413: the process table is shared by every CPU, and two forks can
	 * run at the same time (fork itself serialises on the BKL, but a
	 * future fine-grained split would not).  The lock makes the scan +
	 * bzero + claim one step.  It is held only for the initialisation,
	 * never across a page allocation inside the caller.
	 */
	spin_lock(&proc_table_lock);
	for (i = 0; i < NPROC; i++)
		if (procs[i].uk_state == P_FREE) {
			int fd;

			bzero(&procs[i], sizeof(procs[i]));
			procs[i].uk_p_cwd[0] = '/';
			procs[i].uk_p_cwd[1] = 0;
			procs[i].uk_p_mmap_base = mmap_base;
			for (fd = 0; fd < 3; fd++) {
				procs[i].uk_fds[fd].used = 1;
				procs[i].uk_fds[fd].type = FD_CONSOLE;
				procs[i].uk_fds[fd].rw = (fd == 0) ? 1 : 2;
				procs[i].uk_fds[fd].pipeidx = -1;
				procs[i].uk_fds[fd].node = -1;
			}
			/* still P_FREE until the caller sets P_RUN: a second
			 * allocator must not see this slot as free, so mark
			 * it with a private state here. */
			procs[i].uk_state = P_ALLOC;
			spin_unlock(&proc_table_lock);
			return (&procs[i]);
		}
	spin_unlock(&proc_table_lock);
	return (0);
}

/*
 * S15: remember a short process name for /dev/ps ("comm").  The kernel does
 * not keep argv around, so the executable's basename is the best we have.
 */
/* 中文：记录进程的可执行文件基名到 p_comm（用于 /dev/ps），内核不保留 argv */
/* EN: Record the executable basename of a path into p_comm for /dev/ps; the kernel does not keep argv */
static void
proc_setcomm(struct process *p, const char *path)
{
	const char *base, *s;
	int i;

	base = path;
	for (s = path; *s; s++)
		if (*s == '/')
			base = s + 1;
	bzero(p->uk_p_comm, sizeof(p->uk_p_comm));
	for (i = 0; i < (int)sizeof(p->uk_p_comm) - 1 && base[i] != 0; i++)
		p->uk_p_comm[i] = base[i];
}

/* 中文：按 pid 查找进程：找到返回进程指针，否则 0 */
/* EN: Find a process by pid, returning its pointer or 0 */
static struct process *
proc_find(int pid)
{
	int i;

	for (i = 0; i < NPROC; i++)
		if (procs[i].uk_state != P_FREE && procs[i].uk_pid == pid)
			return (&procs[i]);
	return (0);
}

/*
 * 中文：S300 KP-7 桥接——返回进程 pid 的页目录物理地址（找不到返回 0）。
 * EN: S300 KP-7 bridge: return the page-directory physical address of process
 * pid (0 if not found).
 */
unsigned
proc_pd_by_pid(unsigned pid)
{
	struct process *p = proc_find((int)pid);

	return (p != 0 ? p->uk_pd : 0);
}

/* 中文：S300——当前进程的 pid（无用户进程上下文时返回 0） */
/* EN: S300: pid of the current process (0 if no user process context). */
unsigned
proc_cur_pid(void)
{

	return (cur != 0 ? (unsigned)cur->uk_pid : 0);
}

/* 中文：统计指定 pid 的子进程数量 */
/* EN: Count the number of child processes of a given pid */
static int
proc_children(int pid)
{
	int i, n = 0;

	for (i = 0; i < NPROC; i++)
		if (procs[i].uk_state != P_FREE && procs[i].uk_ppid == pid)
			n++;
	return (n);
}

/*
 * S413: per-CPU scheduler bring-up for an AP.
 *
 * The AP owns an "idle" carrier TCB that lives outside the TCB pool, so the
 * scan below can never hand it to another CPU.  Its kernel stack is the
 * stack the AP booted on, which is also what its TSS esp0 points at until
 * the first real thread is dispatched.
 * S413：为 AP 建立每核调度状态。AP 拥有一个池外的 idle 载体 TCB，下面的扫描
 * 永远不会把它交给别的核；它的内核栈就是 AP 启动时用的那段，在被第一次真正
 * 调度前，TSS 的 esp0 也指向这里。
 */
void
sched_percpu_init(int id, unsigned stack_top)
{
	struct kthread *t;

	if (id <= 0 || id >= MAXCPU)
		return;
	t = &per_cpu_idle[id];
	bzero(t, sizeof(*t));
	t->t_runq_cpu = -1;		/* S428: carriers are never queued */
	t->t_state = KT_RUN;
	t->t_tid = -1;
	t->t_name = "idle";
	t->t_used = 1;			/* valid carrier, but not pooled */
	t->t_proc = 0;
	t->t_stack_top = stack_top & ~15u;
	t->t_oncpu = -1;
	t->t_base_ipl = 0;
	t->t_preemptable = 1;
	sched_cpu[id].sc_idle = t;
	sched_cpu[id].sc_cur = 0;
	sched_cpu[id].sc_curthread = t;
	sched_cpu[id].sc_cur_pd = 0;
	sched_cpu[id].sc_need_resched = 0;
	sched_cpu[id].sc_switches = 0;
	/* S428: this CPU's run queue starts empty */
	sched_cpu[id].sc_rqhead = 0;
	sched_cpu[id].sc_rqtail = 0;
	sched_cpu[id].sc_nrun = 0;
	spin_init(&sched_cpu[id].sc_rqlock);
	sched_cpu[id].sc_online = 1;
	hal_kernel_stack_set(t->t_stack_top);
}

/* EN: AP entry into the shared scheduler loop (never returns). */
void
ap_sched(void)
{

	sched_cpu[cpu_id()].sc_online = 1;
	proc_sched();
}

/*
 * Pick the next runnable process and return to user mode.
 * Never returns.
 */
/* 中文：进程调度器：循环运行内核线程与可运行用户线程（先投递信号再 resume_user），无任务时停机；永不返回 */
/* EN: Process scheduler: loop running kernel threads and runnable user threads (deliver signals then resume_user), halting when idle; never returns */
static void
proc_sched(void)
{
	struct process *me = cur;
	struct kthread *mt;
	int cid = cpu_id();

	/*
	 * S417: keep the "thread that called the scheduler" in a per-CPU
	 * global, NOT in a stack local.  The TRAP 12 at the end of this loop
	 * (mt suddenly reading as a page directory, 0x6c6000, while `me` in
	 * the adjacent slot stayed valid) was a stack-local `mt` being
	 * overwritten somewhere in the deep signal/exit call chain: resume_user()
	 * leaves this frame via iret and a later syscall re-enters the kernel on
	 * a different stack, so the value has to survive outside the frame.
	 * Any probe added inside this function changed the frame layout and
	 * hid the bug - a sure sign of a stack-slot clobber.  A per-CPU slot
	 * is immune to that and is the natural home for scheduler state.
	 */
	if (cid < 0 || cid >= MAXCPU) {
		printf("smp: impossible cpu id %d\n", cid);
		return;
	}
	mt = cur_thread;
	ps_mt[cid] = mt;
	/*
	 * S414: defence in depth.  sc_curthread must be 0, an AP idle
	 * carrier (per_cpu_idle[]) or a TCB from the pool; a value like a
	 * page-directory address means this CPU's scheduler state was
	 * overwritten (the S413 TRAP 12 was exactly that: mt became
	 * 0x6c6000, a page-directory pointer).  Drop it and keep scheduling
	 * instead of faulting on a wild pointer.
	 */
	if (mt != 0 &&
	    (mt < kthreads || mt >= kthreads + nkthread) &&
	    (mt < per_cpu_idle || mt >= per_cpu_idle + MAXCPU)) {
		printf("smp: cpu%d bad cur_thread %p (cur %p), resetting\n",
		    cid, mt, cur);
		mt = 0;
		ps_mt[cid] = 0;
		cur_thread = 0;
	}

	for (;;) {
		unsigned pd_to_load;

		/*
		 * S417: re-read the caller TCB from the per-CPU slot at the
		 * top of every iteration.  `mt` is a stack local and can be
		 * clobbered by the deep call chain below (that was the TRAP 12);
		 * the per-CPU copy is not on this stack.
		 */
		mt = ps_mt[cid];

		/*
		 * S413: the caller's kernel epoch ends here, on every path -
		 * including the "nothing to run, go idle" one.  Dropping the
		 * BKL only just before resume_user() was not enough: a CPU
		 * that parks in hlt with the lock still held blocks every
		 * other CPU in smp_bkl_enter(), which is exactly how the first
		 * SMP run deadlocked (BSP waiting in wait4, AP spinning on the
		 * BKL with the `ls` child it had just dispatched).
		 * S413：调用者的内核临界区在这里结束，所有路径都一样——包括“没活干
		 * 就去 idle”。只在 resume_user 之前放锁不够：持锁停在 hlt 的核会
		 * 把其它核全卡在 smp_bkl_enter 里，这正是第一版 SMP 的死锁
		 * （BSP 在 wait4 里等，AP 拿着刚派发的 ls 子进程在 BKL 上自旋）。
		 */
		smp_bkl_drop();
		/*
		 * S424: drop the ownership of the TCB this CPU left last time
		 * round, so it can migrate to an idle CPU.
		 */
		sched_release_prev();
		/*
		 * S413: publish TCB slots whose owner threads have already left
		 * their stacks (see kthread_reap).  `mt` is the stack this CPU
		 * is on, so its own slot is never recycled under its feet.
		 */
		kthread_reap(mt);
		/*
		 * S21: kernel threads (softirq, page reclaim, IRP workers)
		 * get their own execution carrier first.
		 *
		 * S429: only the BSP does this *before* user dispatch.  An AP
		 * reaches this point with the previous thread's CR3 still
		 * loaded, and a kernel thread that touches a user buffer would
		 * then run in the wrong address space.  APs poll from the idle
		 * path below instead, where CR3 is IdlePTD - the same context
		 * the BSP has always used for them.
		 */
		if (cid == 0)
			kthread_run();
		/*
		 * S25: the scheduler walks threads, not processes.  Each
		 * user thread names its address-space container through
		 * t_proc; run state still lives on the process for now.
		 */
		/*
		 * S424: ONE selection policy for the whole scheduler.  This
		 * loop used to dispatch the first claimable KT_RUN slot in
		 * table order - i.e. "lowest TCB index wins" - while
		 * sched_tick() used fair_pick_next() (effective priority,
		 * then estcpu, then SMT affinity).  So a CPU-bound thread in
		 * a low slot took the CPU at every voluntary switch point
		 * (block/exit/syscall) and the priority decision only ever
		 * applied on timer ticks.  fair_pick_next() already filters
		 * with tcb_runnable(), which carries the same process-state
		 * rules this loop used to duplicate.
		 *
		 * S413: with AP user dispatch switched off, an AP keeps its
		 * LAPIC/IPI path alive but never runs user threads; `smpuser`
		 * toggles this at runtime.
		 */
		if (cid == 0 || smp_user_dispatch) {
			struct kthread *t = fair_pick_next(mt);
			struct process *p;
			struct sysregs *regs;
			int main_thread;

			/*
			 * S413: the claim is the SMP-critical part.  It fails
			 * when another CPU already owns the TCB (which is how
			 * N CPUs share one table without running a thread
			 * twice or sharing its kernel stack) - and it can fail
			 * for a thread fair_pick_next() just chose, because
			 * another CPU may have claimed it in the meantime.
			 */
			if (t != 0 && kthread_claim_cpu(t, cid)) {
				p = t->t_proc;
				main_thread = (t->t_tid == p->uk_pid);
				regs = main_thread ? &p->uk_regs : &t->t_regs;
				cur = p;
				cur_thread = t;
				cpus[cid].ci_curthread = t;
				t->t_want_cpu = cid;	/* S424: affinity hint */
				/*
				 * S424: a dispatched thread starts a fresh
				 * quantum.  sched_on_tick() only increments
				 * t_ticks and the preemption test is
				 * "t_ticks >= t_quantum", so without this
				 * reset a thread that once exhausted its
				 * quantum stayed "expired" forever and every
				 * tick switched it out - disabling
				 * QUANTUM_IO/QUANTUM_CPU and turning the
				 * scheduler into a fixed 1-tick round robin.
				 */
				t->t_ticks = 0;
				/* KLT: pass 0 to resume_user when staying in
				 * the same address space (it then keeps CR3). */
				pd_to_load = switch_pd_for(p);
				hal_kernel_stack_set(t->t_stack_top);
				sig_deliver(p, regs);
				if (t->t_state == KT_RUN) {
					/* Leaving kernel cooperation: the BKL
					 * must not travel into user mode. */
					if (kstack_overflowed(t))
						printf("smp: STACK OVERFLOW tid %d cpu %d\n",
						    t->t_tid, cid);
					smp_bkl_drop();
					sched_cpu[cid].sc_switches++;
					idle_reported = 0;	/* dispatched */
					/*
					 * S428: this CPU is leaving the caller
					 * (mt).  It is still KT_RUN, so it has
					 * to be on somebody's run queue before we
					 * switch away - otherwise it would only
					 * come back through the repair sweep.
					 */
					if (mt != 0 && mt != t &&
					    mt->t_state == KT_RUN)
						sched_rq_put(mt, cid);
					/* S424: release mt on the next entry */
					sched_cpu[cid].sc_prev = mt;
					sched_cpu[cid].sc_parked = 0;
					resume_user(pd_to_load, regs);
				}
				kthread_release_cpu(t);	/* sig_deliver killed it */
			} else if (t != 0) {
				/* S428: claim lost (or sig killed it): requeue */
				sched_rq_put(t, cid);
			}
		}
		if (me != 0 && mt != 0 && mt->t_state == KT_RUN &&
		    me->uk_state != P_ZOMBIE && me->uk_state != P_EXITING) {
			cur = me;
			cur_thread = mt;
			cpus[cid].ci_curthread = mt;
			pd_to_load = switch_pd_for(me);
			hal_kernel_stack_set(mt->t_stack_top);
			sig_deliver(me, &me->uk_regs);
			smp_bkl_drop();
			sched_cpu[cid].sc_switches++;
			sched_cpu[cid].sc_prev = mt;	/* S424 */
			sched_cpu[cid].sc_parked = 0;
			resume_user(pd_to_load, &me->uk_regs);
		}
		/* nothing runnable: let deferred work drain, then halt */
		kthread_run();
		if (kthread_runnable())
			continue;
		/*
		 * S413: with SMP on, an idle CPU must stay in the loop: a
		 * woken thread can be claimed here at any time, and the BSP
		 * itself may have work again.  Interrupts are enabled before
		 * hlt so the next tick/IPI brings us back.
		 * S413：开 SMP 后空闲核要留在循环里——随时可能有被唤醒的线程归它领，
		 * BSP 自己也可能又有活。hlt 前开中断，下一个 tick/IPI 会把它叫回来。
		 */
		if (smp_enabled) {
			/*
			 * S425: park on the kernel directory.  An idle CPU has
			 * no user thread, and leaving a process PD in CR3 both
			 * keeps that address space un-freeable (proc_exit()'s
			 * quiesce waits for exactly this) and leaves its page
			 * tables hot for nothing.  Everyone coming back through
			 * switch_pd_for() compares against the live CR3, so the
			 * reload happens automatically.
			 */
			if (hal_address_space_current() != (unsigned)IdlePTD) {
				hal_address_space_activate((unsigned)IdlePTD);
				cur_pd = (unsigned)IdlePTD;
			}
			/*
			 * S424: mark the CPU as parked on the kernel directory.
			 * proc_exit()'s address-space quiesce uses this: a
			 * parked CPU has already loaded IdlePTD, so it is off
			 * every user address space even though t_oncpu still
			 * names it.
			 *
			 * Do NOT release the TCB ownership here.  The slot's
			 * publication is keyed off t_oncpu (kthread_user_free
			 * defers only while an owner exists), and this CPU is
			 * still executing the idle loop on that very 16 KB
			 * stack: publishing it would let another CPU's
			 * kstack_alloc() bzero the stack underneath us, which
			 * showed up as a garbage iret frame inside resume_user
			 * (kernel TRAP 9, esp=0 ss=0, "system halted").
			 */
			sched_cpu[cid].sc_parked = 1;	/* S424 */
			/*
			 * S413: one summary per busy->idle transition so the
			 * multi-core work is visible: the per-CPU switch
			 * counts only differ if user threads really ran on
			 * more than one CPU.
			 */
			if (!idle_reported) {
				idle_reported = 1;
				if (cid == 0)
					smp_print();
			}
			__asm __volatile("sti; hlt");
			continue;
		}
		printf("\n[no runnable process]\n");
		/*
		 * Idle report: physical pages still held when the last process
		 * exits.  Fork/exit churn is only visible here - a per-spawn leak
		 * (kstack_alloc used to map a fresh 8 KB stack every time and
		 * never release it) shows up as page_ninuse growing with the
		 * number of forks while next_pa stays put.
		 * 停机报告：最后一个进程退出时仍占用的物理页。fork/exit 抖动只在
		 * 这里可见——每次 spawn 都新分配 8KB 内核栈且从不归还时，
		 * page_ninuse 会随 fork 次数增长而 next_pa 不动。
		 */
		mem_report();
		for (;;)
			hal_cpu_halt();
	}
}

/*
 * os.md mechanism 4, rule 1: the clock interrupt preempts a user thread.
 *
 * The interrupt frame on the shared kernel stack describes the interrupted
 * user context.  We store it in that thread's slot, pick the next runnable
 * thread in round-robin order, load its address space, and rewrite the
 * frame; irq0_entry's iret then returns straight into the new thread.
 * Interrupts are off (interrupt gate) for the whole switch, and we never
 * switch while the interrupted context is in kernel mode: kernel-side
 * deferred work is handled at syscall exit via softirq_dispatch().
 */
unsigned sched_switches;

/*
 * S413: runtime switch for AP user-thread dispatch (monitor `smpuser`).
 * Default OFF: smpon brings the APs up (per-CPU TSS, scheduler state, IPI),
 * and `smpuser` additionally lets them dispatch user threads.  It is off by
 * default because some workloads still trip over the coarse kernel locking
 * (see os_smp.md section 7); with it off the system behaves exactly like the
 * single-CPU build, only with the APs parked in hlt.
 * S413：AP 运行用户线程的运行时开关（monitor `smpuser`）。默认开：
 * smpon + 这个开关才让用户线程真正跑在多个核上。关掉后 AP 仍在线（IPI 通路
 * 正常），但只有 BSP 做调度——遇到内核锁粒度不足导致的负载时，这是安全退路。
 */
int	smp_user_dispatch = 0;

/*
 * S413: need_resched is per-CPU now (see sched_cpu above).  The clock
 * interrupt still has to ask for a reschedule, but timer.c is a separate
 * translation unit and cannot see the macro, so it calls these.
 */
void
smp_need_resched(void)
{

	need_resched = 1;
}

int
smp_resched_pending(void)
{

	return (need_resched);
}

/* EN: S413: per-CPU context-switch counter (monitor 'smpinfo'). */
unsigned
sched_cpu_switches(int id)
{

	return ((id >= 0 && id < MAXCPU) ? sched_cpu[id].sc_switches : 0);
}

/*
 * S413: wake one other CPU's scheduler.
 *
 * A freshly published thread (fork/clone) is unowned, so any CPU may take
 * it - but an idle AP sits in `hlt` and would only notice at the next clock
 * tick.  Kicking one AP in round-robin order makes a fork land on a second
 * CPU immediately, which is what turns "N CPUs online" into "N CPUs doing
 * work".  The IPI is advisory: if that CPU is busy it just sets its
 * reschedule flag.
 * S413：唤醒另一个核的调度器。新发布的线程无人占用，任何核都能领；但空闲 AP
 * 停在 hlt 里，要等下一个时钟才醒。按轮转踢一个 AP，fork 就能立刻落到第二个
 * 核上——这才把"在线 N 核"变成"N 核都在干活"。IPI 只是提示，对方忙就只置标志。
 */
static int kick_next = 1;

static void
sched_kick_other(void)
{
	int id;

	if (!smp_enabled || ncpu <= 1)
		return;
	if (kick_next < 1 || kick_next >= ncpu)
		kick_next = 1;
	id = kick_next;
	kick_next = (kick_next + 1 >= ncpu) ? 1 : kick_next + 1;
	smp_resched_cpu(id);
}

/* ------------------------------------------------------------------ */
/* S413: big kernel lock                                              */
/* ------------------------------------------------------------------ */

/*
 * User code runs in parallel on every CPU.  The kernel is not yet split
 * into fine-grained locks (proc table, VFS, pipes, address space and the
 * page allocator all assume they are alone), so kernel entries serialize
 * on one lock.  This is deliberately the Linux 2.0 model: correctness
 * first, user-space parallelism now, kernel parallelism later.
 *
 * Contract:
 *   - smp_bkl_enter() at a kernel entry (syscall, user page fault);
 *   - smp_bkl_exit() when the CPU returns to user mode;
 *   - smp_bkl_drop() right before a context switch, because the CPU may
 *     not return to this code path at all and the other CPUs must not
 *     spin behind a thread that is asleep.
 * The owner is recorded so a nested entry on the same CPU does not
 * self-deadlock, and drop/exit are safe to call when not held.
 */
static struct spinlock	smp_bkl_lock;
static volatile int	smp_bkl_owner = -1;
static int		smp_bkl_depth;
static unsigned		smp_bkl_waits;

void
smp_bkl_enter(void)
{
	int me = cpu_id();

	if (smp_bkl_owner == me) {	/* nested kernel entry on this CPU */
		smp_bkl_depth++;
		return;
	}
	spin_lock(&smp_bkl_lock);
	smp_bkl_owner = me;
	smp_bkl_depth = 1;
}

void
smp_bkl_exit(void)
{
	int me = cpu_id();

	if (smp_bkl_owner != me)
		return;
	if (--smp_bkl_depth > 0)
		return;
	smp_bkl_owner = -1;
	spin_unlock(&smp_bkl_lock);
}

int
smp_bkl_held(void)
{

	return (smp_bkl_owner == cpu_id());
}

/* EN: Release the BKL before this CPU switches to another thread. */
void
smp_bkl_drop(void)
{

	if (!smp_bkl_held())
		return;
	smp_bkl_depth = 1;		/* collapse nesting: we are leaving */
	smp_bkl_exit();
}

/*
 * Process-level fairness (BSD estcpu style).  CPU usage is charged to the
 * PROCESS, not the thread, so one process with 100 runnable threads cannot
 * take 100 shares.  pick_next first chooses the least-charged runnable
 * process, then round-robins a thread within it (affinity: last me's proc
 * wins ties, preserving cache locality).
 */
/*
 * 4.4BSD estcpu model (McKusick).  estcpu is charged +1 each tick a process
 * runs, and decayed ONCE PER SECOND by:
 *     estcpu = (2*loadavg * estcpu) / (2*loadavg + 1) + nice
 * so decay = 2L/(2L+1): a busy system decays slowly (L large -> decay~1),
 * an idle one decays fast (L=1 -> 2/3).  This is what makes interactive
 * (mostly-sleeping) processes keep a low estcpu and wake up at high priority.
 * The old code decayed by >>2 EVERY tick, which drove estcpu to 0 (bug).
 */
#define	ESTCPU_HZ	100		/* PIT Hz: one decay per 100 ticks */
#define	ESTCPU_MAX	255

static unsigned estcpu_tick_count;	/* ticks since last 1-second decay */

static int tcb_runnable(struct kthread *t);	/* forward decl */

/* EN: Approximate loadavg as the number of runnable threads (min 1). */
static int
estcpu_loadavg(void)
{
	int i, n = 0;

	for (i = 0; i < nkthread; i++)
		if (tcb_runnable(&kthreads[i]))
			n++;
	return (n < 1 ? 1 : n);
}

/* EN: One-second BSD decay of every process's estcpu. */
static void
estcpu_decay_all(void)
{
	int i, L2 = 2 * estcpu_loadavg();	/* 2 * loadavg */

	for (i = 0; i < NPROC; i++) {
		struct process *p = &procs[i];

		if (p->uk_state == P_FREE)
			continue;
		/* estcpu = estcpu * 2L/(2L+1) + nice */
		p->uk_estcpu = (p->uk_estcpu * L2) / (L2 + 1) +
		    (unsigned)(p->p_nice > 0 ? p->p_nice : 0);
		if (p->uk_estcpu > ESTCPU_MAX)
			p->uk_estcpu = ESTCPU_MAX;
	}
	/*
	 * S424: the scheduling penalty is charged per thread now, so the
	 * per-thread charge has to decay on the same schedule as the
	 * process one - otherwise t_estcpu would only ever grow and every
	 * long-lived thread would sink to the lowest priority.
	 */
	for (i = 0; i < nkthread; i++) {
		struct kthread *t = &kthreads[i];

		if (!t->t_used || t->t_estcpu <= 0)
			continue;
		t->t_estcpu = (t->t_estcpu * L2) / (L2 + 1);
		if (t->t_estcpu > ESTCPU_MAX)
			t->t_estcpu = ESTCPU_MAX;
	}
}

/* EN: Is a TCB currently runnable (TCB state + whole-process block rule)? */
static int
tcb_runnable(struct kthread *t)
{
	struct process *p;

	if (t == 0 || !t->t_used || t->t_proc == 0 || t->t_state != KT_RUN)
		return (0);
	p = t->t_proc;
	if (p->uk_state == P_FREE || p->uk_state == P_ZOMBIE ||
	    p->uk_state == P_ALLOC || p->uk_state == P_EXITING)
		return (0);
	if (p->uk_state == P_BLOCKED && p->uk_p_nthreads <= 1)
		return (0);
	return (1);
}

/*
 * EN: BSD-style interaction penalty: a high estcpu (CPU-bound) or a high
 * p_nice (voluntary yield) lowers the thread's effective priority;
 * interactive (low estcpu) tasks stay high.
 *
 * S424: the charge is per THREAD (t_estcpu).  It used to read uk_estcpu,
 * which is charged once per tick to the whole process: an N-thread process
 * therefore collected N times the charge of a single-threaded process at
 * the same nice (getting N times the CPU share), while all its threads
 * shared one penalty.  uk_estcpu is still maintained for /dev/ps.
 */
int
sched_interaction_penalty(struct kthread *t)
{
	struct process *p;
	int penalty;

	if (t == 0 || (p = t->t_proc) == 0)
		return (0);
	/* 4.4BSD: usrpri = PUSER + estcpu/4 + 2*nice.  estcpu (0..255)
	 * contributes 0..63; compress into a few priority levels here. */
	penalty = (int)(t->t_estcpu / 32);	/* estcpu/4 scaled to levels */
	penalty += 2 * (int)(p->p_nice > 0 ? p->p_nice : 0) / 4; /* 2*nice */
	if (penalty > 8)
		penalty = 8;
	return (penalty);
}

/*
 * S415: SMT/cache affinity score for dispatching TCB t on logical CPU cid.
 * This is a tie-breaker only - it never changes a thread's dynamic priority
 * or the preemption decision, so the BSD scheduler semantics stay intact.
 *   +3  thread last ran on this exact CPU       (hot L1/L2, no migration)
 *   +1  last ran on this physical core's SMT sibling (cheap intra-core move)
 *    0  last ran elsewhere / never ran          (cold, normal placement)
 * With no SMT (cpu_smt <= 1) the sibling case cannot occur.
 * S415：分发 TCB 时的 SMT/cache 亲和分。仅作同优先级时的裁决，不改变动态
 * 优先级或抢占语义。上次在本核跑过 +3；上次在同物理核的 SMT 兄弟上 +1；
 * 其余 0。无 SMT 时不存在兄弟情况。
 */
static int
smt_affinity_score(struct kthread *t, int cid)
{
	/*
	 * S424: t_want_cpu is where the thread last actually ran, i.e. where
	 * its L1/L2 lines still are.  Ownership (t_oncpu) is a stack mutex
	 * and is released as soon as the CPU switches away, so it can no
	 * longer stand in for affinity - this is a pure placement hint.
	 */
	int last = (t->t_want_cpu >= 0) ? t->t_want_cpu : t->t_lastcpu;

	if (last < 0 || last >= MAXCPU)
		return (0);
	if (last == cid)
		return (3);
	if (cpu_smt > 1 && smt_phys_core((int)cpus[cid].ci_apicid) ==
	    smt_phys_core((int)cpus[last].ci_apicid))
		return (1);
	return (0);
}

/*
 * EN: Pick the best runnable TCB.  Primary key is the DYNAMIC effective
 * priority (HIGHER = better, 0 lowest .. 19 highest).  I/O-woken, starved
 * and priority-inherited threads get boosted; CPU-bound threads stay low.
 * estcpu and SMT affinity break ties so a process with many threads cannot
 * dominate and a thread prefers the CPU (or physical core) it last used.
 *
 * S428: the candidates now come from the per-CPU run queues instead of a walk
 * over the whole TCB table: this CPU's own queue first (O(1) amortised), then
 * one stealing pass over the other queues, and only if both come up empty the
 * repair sweep costs an O(nkthread) walk to make sure nothing was lost.  The
 * normal steady state never touches another CPU's cache lines.
 */
static struct kthread *
fair_pick_next(struct kthread *me)
{
	struct kthread *t;
	int cid = cpu_id();

	(void)me;		/* the caller's TCB is never on a queue */
	if (cid < 0 || cid >= MAXCPU)
		return (0);
	if ((t = sched_rq_pick(cid)) != 0)
		return (t);
	if ((t = sched_rq_steal(cid)) != 0)
		return (t);
	sched_rq_sweep(cid);
	if ((t = sched_rq_pick(cid)) != 0)
		return (t);
	return (0);
}

/* 中文：时钟中断抢占处理：保存被中断用户线程的上下文，按进程公平选中下一线程并改写中断帧，iret 直接进入新线程 */
/* EN: Clock-tick preemption: save context, pick the next thread with process-level fairness and rewrite the frame so iret enters it */
void
sched_tick(struct sysregs *r)
{
	struct kthread *me = cur_thread;
	struct process *mp = cur;
	struct kthread *t;
	struct sysregs *regs;
	struct process *p;
	int main_thread;

	if (cur == 0 || me == 0 || r == 0)
		return;
	/* S424: the TCB this CPU left on an earlier tick may migrate now */
	sched_release_prev();
	/*
	 * Preemption rule: user mode is always preemptable; kernel mode may
	 * be inside a critical section (t_preempt_cnt > 0) or an opted-out
	 * thread, in which case we only set the flag and defer the switch to
	 * the next safe point (return to user mode).
	 */
	if (!hal_intr_from_user(r)) {
		need_resched = 1;	/* deferred: no switch in kernel context */
		return;
	}
	if (me->t_preempt_cnt > 0 || me->t_base_ipl >= KTHREAD_NO_PREEMPT) {
		need_resched = 1;
		return;
	}

	/* save the interrupted context */
	if (me->t_tid == mp->uk_pid)
		mp->uk_regs = *r;
	else
		me->t_regs = *r;

	/* charge this tick: process CPU accounting + thread quantum/counters */
	if (mp->uk_estcpu < ESTCPU_MAX)
		mp->uk_estcpu++;		/* BSD: running process accumulates */
	sched_on_tick(me);			/* quantum + CPU-bound detection */
	/* BSD: decay once per second (100 PIT ticks), not every tick */
	if (++estcpu_tick_count >= ESTCPU_HZ) {
		estcpu_tick_count = 0;
		estcpu_decay_all();
	}

	/*
	 * Anti-starvation aging + sleep accounting.
	 *
	 * Every waiting-but-runnable thread ages upward (so a CPU-bound
	 * thread cannot sit on the same low priority forever), and every
	 * blocked thread accumulates sleep ticks - that is what makes
	 * "slept long enough to be interactive" measurable; before this,
	 * t_sleep_ticks only ever counted wakeups, not time.
	 *
	 * S424: it walks the whole TCB table and writes into live TCBs, so
	 * it now runs on alternate ticks per CPU.  At 1024 slots x 16 CPUs x
	 * 100 Hz the old version issued ~1.6 M written cache lines per
	 * second at other CPUs' live threads - pure false sharing - and the
	 * aging threshold is a wall-clock heuristic anyway.
	 */
	if ((++sched_cpu[cpu_id()].sc_tick & 1) == 0) {
		int j;

		for (j = 0; j < nkthread; j++) {
			struct kthread *o = &kthreads[j];

			if (!o->t_used || o == me)
				continue;
			if (o->t_state == KT_RUN) {
				if (o->t_starve < STARVE_THRESHOLD * 2)
					o->t_starve++;
			} else if (o->t_state == KT_BLOCK) {
				if (o->t_sleep_ticks < 0x40000000)
					o->t_sleep_ticks++;
			}
		}
	}

	/*
	 * Preemptive decision (priority scheduling): switch immediately if a
	 * strictly higher-priority thread became runnable, OR the current
	 * thread exhausted its quantum.  Need_resched from syscall exit is
	 * still honoured.  A high-priority thread need not wait for the
	 * quantum to expire -- that is the essence of preemption.
	 */
	t = fair_pick_next(me);
	{
		int me_prio = sched_effective_prio(me);
		int next_prio = (t != 0) ? sched_effective_prio(t) : PRI_LOWEST;
		int quantum_expired = (me->t_ticks >= me->t_quantum);

		/* higher-priority next preempts immediately (no wait for quantum) */
		if (t == 0 || (!quantum_expired && !need_resched &&
		               next_prio <= me_prio)) {
			/*
			 * S428: fair_pick_next() took `t` off a run queue.
			 * Keeping the current thread means putting it back,
			 * otherwise the candidate stays invisible until the
			 * next repair sweep.
			 */
			sched_rq_put(t, cpu_id());
			return;			/* keep running current thread */
		}
	}
	need_resched = 0;
	if (t == 0) {
		/* nobody else runnable: current thread keeps the CPU */
		if (me->t_tid == mp->uk_pid)
			mp->uk_regs = *r;
		else
			me->t_regs = *r;
		return;
	}
	p = t->t_proc;
	main_thread = (t->t_tid == p->uk_pid);
	regs = main_thread ? &p->uk_regs : &t->t_regs;
	/*
	 * S413: claim before resuming.  If another CPU grabbed the thread in
	 * the window between the scan and here, keep running the current one
	 * instead of putting two CPUs on one kernel stack.
	 * S413：恢复前先领。如果在扫描到这里之间别的核抢先领走了，就继续跑
	 * 当前线程，绝不让两个核同时落在一个内核栈上。
	 */
	if (!kthread_claim_cpu(t, cpu_id())) {
		/* S428: lost the race for a queued thread - put it back */
		sched_rq_put(t, cpu_id());
		if (me->t_tid == mp->uk_pid)
			mp->uk_regs = *r;
		else
			me->t_regs = *r;
		return;
	}
	cur = p;
	cur_thread = t;
	cpus[cpu_id()].ci_curthread = t;
	t->t_want_cpu = cpu_id();	/* S424: affinity hint */
	t->t_ticks = 0;		/* S424: fresh quantum, see proc_sched() */
	/* KLT: irq0's iret does not reload CR3, so switch it here —
	 * but only when the selected thread is from another process.
	 * Same-process thread switches keep the current page table. */
	/* S421: compare with the live CR3 - proc_exit() may have switched to
	 * IdlePTD behind cur_pd's back; trusting the cache here would iret to
	 * user mode with the kernel directory still loaded (#SS err=4). */
	if (p->uk_pd != hal_address_space_current()) {
		cur_pd = p->uk_pd;
		hal_address_space_activate(cur_pd);
	} else
		cur_pd = p->uk_pd;
	hal_kernel_stack_set(t->t_stack_top);
	*r = *regs;
	/*
	 * S428: the preempted thread is still KT_RUN, so it has to go back on a
	 * run queue before this CPU leaves its stack - another CPU may pick it
	 * up straight away.  (The idle carrier has t_proc == 0, so it is
	 * ignored by the queue code.)
	 */
	sched_rq_put(me, cpu_id());
	/* S424: this CPU is about to leave me's stack - release it next entry */
	sched_cpu[cpu_id()].sc_prev = me;
	sched_cpu[cpu_id()].sc_parked = 0;
	hal_intr_user_resume(r);
	(void)atomic_add((volatile int *)&sched_switches, 1);
	sched_cpu[cpu_id()].sc_switches++;
}

/* 中文：fork 实现：分配子进程、COW 克隆地址空间、继承 fd/管道/工作目录、复制上下文，子进程返回 0，父进程返回子 pid */
/* EN: fork implementation: allocate a child, COW-clone its address space, inherit fds/pipes/cwd, copy context; child returns 0, parent returns child pid */
static int
do_fork(struct sysregs *r)
{
	struct process *p;
	unsigned pd;

	if ((p = proc_alloc()) == 0)
		return (-EAGAIN);
	pd = copy_address_space_cow(cur->uk_pd);
	if (pd == 0) {
		p->uk_state = P_FREE;
		return (-ENOMEM);
	}
	hal_address_space_activate(cur->uk_pd);		/* flush the parent's stale writable TLBs */
	p->uk_state = P_RUN;
	p->uk_pid = atomic_add(&next_pid, 1);
	p->uk_ppid = cur->uk_pid;
	p->uk_pd = pd;
	(void)proc_add_thread(p, p->uk_pid);	p->uk_p_next_tid = TID_USER_BASE + p->uk_pid * TID_STRIDE;
	p->uk_brk_mapped = cur->uk_brk_mapped;
	bcopy(cur->uk_p_cwd, p->uk_p_cwd, sizeof(p->uk_p_cwd));
	bcopy(cur->uk_p_comm, p->uk_p_comm, sizeof(p->uk_p_comm));
	bcopy(cur->uk_fds, p->uk_fds, sizeof(p->uk_fds));
	{
		int i;

		for (i = 0; i < NFD; i++)
			if (p->uk_fds[i].used && p->uk_fds[i].vp != 0)
				vn_ref(p->uk_fds[i].vp);
		for (i = 0; i < NFD; i++)
			if (p->uk_fds[i].used && p->uk_fds[i].type == FD_PIPE &&
			    p->uk_fds[i].pipeidx >= 0)
				pipe_addref(&pipes[p->uk_fds[i].pipeidx],
				    p->uk_fds[i].rw);
	}
	p->uk_regs = *r;
	p->uk_regs.eax = 0;		/* the child returns 0 from fork() */
	/*
	 * S413: publish the child last.  The TCB was created KT_BLOCK, so no
	 * CPU could dispatch it while uk_pd / uk_regs were still being filled;
	 * flipping it to KT_RUN here is the single point where the child
	 * becomes visible to every scheduler.
	 * S413：最后才发布子进程。TCB 建出来是 KT_BLOCK，填 uk_pd / uk_regs 期间
	 * 没有核能派发它；这里置 KT_RUN 是子进程对所有调度器可见的唯一时刻。
	 */
	if (p->uk_p_thread != 0)
		sched_runnable(p->uk_p_thread, cpu_id());
	sched_kick_other();		/* let an idle AP take the child now */

	return (p->uk_pid);
}

/* 中文：vfork 实现：用 COW 私有拷贝共享地址空间，父进程阻塞直到子进程 exec/exit，子进程返回 0 */
/* EN: vfork implementation: use a COW private copy of the shared address space, block the parent until the child execs/exits, child returns 0 */
static int
do_vfork(struct sysregs *r)
{
	struct process *p;
	unsigned pd;

	if ((p = proc_alloc()) == 0)
		return (-EAGAIN);
	/*
	 * A strict vfork shares the address space, but our userland has no
	 * way to protect the parent's stack frame while the child runs (the
	 * child's own libc calls would clobber it).  A private copy plus the
	 * "parent waits for exec/exit" rule is a safe superset.  The private
	 * copy is COW, so vfork is cheap until either side writes.
	 */
	pd = copy_address_space_cow(cur->uk_pd);
	if (pd == 0) {
		p->uk_state = P_FREE;
		return (-ENOMEM);
	}
	hal_address_space_activate(cur->uk_pd);
	p->uk_state = P_RUN;
	p->uk_pid = atomic_add(&next_pid, 1);
	p->uk_ppid = cur->uk_pid;
	p->uk_pd = pd;
	(void)proc_add_thread(p, p->uk_pid);
	p->uk_p_next_tid = TID_USER_BASE + p->uk_pid * TID_STRIDE;
	p->uk_brk_mapped = cur->uk_brk_mapped;
	bcopy(cur->uk_p_cwd, p->uk_p_cwd, sizeof(p->uk_p_cwd));
	bcopy(cur->uk_p_comm, p->uk_p_comm, sizeof(p->uk_p_comm));
	bcopy(cur->uk_fds, p->uk_fds, sizeof(p->uk_fds));
	p->uk_regs = *r;
	p->uk_regs.eax = 0;
	p->uk_vfork_child = 1;
	/* S413: same publish-last rule as do_fork(). */
	if (p->uk_p_thread != 0)
		sched_runnable(p->uk_p_thread, cpu_id());
	sched_kick_other();

	/* the parent blocks until the child execs or exits */
	cur->uk_regs = *r;
	kthread_block_tcb(0);
	cur->uk_vfork_wait = 1;
	proc_sched();
	return (0);
}

#define	QUIESCE_TICKS	100	/* S425: max 1s wait for sibling CPUs */

/*
 * S425: make sure no CPU can still be running on this address space before
 * the page directory is handed back to the allocator.
 *
 * S423 fixed the single-CPU half of this (the exiting thread switched to
 * IdlePTD before publishing KT_ZOMBIE), but with threads the *siblings*
 * matter too: they are running on other CPUs with this PD loaded.  Once
 * free_address_space() returns the directory page to the pool, those CPUs
 * fault while fetching the page-fault entry itself (_Xpage is only mapped
 * through the PD) -> double -> triple fault.
 *
 * t_oncpu is exactly the "some CPU may still be on this TCB's stack" flag,
 * and a CPU clears it only after switching off that stack (and therefore
 * off this CR3), so waiting for it to reach -1 is the right barrier.
 *
 * Returns 1 when the address space is quiesced, 0 on timeout - the caller
 * then leaks the directory instead of corrupting a live CPU.
 */
static int
addrspace_quiesce(struct process *p)
{
	struct kthread *skip = cur_thread;
	int i, cid = cpu_id(), busy;
	unsigned deadline;

	if (!smp_enabled)
		return (1);
	/*
	 * This CPU is running on `skip`'s stack, so any other TCB it still
	 * "owns" is by definition off-stack and can be dropped right now.
	 */
	for (i = 0; i < nkthread; i++) {
		struct kthread *t = &kthreads[i];

		if (!t->t_used || t->t_proc != p || t == skip)
			continue;
		if (t->t_oncpu == cid)
			kthread_release_cpu(t);
		else if (t->t_oncpu >= 0)
			smp_resched_cpu(t->t_oncpu);
	}
	deadline = clock_get_ticks() + QUIESCE_TICKS;
	for (;;) {
		busy = -1;
		for (i = 0; i < nkthread; i++) {
			struct kthread *t = &kthreads[i];

			if (!t->t_used || t->t_proc != p || t == skip)
				continue;
			if (t->t_oncpu >= 0) {
				/*
				 * S424: a CPU parked in the idle loop has
				 * already switched CR3 to IdlePTD, so it is
				 * off this address space even though it still
				 * owns the TCB.
				 */
				if (sched_cpu[t->t_oncpu].sc_parked)
					continue;
				busy = i;
				break;
			}
		}
		if (busy < 0)
			return (1);
		if ((int)(clock_get_ticks() - deadline) >= 0) {
			printf("smp: pid %d: address space busy (tid %d cpu %d),"
			    " directory leaked\n", p->uk_pid,
			    kthreads[busy].t_tid, kthreads[busy].t_oncpu);
			return (0);
		}
		/*
		 * A sibling may be spinning in smp_bkl_enter() with our CR3
		 * loaded; it cannot make progress while we hold the BKL, so
		 * let it through.  The process is P_EXITING, so nothing can
		 * dispatch it or reap it while the lock is dropped.
		 */
		smp_bkl_drop();
		smp_bkl_enter();
	}
}

/* 中文：进程退出：唤醒 vfork/wait4 等待者并写状态，关闭所有 fd、释放线程与 mmap 及地址空间，置为僵尸或空闲 */
/* EN: Process exit: wake vfork/wait4 waiters with status, close all fds, free thread/mmap/address space, then mark zombie or free */
static void
proc_exit(int code)
{
	struct process *p;
	int i, reaped = 0;

	for (i = 0; i < NPROC; i++) {
		p = &procs[i];
		if (p->uk_state != P_BLOCKED || p->uk_pid != cur->uk_ppid)
			continue;
		if (p->uk_vfork_wait) {
			p->uk_vfork_wait = 0;
			p->uk_regs.eax = cur->uk_pid;
			proc_wake(p);
			continue;
		}
		if (p->uk_wait_pid != 0 && p->uk_wait_pid != cur->uk_pid)
			continue;
		if (p->uk_wait_status != 0 && user_ok(p->uk_wait_status, 4)) {
			unsigned save = hal_address_space_current();

			hal_address_space_activate(p->uk_pd);
			*(int *)p->uk_wait_status = cur->uk_p_termsig ?
			    (cur->uk_p_termsig & 0x7f) : ((code & 0xff) << 8);
			hal_address_space_activate(save);
			cur_pd = save;
		}
		p->uk_regs.eax = cur->uk_pid;
		proc_wake(p);
		reaped = 1;		/* the parent's wait4 gets the status */
	}

	/*
	 * S425: retire every sibling thread first.  A CPU may only resume a
	 * KT_RUN thread, so flipping them to KT_ZOMBIE is what stops new
	 * dispatches while the address space is torn down underneath them.
	 */
	cur->uk_state = P_EXITING;
	for (i = 0; i < nkthread; i++) {
		struct kthread *t = &kthreads[i];

		if (!t->t_used || t->t_proc != cur || t == cur_thread)
			continue;
		sched_unrunnable(t, KT_ZOMBIE);	/* S428: off the queue */
	}
	if (!addrspace_quiesce(cur)) {
		/*
		 * Degraded mode: a CPU is still on our CR3.  Keep the
		 * directory alive (leak it) rather than hand a live CPU a
		 * freed page table.
		 */
		cur->uk_pd_pinned = 1;
	}

	{
		int fd;

		for (fd = 0; fd < NFD; fd++)
			if (cur->uk_fds[fd].used)
				fd_close(fd);
	}
	/* S300: close this process's IPC ports; wake waiters with EIO */
	ipc_proc_exit((unsigned)cur->uk_pid);
	/*
	 * S17: release mmap bookkeeping first.  Shared page-cache pages must
	 * be unmapped here so free_address_space() does not return them to the
	 * page pool; private pages are left for it to free.
	 */
	mmap_teardown(cur);
	/*
	 * S10.1: give the address space back.  Switch to the kernel page
	 * directory first - the user PD is about to be freed and we are
	 * still running on its kernel-stack mapping (0x3f6 is shared, so the
	 * stack itself stays valid on IdlePTD).
	 */
	if (cur->uk_pd != 0) {
		unsigned save = cur->uk_pd;

		/* CR3 wants the *physical* address (IdlePTD), not pm_pdir's VA */
		hal_address_space_activate((unsigned)IdlePTD);
		cur_pd = (unsigned)IdlePTD;
		if (!cur->uk_pd_pinned)
			free_address_space(save);
		cur->uk_pd = 0;
	}
	/*
	 * S425: release every TCB of this process, not just the main one.
	 * thread_exit_current() can reach here from any thread (the last one
	 * to exit finishes the process), so the siblings - including the
	 * early-exited main thread, which used to be left as a zombie no one
	 * could ever reap - have to be freed here.  The current TCB goes
	 * last: kthread_user_free() then takes the deferred path, so its
	 * stack is only republished after this CPU has switched off it.
	 */
	for (i = 0; i < nkthread; i++) {
		struct kthread *t = &kthreads[i];

		if (t == cur_thread || !t->t_used || t->t_proc != cur)
			continue;
		kthread_user_free(t);
	}
	cur->uk_p_thread = 0;
	if (cur_thread != 0)
		kthread_user_free(cur_thread);
	/*
	 * S425: orphans.  Nothing reaps on behalf of a dead parent, so a
	 * child whose parent disappears would keep pointing at a pid that no
	 * longer waits for it: its own zombie would never be collected and
	 * its children would be un-waitable forever (NPROC is 1024).  Adopt
	 * live children to init and release the slots of children that are
	 * already zombies - nobody is left to read their status.
	 */
	if (cur->uk_pid != 1) {
		for (i = 0; i < NPROC; i++) {
			struct process *c = &procs[i];

			if (c == cur || c->uk_state == P_FREE ||
			    c->uk_ppid != cur->uk_pid)
				continue;
			c->uk_ppid = 1;		/* adopted by init */
			if (c->uk_state == P_ZOMBIE)
				c->uk_state = P_FREE;	/* nobody to reap it */
		}
	}
	cur->uk_exitcode = code;
	if (cur->uk_ppid == 0) {
		cur->uk_state = P_FREE;
		printf("\n[user exit %d]\n", code);
		mem_report();		/* the whole workload is gone: idle snapshot */
		for (;;)
			hal_cpu_halt();
	}
	/*
	 * S425: if there is no parent left to wait for us, do not leave a
	 * zombie behind - free the slot now.  `reaped` means a parent was
	 * blocked in wait4() and already got the pid and status above.
	 */
	p = proc_find(cur->uk_ppid);
	if (reaped || p == 0 || p->uk_state == P_FREE ||
	    p->uk_state == P_EXITING)
		cur->uk_state = P_FREE;
	else
		cur->uk_state = P_ZOMBIE;
	proc_sched();
}

/*
 * A user-mode exception (SIGSEGV/SIGFPE/SIGILL/...).  os.md mechanism 1:
 * the application dies, the kernel survives.  The faulting process is
 * terminated with the signal recorded in p_termsig so wait4() reports
 * WIFSIGNALED; the scheduler then runs another thread.
 */
/* 中文：用户态异常（SIGSEGV 等）处理：记录 p_termsig、终止该进程所有线程并调用 proc_exit，保证应用死而内核存活 */
/* EN: Handle a user-mode exception: record p_termsig, terminate all threads and call proc_exit so the app dies but the kernel survives */
void
user_fault(int sig, unsigned eip, unsigned va)
{
	int i;

	if (cur == 0) {
		printf("\nfault in kernel context: sig %d eip %x va %x\n",
		    sig, eip, va);
		for (;;)
			hal_cpu_halt();
	}
	printf("\nuser fault: pid %d sig %d eip %x va %x\n",
	    cur->uk_pid, sig, eip, va);
	cur->uk_p_termsig = sig;
	/* a thread-fatal signal ends the whole process (os.md 9.2/9.4) */
	for (i = 0; i < nkthread; i++) {
		struct kthread *t = &kthreads[i];

		if (t->t_used && t->t_proc == cur && t->t_tid != cur->uk_pid) {
			sched_unrunnable(t, KT_ZOMBIE);
			t->t_exitcode = 128 + sig;
		}
	}
	proc_exit(0);
}

/* ---------------------------------------------------------------- signals */

static int	console_fg_pgrp = 1;

struct sigframe {
	unsigned	magic;
	unsigned	sig;
	unsigned	mask;
	unsigned	eax, ebx, ecx, edx, esi, edi, ebp;
	unsigned	ds, es;
	unsigned	eip, cs, eflags, uesp, ss;
};

#define	SIGFRAME_MAGIC	0x53494746	/* "SIGF" */
#define	SIGFRAME_SIZE	(sizeof(struct sigframe))
#define	SIGTRAMP		8	/* movl $103,%eax ; int $0x80 ; hlt */

/* 0 = ignore, 1 = terminate, 2 = stop */
/* 中文：返回信号的默认动作：忽略、终止(1)或停止(2)，SIGCHLD/SIGCONT 等忽略，停止类信号返回 2 */
/* EN: Return a signal's default action: ignore, terminate (1) or stop (2); SIGCHLD/SIGCONT are ignored, stop signals return 2 */
static int
sig_default(int sig)
{

	switch (sig) {
	case SIGCHLD: case SIGCONT: case 16: case 23: case 28: case 29:
		return (0);
	case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
		return (2);
	default:
		return (1);
	}
}

/* 中文：向进程 p 发送信号：SIGCONT 恢复停止进程，否则置挂起位；阻塞进程被置可运行并收到 -EINTR */
/* EN: Send a signal to process p: SIGCONT resumes a stopped process, else set the pending bit; a blocked process becomes runnable with -EINTR */
static void
sig_send(struct process *p, int sig)
{

	if (p == 0 || p->uk_state == P_FREE || sig <= 0 || sig >= NSIG)
		return;
	if (sig == SIGCONT) {
		if (p->uk_state == P_STOPPED) {
			p->uk_state = P_RUN;
			p->uk_p_stopsig = 0;
			p->uk_p_cont = 1;
		}
		return;
	}
	p->uk_p_sig |= SIG_TO_MASK(sig);
	/*
	 * S425: SIGKILL must also reach a stopped process - it never runs, so
	 * it could never execute the default terminate action itself and sat
	 * there forever holding its process slot.
	 */
	if (p->uk_state == P_STOPPED) {
		if (sig == SIGKILL) {
			p->uk_state = P_RUN;
			p->uk_p_stopsig = 0;
		}
		return;
	}
	/* a blocked process gets EINTR and is made runnable again */
	if (p->uk_state == P_BLOCKED) {
		proc_wake(p);
		p->uk_regs.eax = (unsigned)-EINTR;
	}
}

/* 中文：向进程组 pgrp 的所有进程发送信号 */
/* EN: Send a signal to every process in process group pgrp */
static void
sig_send_pgrp(int pgrp, int sig)
{
	int i;

	for (i = 0; i < NPROC; i++)
		if (procs[i].uk_state != P_FREE && procs[i].p_pgrp == pgrp)
			sig_send(&procs[i], sig);
}

/* 中文：按 pid 向单个进程发送信号，找不到则忽略 */
/* EN: Send a signal to a single process by pid, ignoring it if not found */
static void
sig_send_pid(int pid, int sig)
{
	struct process *p = proc_find(pid);

	if (p != 0)
		sig_send(p, sig);
}

/* push a sigframe + trampoline on the user stack and point the frame at it */
/* 中文：在用户栈上压入 sigframe 与返回跳板（sigreturn 系统调用），把上下文改为信号处理入口；栈无效返回 -1 */
/* EN: Push a sigframe and sigreturn trampoline on the user stack and redirect context to the handler; returns -1 for a bad stack */
static int
sig_setup(struct process *p, struct sysregs *r, int sig)
{
	static const unsigned char tramp[8] = {
		0xb8, 0x67, 0x00, 0x00, 0x00,	/* movl $103,%eax	*/
		0xcd, 0x80,			/* int $0x80		*/
		0xf4				/* hlt			*/
	};
	struct sigframe *fp;
	unsigned sp = r->uesp & ~3u;
	unsigned stub;
	int i;

	sp -= SIGFRAME_SIZE;
	if (sp < 0x08000000 || !user_ok(sp, SIGFRAME_SIZE))
		return (-1);
	fp = (struct sigframe *)sp;
	fp->magic = SIGFRAME_MAGIC;
	fp->sig = sig;
	fp->mask = p->uk_p_sigmask;
	fp->eax = r->eax; fp->ebx = r->ebx; fp->ecx = r->ecx; fp->edx = r->edx;
	fp->esi = r->esi; fp->edi = r->edi; fp->ebp = r->ebp;
	fp->ds = r->ds; fp->es = r->es;
	fp->eip = r->eip; fp->cs = r->cs; fp->eflags = r->eflags;
	fp->uesp = r->uesp; fp->ss = r->ss;

	sp -= SIGTRAMP;
	stub = sp;
	{
		unsigned char *d = (unsigned char *)sp;

		for (i = 0; i < SIGTRAMP; i++)
			d[i] = tramp[i];
	}
	sp -= 4;
	*(unsigned *)sp = (unsigned)sig;	/* handler argument */
	sp -= 4;
	*(unsigned *)sp = stub;			/* return address -> trampoline */

	r->uesp = sp;
	r->eip = p->uk_p_sighand[sig];
	r->eax = (unsigned)sig;
	/* the handler runs with the signal blocked (no SA_NODEFER) */
	p->uk_p_sigmask |= SIG_TO_MASK(sig);
	return (0);
}

/* 中文：停止进程：置 P_STOPPED 与停止信号，并唤醒带 WUNTRACED 等待的父进程写入停止状态 */
/* EN: Stop a process: set P_STOPPED and the stop signal, then wake a WUNTRACED-waiting parent with the stop status */
static void
sig_stop(struct process *p, int sig)
{
	struct process *pp;

	p->uk_state = P_STOPPED;
	p->uk_p_stopsig = sig;
	pp = proc_find(p->uk_ppid);
	if (pp != 0 && pp->uk_state == P_BLOCKED && pp->uk_wait_status != 0 &&
	    (pp->uk_wait_options & WUNTRACED) &&
	    (pp->uk_wait_pid == 0 || pp->uk_wait_pid == p->uk_pid) &&
	    user_ok(pp->uk_wait_status, 4)) {
		hal_address_space_activate(pp->uk_pd);
		*(int *)pp->uk_wait_status = ((sig & 0xff) << 8) | 0x7f;
		hal_address_space_activate(cur->uk_pd);
		pp->uk_regs.eax = p->uk_pid;
		proc_wake(pp);
	}
}

/*
 * Deliver at most one signal per return to user mode.  The caller must run
 * with the target process's page directory loaded.
 */
/* 中文：返回用户态前投递至多一个信号：处理 SIGCONT/忽略/捕获/默认动作，默认终止则退出或置僵尸；调用前须已加载目标页目录 */
/* EN: Deliver at most one pending signal before returning to user mode: handle SIGCONT/ignore/catch/default, terminating or zombifying as needed; caller must have the target PD loaded */
static void
sig_deliver(struct process *p, struct sysregs *r)
{
	int sig;
	unsigned pend, mask;
	struct kthread *t = 0;

	if (cur_thread != 0 && cur_thread->t_proc == p)
		t = cur_thread;
	else if (p->uk_p_thread != 0)
		t = p->uk_p_thread;
	mask = (t != 0) ? t->t_sigmask : p->uk_p_sigmask;
	for (;;) {
		pend = (p->uk_p_sig | (t != 0 ? t->t_sig : 0)) & ~mask;
		pend |= (p->uk_p_sig | (t != 0 ? t->t_sig : 0)) &
		    (SIG_TO_MASK(SIGKILL) | SIG_TO_MASK(SIGSTOP));
		if (pend == 0)
			return;
		for (sig = 1; sig < NSIG; sig++)
			if (pend & SIG_TO_MASK(sig))
				break;
		if (sig >= NSIG)
			return;
		p->uk_p_sig &= ~SIG_TO_MASK(sig);
		if (t != 0)
			t->t_sig &= ~SIG_TO_MASK(sig);

		if (sig == SIGCONT) {
			if (p->uk_p_stopsig != 0) {
				p->uk_p_stopsig = 0;
				p->uk_state = P_RUN;
			}
			continue;
		}
		if (p->uk_p_sigignore & SIG_TO_MASK(sig))
			continue;
		if ((p->uk_p_sigcatch & SIG_TO_MASK(sig)) && r != 0 &&
		    sig_setup(p, r, sig) == 0)
			return;
		if (sig_default(sig) == 2) {
			sig_stop(p, sig);
			return;
		}
		if (sig_default(sig) == 0)
			continue;
		/* terminate */
		if (p != cur) {
			/*
			 * S425: unreachable today - both callers pass the
			 * process they just loaded (proc_sched and the
			 * syscall-exit path hand in `cur`).  The old code set
			 * P_ZOMBIE here, which is a half exit: no fd close,
			 * no IPC port release, no TCB teardown, no address
			 * space free.  Route the signal back through the
			 * normal pending path so the target exits through
			 * proc_exit() on its own CPU instead.
			 */
			sig_send(p, sig);
			return;
		}
		p->uk_p_termsig = sig;
		proc_exit(sig);		/* never returns */
	}
}
/* 中文：wait4 实现：回收指定（或任意）僵尸/停止子进程并写状态，无子进程返回 ECHILD，WNOHANG 立即返回，否则阻塞调度 */
/* EN: wait4 implementation: reap a specific (or any) zombie/stopped child and write status; ECHILD if none, WNOHANG returns immediately, else block and schedule */
static int
do_wait4(int want, unsigned statusp, int options, struct sysregs *r)
{
	struct process *p;
	int i, pid;

	for (i = 0; i < NPROC; i++) {
		p = &procs[i];
		if (p->uk_state == P_FREE || p->uk_ppid != cur->uk_pid)
			continue;
		if (want > 0 && p->uk_pid != want)
			continue;
		pid = p->uk_pid;
		if (p->uk_state == P_ZOMBIE) {
			if (statusp != 0 && user_ok(statusp, 4))
				*(int *)statusp = p->uk_p_termsig ?
				    (p->uk_p_termsig & 0x7f) :
				    ((p->uk_exitcode & 0xff) << 8);
			p->uk_state = P_FREE;
			r->eax = pid;
			return (0);
		}
		if (p->uk_state == P_STOPPED && (options & WUNTRACED)) {
			if (statusp != 0 && user_ok(statusp, 4))
				*(int *)statusp =
				    ((p->uk_p_stopsig & 0xff) << 8) | 0x7f;
			r->eax = pid;
			return (0);
		}
	}
	if (proc_children(cur->uk_pid) == 0) {
		r->eax = (unsigned)-ECHILD;
		return (0);
	}
	if (options & WNOHANG) {
		r->eax = 0;
		return (0);
	}
	cur->uk_regs = *r;
	cur->uk_wait_pid = (want > 0) ? want : 0;
	cur->uk_wait_status = statusp;
	cur->uk_wait_options = options;
	kthread_block_tcb(0);
	proc_sched();
	r->eax = 0;
	return (0);
}


/* ---- S13: render the process table for the /dev/ps pseudo file ---- */

/* 中文：把字符串 s 追加到缓冲区 buf 的偏移 o 处（留一字节余量），返回新偏移 */
/* EN: Append string s to buffer buf at offset o (leaving one byte of slack), returning the new offset */
static unsigned
ps_str(char *buf, unsigned o, unsigned len, const char *s)
{

	while (*s && o + 1 < len)
		buf[o++] = *s++;
	return (o);
}

/* 中文：把无符号数 v 以十进制追加到缓冲区 buf 的偏移 o 处，返回新偏移 */
/* EN: Append unsigned value v in decimal to buffer buf at offset o, returning the new offset */
static unsigned
ps_num(char *buf, unsigned o, unsigned len, unsigned v)
{
	char num[12];
	int n = 0;

	if (v == 0)
		num[n++] = '0';
	while (v > 0) {
		num[n++] = '0' + (v % 10);
		v /= 10;
	}
	while (n > 0 && o + 1 < len)
		buf[o++] = num[--n];
	return (o);
}

/* 中文：为 /dev/ps 生成进程表文本：逐进程列出 pid、ppid、pgrp、状态与 comm，NUL 结尾并返回长度 */
/* EN: Render the process table text for /dev/ps: list pid, ppid, pgrp, state and comm per process, NUL-terminate and return the length */
static int
proc_list_text(char *buf, unsigned len)
{
	unsigned o = 0;
	int i;

	for (i = 0; i < NPROC; i++) {
		const char *st;

		if (procs[i].uk_state == P_FREE)
			continue;
		switch (procs[i].uk_state) {
		case P_RUN:	st = "RUN"; break;
		case P_BLOCKED:	st = "BLOCK"; break;
		case P_ZOMBIE:	st = "ZOMB"; break;
		case P_STOPPED:	st = "STOP"; break;
		default:	st = "?"; break;
		}
		o = ps_num(buf, o, len, (unsigned)procs[i].uk_pid);
		o = ps_str(buf, o, len, " ");
		o = ps_num(buf, o, len, (unsigned)procs[i].uk_ppid);
		o = ps_str(buf, o, len, " ");
		o = ps_num(buf, o, len, (unsigned)procs[i].p_pgrp);
		o = ps_str(buf, o, len, " ");
		o = ps_str(buf, o, len, st);
		o = ps_str(buf, o, len, " ");
		o = ps_str(buf, o, len, procs[i].uk_p_comm);
		o = ps_str(buf, o, len, "\n");
	}
	if (o < len)
		buf[o] = 0;
	return ((int)o);
}

/* --------------------------------------------------------------- syscalls */
/* Absolutize a path argument once, here in the syscall layer. */
/* 中文：在系统调用层把用户路径参数 uptr 读入 rel 并用当前 cwd 绝对化到 buf，读取失败返回 -EFAULT */
/* EN: Read a user path argument at uptr into rel and absolutize it against the current cwd into buf; returns -EFAULT on read failure */
static int
path_arg(unsigned uptr, char *buf, unsigned len)
{
	char rel[128];

	if (user_str(uptr, rel, sizeof(rel)) < 0)
		return (-EFAULT);
	return (vfs_absolutize(cur->uk_p_cwd, rel, buf, len));
}



/* 中文：校验用户缓冲 [p,p+len) 可读：逐页检查映射，必要时补页/COW 私有化，全部有效返回 1，否则 0 */
/* EN: Validate that user buffer [p,p+len) is readable: check each page's mapping, demand-fault/COW as needed, return 1 if all valid else 0 */
static int
user_ok(unsigned p, unsigned len)
{
	unsigned a;

	if (len == 0)
		return (1);
	if (p < USER_LOAD || p >= 0xC0000000)
		return (0);
	if (len > 0xC0000000 - p)
		return (0);
	/*
	 * Walk the page tables instead of trusting the address range: a bad
	 * pointer inside the user window must return EFAULT, not fault in the
	 * kernel (os.md mechanism 1, access_ok + copy_from_user).
	 */
	for (a = p; a < p + len; a = (a + PAGE_SIZE) & ~(PAGE_SIZE - 1)) {
		unsigned *pt;
		unsigned pti;

		pti = user_pte_ptr(cur->uk_pd, a, &pt);
		if (pt == 0 || (pt[pti] & PG_V) == 0) {
			/*
			 * A legal heap address below brk is demand-paged here,
			 * so syscall buffers on untouched heap work without a
			 * user fault; anything else is EFAULT.
			 * brk 以下的合法堆地址在此按需映射，系统调用缓冲区
			 * 可直接使用未触碰的堆页；其它地址仍返回 EFAULT。
			 */
			if (!zcomp_refault(a) && !demand_page(a) &&
			    !mmap_fault(a & ~(PAGE_SIZE - 1)))
				return (0);
			pti = user_pte_ptr(cur->uk_pd, a, &pt);
			if (pt == 0 || (pt[pti] & PG_V) == 0)
				return (0);
		}
		if ((pt[pti] & PG_COW) != 0) {
			/* output paths write here: give the page a private copy */
			if (!user_page_fault(a, 7))
				return (0);
			pti = user_pte_ptr(cur->uk_pd, a, &pt);
			if (pt == 0 || (pt[pti] & PG_V) == 0)
				return (0);
		}
	}
	return (1);
}

/* Like user_ok(), but the kernel is going to write: the PTE must be RW. */
/* 中文：校验用户缓冲可写：在 user_ok 基础上额外要求每页 PTE 具 PG_RW，全部满足返回 1 */
/* EN: Validate that a user buffer is writable: on top of user_ok, additionally require PG_RW on every page's PTE, returning 1 if all satisfied */
static int
user_ok_w(unsigned p, unsigned len)
{
	unsigned a;

	if (!user_ok(p, len))
		return (0);
	for (a = p; a < p + len; a = (a + PAGE_SIZE) & ~(PAGE_SIZE - 1)) {
		unsigned *pt;
		unsigned pti = user_pte_ptr(cur->uk_pd, a, &pt);

		if (pt == 0 || (pt[pti] & PG_RW) == 0)
			return (0);
	}
	return (1);
}

/*
 * Page-validating user copies (os.md mechanism 1).  Named ucopy* to avoid
 * the 4.5BSD locore.s copyin/copyout assembly, which uses pcb_onfault and
 * is not wired into this port; the contract is the same: 0 or -EFAULT.
 */
/* 中文：校验用户地址后从用户缓冲拷贝 len 字节到内核 kaddr，成功返回 0，否则 -EFAULT */
/* EN: After validating the user address, copy len bytes from user buffer into kernel kaddr; returns 0 or -EFAULT */
static int
ucopyin(void *udaddr, void *kaddr, unsigned len)
{

	if (!user_ok((unsigned)udaddr, len))
		return (-EFAULT);
	bcopy(udaddr, kaddr, len);
	return (0);
}

/* 中文：校验用户地址可写后从内核 kaddr 拷贝 len 字节到用户缓冲，成功返回 0，否则 -EFAULT */
/* EN: After validating the user address is writable, copy len bytes from kernel kaddr into the user buffer; returns 0 or -EFAULT */
static int
ucopyout(void *kaddr, void *udaddr, unsigned len)
{

	if (!user_ok_w((unsigned)udaddr, len))
		return (-EFAULT);
	bcopy(kaddr, udaddr, len);
	return (0);
}

/*
 * S16: wall clock.  The kernel has no clock interrupt handler wired up, so
 * read the CMOS RTC directly instead: registers are indexed through port
 * 0x70 and read from 0x71 by rtcin() in locore.s.  The RTC holds local time
 * (the host's time zone); we hand it out as UTC until a timezone table
 * exists.  Sub-second resolution is not available, hence tv_usec = 0.
 */
extern unsigned char rtcin(int);

/* 中文：把 CMOS RTC 的 BCD 编码字节 v 转换为十进制整数 */
/* EN: Convert a BCD-encoded CMOS RTC byte v into a decimal integer */
static int
rtc_bcd(int v)
{

	return ((v & 0xf) + (v >> 4) * 10);
}

/* 中文：把年月日 (y,m,d) 换算为自 1970-01-01 起的天数，考虑闰年 */
/* EN: Convert a date (y,m,d) into days since 1970-01-01, accounting for leap years */
static long
rtc_days(int y, int m, int d)
{
	static const int mdays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243,
	    273, 304, 334 };
	long days = 0;
	int i, leap;

	for (i = 1970; i < y; i++) {
		leap = (i % 4 == 0 && (i % 100 != 0 || i % 400 == 0));
		days += 365 + leap;
	}
	days += mdays[m - 1];
	if (m > 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
		days++;
	days += d - 1;
	return (days);
}

/* 中文：直接从 CMOS RTC 读取当前时间并换算为 Unix 纪元秒数，处理 12/24 小时与 BCD/二进制格式，非法日期返回 0 */
/* EN: Read the current time directly from the CMOS RTC and convert to Unix epoch seconds, handling 12/24-hour and BCD/binary formats; invalid dates return 0 */
static long
rtc_epoch(void)
{
	int sec, min, hour, day, mon, year, i;
	unsigned char sb;

	/* wait for the update-in-progress flag to clear */
	for (i = 0; i < 1000000; i++)
		if ((rtcin(0x0a) & 0x80) == 0)
			break;
	sb = rtcin(0x0b);
	sec = rtcin(0x00);
	min = rtcin(0x02);
	hour = rtcin(0x04);
	day = rtcin(0x07);
	mon = rtcin(0x08);
	year = rtcin(0x09);
	if ((sb & 0x02) == 0 && (hour & 0x80) != 0)	/* 12-hour clock, PM */
		hour = (hour & 0x7f) + 12;
	if ((sb & 0x04) == 0) {				/* BCD, not binary */
		sec = rtc_bcd(sec);
		min = rtc_bcd(min);
		hour = rtc_bcd(hour);
		day = rtc_bcd(day);
		mon = rtc_bcd(mon);
		year = rtc_bcd(year);
	}
	if (mon < 1 || mon > 12 || day < 1 || day > 31)
		return (0);
	return (rtc_days(year + 2000, mon, day) * 86400L +
	    hour * 3600L + min * 60L + sec);
}

/*
 * Fill a 4.4BSD i386 `struct stat` (96 bytes):
 *	0 dev, 4 ino, 8 mode(16), 10 nlink(16), 12 uid, 16 gid, 20 rdev,
 *	24 atimespec, 32 mtimespec, 40 ctimespec,
 *	48 size(64), 56 blocks(64), 64 blksize, 68 flags, 72 gen,
 *	76 lspare, 80 qspare[2]
 */
#define	STAT_SIZE	96

/* 中文：填充 4.4BSD i386 struct stat（96 字节）：清零后置 dev/ino/mode/nlink/size/blksize 等字段 */
/* EN: Fill a 4.4BSD i386 struct stat (96 bytes): zero it then set dev/ino/mode/nlink/size/blksize fields */
static void
fill_stat(unsigned mode, unsigned size, unsigned *sb)
{
	char *p = (char *)sb;
	int i;

	for (i = 0; i < STAT_SIZE; i++)
		p[i] = 0;
	*(unsigned *)(p + 0) = 0;		/* st_dev */
	*(unsigned *)(p + 4) = 1;		/* st_ino */
	*(unsigned short *)(p + 8) = (unsigned short)mode;
	*(unsigned short *)(p + 10) = 1;	/* st_nlink */
	*(unsigned *)(p + 48) = size;		/* st_size */
	*(unsigned *)(p + 52) = 0;
	*(unsigned *)(p + 64) = 4096;		/* st_blksize */
}

/* 中文：exec 核心：经 VFS 读取可执行文件到内核窗口，从用户态复制 argv/envp，构建新映像并切换地址空间与上下文；vfork 子进程唤醒父进程 */
/* EN: exec core: read the executable via VFS into a kernel window, copy argv/envp from user, build a new image and switch address space and context; a vfork child wakes its parent */
static int
exec_image(const char *path, unsigned uargv, unsigned uenvp, struct sysregs *r)
{
	struct file *f;
	const unsigned char *img;
	unsigned len;
	unsigned pd, entry, esp;
	char *argv[MAXARG], *envp[MAXENV];
	static char argbuf[1024];
	int argc = 0, envc = 0;
	unsigned p, n;
	char *b = argbuf;

	/*
	 * Prefer the ISO9660 root: S4 moved /sbin/init and /bin/sh onto the
	 * CD.  The embedded ramfs images stay as a fallback for a kernel
	 * booted without a disc.
	 */
	f = 0;
	{
		struct vnode *vp = 0;
		unsigned char *buf;
		int rv;

		if (vfs_namei(path, &vp) < 0 || vp->type != VREG)
			return (-ENOENT);
		len = vp->size;
		buf = cd_map(len);
		if (buf == 0) {
			vn_put(vp);
			return (-ENOMEM);
		}
		rv = vp->ops->read(vp, 0, buf, len);
		if (rv < 0 || (unsigned)rv != len) {
			vn_put(vp);
			return (-EIO);
		}
		printf("execve %s: via vnode (%d bytes)\n", path, len);
		img = buf;
		vn_put(vp);
	}

	if (uargv != 0) {
		while (argc < MAXARG && user_word(uargv, &p) == 0 && p != 0) {
			n = user_str(p, b, 128);
			if ((int)n < 0)
				return (-EFAULT);
			argv[argc++] = b;
			b += n + 1;
			uargv += 4;
		}
	}
	envp[envc++] = "PATH=/bin:/sbin";

	pd = build_image(img, len, &entry, argv, argc, envp, envc, &esp);
	/* S17: the old address space is replaced, drop its mmap references */
	mmap_teardown(cur);
	cur->uk_pd = pd;
	cur->uk_brk_mapped = USER_HEAP_START;
	printf("execve %s: entry %x esp %x pd %x argc %d\n",
	    path, entry, esp, pd, argc);
	hal_address_space_activate(pd);
	r->eip = entry;
	r->uesp = esp;
	r->eax = 0;
	if (cur->uk_vfork_child) {
		struct process *pp = proc_find(cur->uk_ppid);

		cur->uk_vfork_child = 0;
		if (pp != 0 && pp->uk_state == P_BLOCKED && pp->uk_vfork_wait) {
			pp->uk_vfork_wait = 0;
			pp->uk_regs.eax = cur->uk_pid;
			proc_wake(pp);
			cur->uk_regs = *r;
			cur->uk_state = P_RUN;
			cur = pp;
			cur_thread = pp->uk_p_thread;
			if (cur_thread != 0)
				hal_kernel_stack_set(cur_thread->t_stack_top);
			resume_user(pp->uk_pd, &pp->uk_regs);
		}
	}
	return (0);
}


/*
 * Canonical console input for fd 0: echo, erase (BS/DEL), kill (^U),
 * word-erase (^W) and EOF (^D on an empty line).
 */
/* 中文：控制台 fd 0 的规范输入：回显并处理退格、^U 清行、^W 清词、^D EOF，以及 ^C/^Z/^\ 向前台进程组发信号 */
/* EN: Canonical console input for fd 0: echo with backspace, ^U kill-line, ^W kill-word, ^D EOF and ^C/^Z/^\ signals to the foreground pgrp */
static int
console_read(char *buf, unsigned len)
{
	unsigned i = 0;
	int c;

	while (i < len - 1) {
		c = cngetc();
		if (c == 0)
			continue;
		if (c == '\r' || c == '\n') {
			cnputc('\n');
			buf[i++] = '\n';
			break;
		}
		if (c == '\b' || c == 0x7f) {
			if (i > 0) {
				i--;
				cnputc('\b');
				cnputc(' ');
				cnputc('\b');
			}
			continue;
		}
		if (c == 0x15) {		/* ^U: kill line */
			while (i > 0) {
				i--;
				cnputc('\b');
				cnputc(' ');
				cnputc('\b');
			}
			continue;
		}
		if (c == 0x17) {		/* ^W: kill word */
			while (i > 0 && (buf[i - 1] == ' ' || buf[i - 1] == '\t')) {
				i--;
				cnputc('\b');
				cnputc(' ');
				cnputc('\b');
			}
			while (i > 0 && buf[i - 1] != ' ' && buf[i - 1] != '\t') {
				i--;
				cnputc('\b');
				cnputc(' ');
				cnputc('\b');
			}
			continue;
		}
		if (c == 0x04)			/* ^D: EOF on an empty line */
			break;
		if (c == 0x03) {		/* ^C: SIGINT to the foreground pgrp */
			cnputc('^');
			cnputc('C');
			cnputc('\n');
			sig_send_pgrp(console_fg_pgrp, SIGINT);
			return (-EINTR);
		}
		if (c == 0x1a) {		/* ^Z: SIGTSTP */
			cnputc('^');
			cnputc('Z');
			cnputc('\n');
			sig_send_pgrp(console_fg_pgrp, SIGTSTP);
			return (-EINTR);
		}
		if (c == 0x1c) {		/* ^\: SIGQUIT */
			cnputc('^');
			cnputc('\\');
			cnputc('\n');
			sig_send_pgrp(console_fg_pgrp, SIGQUIT);
			return (-EINTR);
		}
		if (c < 0x20 && c != '\t')
			continue;
		cnputc(c);
		buf[i++] = c;
	}
	return ((int)i);
}

/*
 * Termios for the console.  There is no real UART line discipline: the
 * console is always in canonical mode with echo, which is exactly what
 * isatty(3)/sh want to see.
 */
/* 中文：控制台 termios 处理：get 时填充规范模式+回显的 termios 结构，set 时接受但忽略（标志固定） */
/* EN: Console termios handling: on get fill a canonical+echo termios struct; on set accept but ignore since flags are fixed */
static int
console_termios(unsigned arg, int get)
{
	struct termios *t = (struct termios *)arg;

	if (!user_ok(arg, sizeof(*t)))
		return (-EFAULT);
	if (get) {
		bzero(t, sizeof(*t));
		t->c_iflag = ICRNL | IXON;
		t->c_oflag = OPOST | ONLCR;
		t->c_cflag = CS8 | CREAD | CLOCAL | B9600;
		t->c_lflag = ICANON | ECHO | ECHOE | ISIG | IEXTEN;
		t->c_cc[VINTR] = 3;
		t->c_cc[VEOF] = 4;
		t->c_cc[VERASE] = 0x7f;
		t->c_cc[VKILL] = 0x15;
		t->c_cc[VSTART] = 0x11;
		t->c_cc[VSTOP] = 0x13;
		t->c_cc[VSUSP] = 0x1a;
		t->c_cc[VMIN] = 1;
		t->c_cc[VTIME] = 0;
	}
	/* TIOCSETA*: accept and ignore, the console flags are fixed */
	return (0);
}

/*
 * The native 4.5BSD service ABI.  Personalities call this after translating
 * their own call numbers; it is the only function that knows the syscall
 * switch internals.
 */
/*
 * S25: user threads.  The main thread's context remains in proc->regs (so
 * the existing fork/wait/pipe paths are unchanged); additional threads keep
 * their context in kthread.t_regs and are scheduled by the thread table.
 */
/* 中文：创建用户级线程：分配 tid 与用户栈（或采用指定栈顶）、生成 kthread、设置入口/参数/上下文并置为可运行，返回 tid */
/* EN: Create a user thread: allocate a tid and user stack (or use a given top), spawn a kthread, set entry/arg/context and mark runnable, returning the tid */
static int
thread_create(struct sysregs *r)
{
	struct kthread *t;
	int tid;
	unsigned sva, top;

	/*
	 * S411: each extra thread owns a fixed 2 MB user stack slot.  The
	 * slot is derived from the low tid bits and pages are demand-paged,
	 * so creating a thread costs no stack memory until it runs.  More
	 * than UTHREAD_STACKS threads per process would alias slots, so the
	 * 16-slot window is also the per-process thread cap (TID_STRIDE=16).
	 * S411：每个线程固定 2MB 用户栈槽，按 tid 低位取槽，页按需分配；
	 * 超过 16 个线程会槽位重叠，因此 16 槽窗口同时是单进程线程上限。
	 */
	if (cur->uk_p_nthreads >= UTHREAD_STACKS)
		return (-EAGAIN);
	tid = cur->uk_p_next_tid++;
	if (r->edx != 0)
		top = r->edx;
	else {
		sva = UTHREAD_STACK_BASE +
		    ((unsigned)(tid & (UTHREAD_STACKS - 1)) * UTHREAD_STACK_SIZE);
		top = sva + UTHREAD_STACK_SIZE - 16;
	}
	t = kthread_user_spawn(cur, tid);
	if (t == 0)
		return (-EAGAIN);
	t->t_regs = *r;
	t->t_regs.eip = r->ebx;		/* entry */
	t->t_regs.eax = r->ecx;		/* arg */
	t->t_regs.uesp = top;		/* user stack top */
	t->t_regs.ebp = 0;
	t->t_sigmask = (cur_thread != 0) ? cur_thread->t_sigmask :
	    cur->uk_p_sigmask;
	/* S428: publish through the queue, see sched_runnable() */
	sched_runnable(t, cpu_id());
	/* kthread_user_spawn already linked t and bumped uk_p_nthreads */
	return (tid);
}

/* 中文：终止当前线程：主线程走 proc_exit，其余线程置僵尸、减少计数、唤醒 join 者，分离线程则释放，最后调度 */
/* EN: Exit the current thread: main thread calls proc_exit; others become zombie, decrement count, wake joiners, free if detached, then schedule */
static void
thread_exit_current(int code)
{
	struct kthread *t = cur_thread;
	int i;

	if (t == 0 || t->t_tid == cur->uk_pid) {
		if (t == 0 || cur->uk_p_nthreads <= 1) {
			proc_exit(code);
			return;
		}
		/*
		 * S425: the main thread exits while its siblings are still
		 * alive.  The old code set KT_ZOMBIE and then called
		 * kthread_block_tcb(), whose multi-threaded branch rewrites
		 * t_state to KT_BLOCK - so the TCB was neither a zombie nor
		 * runnable, uk_p_nthreads was never decremented, and nothing
		 * ever finished the process when the last sibling exited:
		 * wait4() blocked forever and the address space leaked.
		 * Retire the TCB properly and hand the "finish the process"
		 * duty to the last thread to exit.
		 */
		cur->uk_exitcode = code;	/* the process status */
		cur->uk_p_maingone = 1;
		cur->uk_p_nthreads--;		/* S424: single owner */
		t->t_exitcode = code;
		hal_address_space_activate((unsigned)IdlePTD);
		cur_pd = (unsigned)IdlePTD;
		sched_unrunnable(t, KT_ZOMBIE);	/* S428 */
		prio_restore(t);
		proc_sched();			/* zombie: not resumable */
		return;
	}
	/*
	 * Leave the user address space BEFORE publishing KT_ZOMBIE.  The main
	 * thread may free the whole address space as soon as thread_join()
	 * observes the zombie state; if this CPU then sat in proc_sched()
	 * with the process CR3 still loaded, the page-directory page would
	 * already be back in the allocator.  A later trap would fault while
	 * fetching _Xpage itself (the kernel PDE 0x3f8 is only present while
	 * the PD is alive), escalating to a double/triple fault - exactly the
	 * pthpre-exit crash.  Publish the zombie state only after CR3 is on
	 * the kernel directory.
	 */
	hal_address_space_activate((unsigned)IdlePTD);
	cur_pd = (unsigned)IdlePTD;
	t->t_exitcode = code;
	sched_unrunnable(t, KT_ZOMBIE);	/* S428: off the queue */
	/* release inherited priority: the resource (this thread) is done */
	prio_restore(t);
	cur->uk_p_nthreads--;		/* S424: single owner of this counter */
	/*
	 * S425: this was the last live thread of the process.  Somebody has
	 * to run the process-level teardown now (nothing else will): the
	 * main thread may have exited long ago (uk_p_maingone) and is only
	 * waiting to be freed.  proc_exit() frees every remaining TCB,
	 * including this one, and never returns.
	 */
	if (cur->uk_p_nthreads <= 0) {
		proc_exit(cur->uk_p_maingone ? cur->uk_exitcode : code);
		return;
	}
	for (i = 0; i < nkthread; i++) {
		struct kthread *j = &kthreads[i];

		if (!j->t_used || j->t_join_tid != t->t_tid)
			continue;
		j->t_join_tid = 0;
		j->t_blocked_on = -1;	/* inherited priority released on wake */
		if (j->t_tid == j->t_proc->uk_pid) {
			j->t_proc->uk_regs.eax = code;
			proc_wake(j->t_proc);
		} else {
			j->t_regs.eax = code;
			sched_runnable(j, j->t_oncpu >= 0 ? j->t_oncpu :
			    cpu_id());
		}
	}
	if (t->t_detached) {
		/*
		 * Keep cur_thread == t across the free so kthread_user_free()
		 * takes the deferred path.  proc_sched() below keeps running
		 * on t's 16 KB stack; clearing cur_thread first (as the old
		 * code did) defeated the "this CPU is still on that stack"
		 * check and let a concurrent tcb_reserve()+kstack_alloc()
		 * bzero a live stack - the detached-thread form of TRAP 12.
		 */
		kthread_user_free(t);
	}
	proc_sched();
}

/* 中文：分离线程 tid：找不到返回 ESRCH，已被 join 返回 EINVAL，否则置分离标志 */
/* EN: Detach thread tid: ESRCH if not found, EINVAL if already joined, else set the detached flag */
static int
thread_detach(int tid)
{
	struct kthread *t;

	t = kthread_user_find(tid);
	if (t == 0)
		return (-ESRCH);
	if (t->t_join_tid != 0)
		return (-EINVAL);
	t->t_detached = 1;
	return (0);
}

/* 中文：等待线程 tid：已僵尸则取退出码并释放，否则当前线程记录 join_tid、保存上下文、阻塞并调度 */
/* EN: Join thread tid: if zombie take its exit code and free it, else record join_tid, save context, block and schedule */
static int
thread_join(int tid, struct sysregs *r)
{
	struct kthread *t, *jt;
	int code;

	t = kthread_user_find(tid);
	if (t == 0)
		return (-ESRCH);
	if (t->t_state == KT_ZOMBIE) {
		code = t->t_exitcode;
		/*
		 * S424: uk_p_nthreads was already decremented when this TCB
		 * became a zombie (thread_exit_current), and kthread_user_free()
		 * no longer touches it.  The extra decrement that used to live
		 * here made a joined thread cost 2-3 counts, so a process with
		 * live runnable siblings could look single-threaded and have
		 * them frozen by a blocking sibling.
		 */
		kthread_user_free(t);
		return (code);
	}
	jt = cur_thread;
	jt->t_join_tid = tid;
	jt->t_blocked_on = t->t_tid;
	/* priority inheritance: if the joiner outranks the joinee, lend it
	 * our priority so it finishes and releases us quickly. */
	prio_inherit(jt, t);
	if (jt->t_tid == cur->uk_pid) {
		cur->uk_regs = *r;
		kthread_block_tcb(0);
	} else {
		jt->t_regs = *r;
		sched_unrunnable(jt, KT_BLOCK);
	}
	proc_sched();
	return (0);
}

/* 中文：返回当前线程的 tid，无当前线程时返回进程 pid */
/* EN: Return the current thread's tid, or the process pid when there is no current thread */
static int
thread_self(void)
{

	return (cur_thread != 0 ? cur_thread->t_tid : cur->uk_pid);
}

/* 中文：线程让出 CPU：保存当前上下文并置为可运行，然后调度其它线程 */
/* EN: Yield the CPU: save the current context and mark it runnable, then schedule another thread */
static void
thread_yield(struct sysregs *r)
{

	if (cur_thread == 0)
		return;
	if (cur_thread->t_tid == cur->uk_pid) {
		cur->uk_regs = *r;
		cur->uk_state = P_RUN;
	} else {
		cur_thread->t_regs = *r;
		cur_thread->t_state = KT_RUN;
	}
	proc_sched();
}

/* 中文：原生 4.5BSD 系统调用分发器：按 eax 调用号分派到各系统调用实现，结果写回 r->eax */
/* EN: Native 4.5BSD system-call dispatcher: dispatch on the eax call number to the various syscall implementations, writing the result back to r->eax */
int
bsd45_syscall(struct sysregs *r)
{
	unsigned n = r->eax;

	switch (n) {
	case 430:			/* getticks(): PIT ticks since boot */
		r->eax = clock_get_ticks();
		break;
	case 460:			/* aio_submit(fd, op, buf, len, off) */
		r->eax = (unsigned)aio_submit_sys((int)r->ebx, (int)r->ecx,
		    r->edx, r->esi, r->edi);
		break;
	case 461:			/* aio_wait(token) */
		r->eax = (unsigned)aio_wait_sys((int)r->ebx, r);
		break;
	case 462:			/* aio_poll(token) */
		r->eax = (unsigned)aio_poll_sys((int)r->ebx);
		break;
	case 463:			/* uring_setup(entries) -> user VA */
		r->eax = (unsigned)uring_setup_sys(r->ebx);
		break;
	case 464:			/* uring_enter(to_submit, wait) */
		r->eax = (unsigned)uring_enter_sys(r->ebx, (int)r->ecx, r);
		break;
	case 465:			/* uring_teardown() */
		r->eax = (unsigned)uring_teardown_sys();
		break;
	case 431: {			/* getcow(int *v): shared/faults/copies */
		unsigned v[3];
		int rv;

		v[0] = cow_shared;
		v[1] = cow_faults;
		v[2] = cow_copies;
		rv = ucopyout(v, (void *)r->ebx, sizeof(v));
		r->eax = (rv < 0) ? (unsigned)rv : 0;
		break;
	}
	case 4: {			/* write(fd, buf, len) */
		unsigned fd = r->ebx, buf = r->ecx, len = r->edx;
		unsigned i;
		int khandle;

		if (!user_ok(buf, len)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		/* KVFS: /srv/ virtual fd -> forward SRV_WRITE.  Copy user data into
		 * a kernel stack buffer first (the IPC payload must be kernel VA). */
		khandle = kvfs_fd_handle(cur, (int)fd);
		if (khandle >= 0) {
			char kbuf[SRV_DATA_MAX_KERNEL];
			unsigned n = len;

			if (n > SRV_DATA_MAX_KERNEL)
				n = SRV_DATA_MAX_KERNEL;
			if (n > 0 && ucopyin((void *)buf, kbuf, n) < 0) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			if (kvfs_forward(r, SRV_WRITE_KERNEL, (unsigned)khandle,
			    0, n, 0, kbuf, n)) {
				break;		/* blocked; resumed via uk_regs */
			}
			r->eax = (unsigned)-EIO;
			break;
		}
		if (fd >= NFD) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (cur->uk_fds[fd].used && cur->uk_fds[fd].vp != 0) {
			int n2;

			/*
			 * Synchronous VFS write: vfs_write_sync() runs the
			 * backend in this context; async uses vfs_write_async.
			 * 同步 VFS 写：vfs_write_sync() 在本地上下文执行后端；
			 * 异步走 vfs_write_async。
			 */
			n2 = vfs_write_sync(cur->uk_fds[fd].vp,
			    cur->uk_fds[fd].off, (const void *)buf, len);
			if (n2 > 0)
				cur->uk_fds[fd].off += n2;
			r->eax = (unsigned)n2;
			break;
		}
		if (cur->uk_fds[fd].used && cur->uk_fds[fd].type == FD_TMPFILE) {
			struct tmpfs_node *n = &tmpfs[cur->uk_fds[fd].node];
			unsigned off = cur->uk_fds[fd].off, n2 = len;

			if (off >= TMPFS_DATASZ) {
				r->eax = (unsigned)-ENOSPC;
				break;
			}
			if (n2 > TMPFS_DATASZ - off)
				n2 = TMPFS_DATASZ - off;
			bcopy((char *)buf, n->data + off, n2);
			cur->uk_fds[fd].off = off + n2;
			if (cur->uk_fds[fd].off > n->size)
				n->size = cur->uk_fds[fd].off;
			cur->uk_fds[fd].len = n->size;
			r->eax = n2;
			break;
		}
		if (cur->uk_fds[fd].used && cur->uk_fds[fd].type == FD_PIPE) {
			struct pipe *pp = &pipes[cur->uk_fds[fd].pipeidx];
			unsigned space, n, first;

			spin_lock(&pipe_lock);
			if (cur->uk_fds[fd].rw != 2) {
				spin_unlock(&pipe_lock);
				r->eax = (unsigned)-EBADF;
				break;
			}
			if (pp->readers <= 0) {
				spin_unlock(&pipe_lock);
				r->eax = (unsigned)-EPIPE;
				break;
			}
			space = PIPESZ - pp->count;
			if (space == 0) {
				cur->uk_regs = *r;
				kthread_block_tcb(0);
				cur->uk_pipeidx = pp - pipes;
				cur->uk_piperw = 2;
				cur->uk_pipeat = buf;
				cur->uk_pipelen = len;
				pp->wpid = cur->uk_pid;
				spin_unlock(&pipe_lock);
				proc_sched();
				break;		/* resumed by the reader */
			}
			n = len;
			if (n > space)
				n = space;
			first = PIPESZ - pp->wpos;
			if (first > n)
				first = n;
			bcopy((char *)buf, pp->buf + pp->wpos, first);
			if (n > first)
				bcopy((char *)buf + first, pp->buf, n - first);
			pp->wpos = (pp->wpos + n) % PIPESZ;
			pp->count += n;
			pipe_wake_reader_locked(pp, 0);
			spin_unlock(&pipe_lock);
			r->eax = n;
			break;
		}
		if (cur->uk_fds[fd].used &&
		    (cur->uk_fds[fd].type == FD_NULL ||
		     cur->uk_fds[fd].type == FD_ZERO)) {
			r->eax = len;		/* discard */
			break;
		}
		if (!cur->uk_fds[fd].used ||
		    cur->uk_fds[fd].type != FD_CONSOLE) {	/* not writable */
			r->eax = (unsigned)-EBADF;
			break;
		}
		for (i = 0; i < len; i++)
			cnputc(((const char *)buf)[i]);
		r->eax = len;
		break;
	}
	case 3: {			/* read(fd, buf, len) */
		unsigned fd = r->ebx, buf = r->ecx, len = r->edx;
		unsigned i = 0;
		int khandle;

		if (!user_ok_w(buf, len)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		if (len == 0) {
			r->eax = 0;
			break;
		}
		/* KVFS: /srv/ virtual fd -> forward SRV_READ.  The reply callback
		 * copies data into buf and returns the byte count. */
		khandle = kvfs_fd_handle(cur, (int)fd);
		if (khandle >= 0) {
			if (kvfs_forward(r, SRV_READ_KERNEL, (unsigned)khandle,
			    buf, len, len, 0, 0)) {
				break;		/* blocked; resumed via uk_regs */
			}
			r->eax = (unsigned)-EIO;
			break;
		}
		if (fd >= NFD || !cur->uk_fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (cur->uk_fds[fd].type == FD_NULL) {
			r->eax = 0;
			break;
		}
		if (cur->uk_fds[fd].type == FD_ZERO) {
			for (i = 0; i < len; i++)
				((char *)buf)[i] = 0;
			r->eax = len;
			break;
		}
		if (cur->uk_fds[fd].type == FD_PS) {
			/*
			 * S409: this used to be `static int plen = -1` - rendered on
			 * the very first read after boot and then cached for the
			 * lifetime of the kernel, so `ps` kept printing whatever the
			 * first snapshot contained (typically sh + ps, i.e. two
			 * lines) no matter how many processes existed later.  The
			 * snapshot is now taken at the start of each open (offset 0)
			 * and cached only for the rest of that fd's reads, so the
			 * table cannot shift under a partially consumed read.
			 * 旧代码用 `static int plen = -1` 只渲染一次就永久缓存，之后 `ps`
			 * 永远打印开机后第一次的快照（通常只有 sh 和 ps 两行）。现在每次
			 * 从 offset 0 开始读（即每次 open）重新快照，同一次读取序列内仍复
			 * 用快照，避免进程表在分片读取途中变化导致输出错位。
			 *
			 * 8 KB, not 512: one line is ~22 bytes and NPROC is 256, so a
			 * full table is ~5.6 KB and the old buffer truncated `ps` to
			 * roughly twenty rows without saying so.
			 * 缓冲从 512 字节扩到 8KB：单行约 22 字节，256 个槽全满约 5.6KB，
			 * 旧缓冲会把 `ps` 静默截断到二十行左右。
			 *
			 * 该缓冲是文件作用域静态量（ps_snap_buf）：两个进程同时读
			 * /dev/ps 会共用它，单控制台环境下可接受，注释在此说明。
			 */
			unsigned off = cur->uk_fds[fd].off, n2;

			if (off == 0)
				ps_snap_len = proc_list_text(ps_snap_buf,
				    sizeof(ps_snap_buf));
			if (off >= (unsigned)ps_snap_len) {
				r->eax = 0;
				break;
			}
			n2 = ps_snap_len - off;
			if (n2 > len)
				n2 = len;
			bcopy(ps_snap_buf + off, (char *)buf, n2);
			cur->uk_fds[fd].off = off + n2;
			r->eax = n2;
			break;
		}
		if (cur->uk_fds[fd].type == FD_MOUNTS) {
			static char mbuf[256];
			static int mlen;
			unsigned off = cur->uk_fds[fd].off, n2;

			/*
			 * Same S409 fix as /dev/ps: the old `static int mlen = -1`
			 * cached the first mount table for the whole boot, so `df`
			 * after a mount(8) still showed the old list.  Re-render at
			 * the start of each read sequence.
			 * 与 /dev/ps 同样的 S409 修复：旧代码 `static int mlen = -1`
			 * 把首次挂载表缓存到关机，mount(8) 之后 df 仍显示旧列表。
			 */
			if (off == 0)
				mlen = vfs_mount_list(mbuf, sizeof(mbuf));
			if (off >= (unsigned)mlen) {
				r->eax = 0;
				break;
			}
			n2 = mlen - off;
			if (n2 > len)
				n2 = len;
			bcopy(mbuf + off, (char *)buf, n2);
			cur->uk_fds[fd].off = off + n2;
			r->eax = n2;
			break;
		}
		if (cur->uk_fds[fd].type == FD_VMSTAT) {
			/*
			 * S17: the counters are live, so re-render on every
			 * read instead of caching a snapshot like /dev/ps.
			 */
			static char vbuf[512];
			int vlen = vfs_cache_report(vbuf, sizeof(vbuf));
			unsigned off = cur->uk_fds[fd].off, n2;

			if (off >= (unsigned)vlen) {
				r->eax = 0;
				break;
			}
			n2 = vlen - off;
			if (n2 > len)
				n2 = len;
			bcopy(vbuf + off, (char *)buf, n2);
			cur->uk_fds[fd].off = off + n2;
			r->eax = n2;
			break;
		}
		if (cur->uk_fds[fd].type == FD_CONSOLE) {
			r->eax = (unsigned)console_read((char *)buf, len);
			break;
		}
		if (cur->uk_fds[fd].type == FD_DIR) {
			r->eax = (unsigned)-EISDIR;
			break;
		}
		if (cur->uk_fds[fd].vp != 0) {
			struct vnode *vp2;
			unsigned off2;
			int n2;

			/*
			 * §7.2 first BKL-free path: read(2) on a regular file.
			 * Inside fd_lock we validate the descriptor, pin the
			 * vnode (fd_lock -> vfs_lock) and snapshot the offset;
			 * the page-cache read then runs with the BKL released.
			 * Everything it touches is already locked: page cache
			 * (pc_lock), vnode pool and name cache (vfs_lock ->
			 * nc_lock).  The offset is committed back only if the
			 * descriptor still names the same vnode.
			 */
			/* 中文：§7.2 第一条摘掉 BKL 的路径：普通文件 read(2)。锁内校验 fd、固定 vnode（fd_lock -> vfs_lock）并取偏移快照，页缓存读在放掉 BKL 后执行；它触及的结构都已加锁（pc_lock、vfs_lock -> nc_lock）。只有在 fd 仍指向同一 vnode 时才回写偏移。 */
			spin_lock(&fd_lock);
			vp2 = cur->uk_fds[fd].vp;
			if (vp2 == 0) {
				spin_unlock(&fd_lock);
				r->eax = (unsigned)-EBADF;
				break;
			}
			if (vp2->type == VDIR) {
				spin_unlock(&fd_lock);
				r->eax = (unsigned)-EISDIR;
				break;
			}
			off2 = cur->uk_fds[fd].off;
			vn_ref(vp2);
			spin_unlock(&fd_lock);

			smp_bkl_drop();
			n2 = vfs_read_sync(vp2, off2, (void *)buf, len);
			smp_bkl_enter();

			spin_lock(&fd_lock);
			if (n2 > 0 && cur->uk_fds[fd].vp == vp2)
				cur->uk_fds[fd].off = off2 + (unsigned)n2;
			spin_unlock(&fd_lock);
			vn_put(vp2);
			r->eax = (unsigned)n2;
			break;
		}
		if (cur->uk_fds[fd].type == FD_TMPFILE) {
			struct tmpfs_node *n = &tmpfs[cur->uk_fds[fd].node];
			unsigned off = cur->uk_fds[fd].off, n2 = 0;

			if (cur->uk_fds[fd].rw != 1 && cur->uk_fds[fd].rw != 2) {
				r->eax = (unsigned)-EBADF;
				break;
			}
			if (off < n->size) {
				n2 = n->size - off;
				if (n2 > len)
					n2 = len;
				bcopy(n->data + off, (char *)buf, n2);
				cur->uk_fds[fd].off = off + n2;
			}
			r->eax = n2;
			break;
		}
		if (cur->uk_fds[fd].type == FD_TMPDIR) {
			r->eax = (unsigned)-EISDIR;
			break;
		}
		if (cur->uk_fds[fd].type == FD_PIPE) {
			struct pipe *pp = &pipes[cur->uk_fds[fd].pipeidx];
			unsigned n, first;

			spin_lock(&pipe_lock);
			if (cur->uk_fds[fd].rw != 1) {
				spin_unlock(&pipe_lock);
				r->eax = (unsigned)-EBADF;
				break;
			}
			if (pp->count == 0) {
				if (pp->writers <= 0) {		/* EOF */
					spin_unlock(&pipe_lock);
					r->eax = 0;
					break;
				}
				cur->uk_regs = *r;
				kthread_block_tcb(0);
				cur->uk_pipeidx = pp - pipes;
				cur->uk_piperw = 1;
				cur->uk_pipeat = buf;
				cur->uk_pipelen = len;
				pp->rpid = cur->uk_pid;
				spin_unlock(&pipe_lock);
				proc_sched();
				break;		/* resumed by the writer */
			}
			n = pp->count;
			if (n > len)
				n = len;
			first = PIPESZ - pp->rpos;
			if (first > n)
				first = n;
			bcopy(pp->buf + pp->rpos, (char *)buf, first);
			if (n > first)
				bcopy(pp->buf, (char *)buf + first, n - first);
			pp->rpos = (pp->rpos + n) % PIPESZ;
			pp->count -= n;
			pipe_wake_writer_locked(pp, 0);
			spin_unlock(&pipe_lock);
			r->eax = n;
			break;
		}
		while (i < len && cur->uk_fds[fd].off < cur->uk_fds[fd].len)
			((char *)buf)[i++] = cur->uk_fds[fd].d[cur->uk_fds[fd].off++];
		r->eax = i;
		break;
	}
	case 5: {			/* open(path, flags, mode) */
		char path[128];
		struct vnode *vp = 0;
		int fd, flags = (int)r->ecx, rw, rv;
		unsigned mode = 0, size = 0;

		if (path_arg(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		/* KVFS: /srv/ prefix -> forward SRV_OPEN to user-space fs_server.
		 * The reply callback registers a virtual fd as the return value.
		 * KVFS: /srv/ 前缀 -> 把 SRV_OPEN 转发给用户态 fs_server；回复回调
		 * 登记虚拟 fd 作为返回值。 */
		if (strncmp(path, "/srv/", 5) == 0) {
			const char *name = path + 5;
			unsigned nlen = strlen(name) + 1;

			if (kvfs_forward(r, SRV_OPEN_KERNEL, 0, 0, 0, 0,
			    name, nlen)) {
				break;		/* blocked; resumed via uk_regs */
			}
			r->eax = (unsigned)-EIO;	/* 服务端口无属主 / 已有在途事务 */
			break;
		}
		/* S11: the three character devices live outside the VFS */
		if (strcmp(path, "/dev/null") == 0 ||
		    strcmp(path, "/dev/zero") == 0 ||
		    strcmp(path, "/dev/tty") == 0 ||
		    strcmp(path, "/dev/mounts") == 0 ||
		    strcmp(path, "/dev/ps") == 0 ||
		    strcmp(path, "/dev/vmstat") == 0) {
			int t;

			t = (strcmp(path, "/dev/null") == 0) ? FD_NULL :
			    (strcmp(path, "/dev/zero") == 0) ? FD_ZERO :
			    (strcmp(path, "/dev/mounts") == 0) ? FD_MOUNTS :
			    (strcmp(path, "/dev/ps") == 0) ? FD_PS :
			    (strcmp(path, "/dev/vmstat") == 0) ? FD_VMSTAT :
			    FD_CONSOLE;
			for (fd = 0; fd < NFD; fd++)
				if (!cur->uk_fds[fd].used)
					break;
			if (fd >= NFD) {
				r->eax = (unsigned)-EMFILE;
				break;
			}
			bzero(&cur->uk_fds[fd], sizeof(cur->uk_fds[fd]));
			cur->uk_fds[fd].used = 1;
			cur->uk_fds[fd].type = t;
			cur->uk_fds[fd].rw = ((flags & O_ACCMODE) == O_RDONLY) ? 1 : 2;
			cur->uk_fds[fd].pipeidx = -1;
			cur->uk_fds[fd].node = -1;
			cur->uk_fds[fd].vp = 0;
			r->eax = (unsigned)fd;
			break;
		}
		rv = vfs_namei(path, &vp);
		if (rv < 0) {
			if (!(flags & O_CREAT)) {
				r->eax = (unsigned)-ENOENT;
				break;
			}
			rv = vfs_create(path, 0, &vp);
			if (rv < 0) {
				r->eax = (unsigned)rv;
				break;
			}
		}
		rw = ((flags & O_ACCMODE) == O_RDONLY) ? 1 : 2;
		if (flags & O_TRUNC) {
			if (vp->ops->truncate == 0) {
				vn_put(vp);
				r->eax = (unsigned)-EROFS;
				break;
			}
			vp->ops->truncate(vp);
		}
		if (flags & O_APPEND) {
			vp->ops->getattr(vp, &mode, &size);
		}
		fd = fd_alloc_vnode(vp, rw, (flags & O_APPEND) ? size : 0);
		if (fd < 0) {
			vn_put(vp);
			r->eax = (unsigned)-EMFILE;
			break;
		}
		r->eax = (unsigned)fd;
		break;
	}
	case 6: {			/* close(fd) */
		int khandle = kvfs_fd_handle(cur, (int)r->ebx);

		if (khandle >= 0) {
			/* KVFS: /srv/ virtual fd -> forward SRV_CLOSE to fs_server.
			 * Full transparent forwarding like open/read/write: the reply
			 * callback (kvfs_reply_deliver) releases the virtual fd slot and
			 * writes eax=0, then wakes us.  Service absent -> -EIO.
			 * KVFS: /srv/ 虚拟 fd -> 把 SRV_CLOSE 完整转发给 fs_server，
			 * 与 open/read/write 一样走阻塞转发；回复回调释放虚拟 fd 槽位、
			 * 写 eax=0 并唤醒。服务不在 -> -EIO。 */
			if (kvfs_forward(r, SRV_CLOSE_KERNEL, (unsigned)khandle,
			    0, 0, 0, 0, 0)) {
				break;		/* blocked; resumed via uk_regs */
			}
			r->eax = (unsigned)-EIO;	/* 服务端口无属主 / 已有在途事务 */
			break;
		}
		fd_close((int)r->ebx);
		r->eax = 0;
		break;
	}
	case 59: {			/* execve(path, argv, envp) */
		char path[128];

		if (path_arg(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		proc_setcomm(cur, path);
		n = (unsigned)exec_image(path, r->ecx, r->edx, r);
		if ((int)n < 0)
			r->eax = n;
		break;
	}
	case 1:				/* exit(code) */
		proc_exit((int)r->ebx);
		break;
	case 17: {			/* break(addr) */
		unsigned a = r->ebx;

		if (a < USER_LOAD || a > user_heap_max) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		/*
		 * Demand-paged heap: only record the new limit.  The first
		 * touch below brk_mapped takes a page fault and gets a zero
		 * page from demand_page(); no page is allocated here.
		 * 按需分页堆：这里只记录新的堆上限；首次访问 brk_mapped
		 * 以下的地址时由 demand_page() 分配零页，本调用不分配物理页。
		 */
		if (a > cur->uk_brk_mapped)
			cur->uk_brk_mapped = a;
		r->eax = a;
		break;
	}
	case 20:			/* getpid */
		r->eax = cur->uk_pid;
		break;
	case 39:			/* getppid */
		r->eax = cur->uk_ppid;
		break;
	case 24: case 25: case 43: case 47:	/* uid/gid */
		r->eax = 0;
		break;
	case 81:			/* getpgrp */
		r->eax = (unsigned)cur->p_pgrp;
		break;
	case 82: {			/* setpgid(pid, pgid) */
		struct process *p = (r->ebx == 0) ? cur :
		    proc_find((int)r->ebx);

		if (p == 0) {
			r->eax = (unsigned)-ESRCH;
			break;
		}
		p->p_pgrp = (r->ecx == 0) ? p->uk_pid : (int)r->ecx;
		r->eax = 0;
		break;
	}
	case 33: {			/* access(path, mode) */
		char path[128];
		struct vnode *vp = 0;

		if (path_arg(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		r->eax = (vfs_namei(path, &vp) == 0) ? 0 : (unsigned)-ENOENT;
		if (vp)
			vn_put(vp);
		break;
	}
	case 37: {			/* kill(pid, sig) */
		int pid = (int)r->ebx, sig = (int)r->ecx, i, found = 0;

		if (sig < 0 || sig >= NSIG) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		for (i = 0; i < NPROC; i++) {
			struct process *p = &procs[i];
			int hit;

			if (p->uk_state == P_FREE)
				continue;
			if (pid > 0)
				hit = (p->uk_pid == pid);
			else if (pid == 0)
				hit = (p->p_pgrp == cur->p_pgrp);
			else if (pid == -1)
				hit = (p->uk_pid != 1);
			else
				hit = (p->p_pgrp == -pid);
			if (hit) {
				sig_send(p, sig);
				found = 1;
			}
		}
		r->eax = found ? 0 : (unsigned)-ESRCH;
		break;
	}
	case 46: {			/* sigaction(sig, nsa, osa) */
		struct {
			unsigned handler, mask, flags;
		} sa, old;
		int sig = (int)r->ebx;
		unsigned nsa = r->ecx, osa = r->edx;

		if (sig <= 0 || sig >= NSIG) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		if (osa != 0) {
			if (!user_ok(osa, sizeof(sa))) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			old.handler = cur->uk_p_sighand[sig];
			old.mask = cur->uk_p_sigmask;
			old.flags = 0;
			bcopy(&old, (void *)osa, sizeof(sa));
		}
		if (nsa == 0) {
			r->eax = 0;
			break;
		}
		if (!user_ok(nsa, sizeof(sa))) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		bcopy((void *)nsa, &sa, sizeof(sa));
		cur->uk_p_sighand[sig] = sa.handler;
		cur->uk_p_sigcatch &= ~SIG_TO_MASK(sig);
		cur->uk_p_sigignore &= ~SIG_TO_MASK(sig);
		if (sa.handler == SIG_IGN)
			cur->uk_p_sigignore |= SIG_TO_MASK(sig);
		else if (sa.handler != SIG_DFL)
			cur->uk_p_sigcatch |= SIG_TO_MASK(sig);
		r->eax = 0;
		break;
	}
	case 48: {			/* sigprocmask(how, set, oset) */
		unsigned how = r->ebx, set = r->ecx, oset = r->edx;
		unsigned m = 0, old;
		unsigned *maskp = &cur->uk_p_sigmask;

		if (cur_thread != 0 && cur_thread->t_proc == cur)
			maskp = &cur_thread->t_sigmask;
		old = *maskp;
		if (set != 0) {
			if (!user_ok(set, 4)) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			m = *(unsigned *)set;
		}
		if (oset != 0) {
			if (!user_ok(oset, 4)) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			*(unsigned *)oset = old;
		}
		if (set != 0) {
			if (how == 1)
				*maskp |= m;
			else if (how == 2)
				*maskp &= ~m;
			else if (how == 3)
				*maskp = m &
				    ~(SIG_TO_MASK(SIGKILL) |
				      SIG_TO_MASK(SIGSTOP));
			else {
				r->eax = (unsigned)-EINVAL;
				break;
			}
			if (cur_thread == cur->uk_p_thread)
				cur->uk_p_sigmask = cur_thread->t_sigmask;
		}
		r->eax = 0;
		break;
	}
	case 103: {			/* sigreturn: restore the saved context */
		struct sigframe *fp = 0;
		unsigned a, i;

		a = (r->uesp + 4 + SIGTRAMP) & ~3u;
		if (user_ok(a, SIGFRAME_SIZE) &&
		    ((struct sigframe *)a)->magic == SIGFRAME_MAGIC)
			fp = (struct sigframe *)a;
		else
			for (i = r->uesp & ~3u; i < r->uesp + 320; i += 4)
				if (user_ok(i, SIGFRAME_SIZE) &&
				    ((struct sigframe *)i)->magic ==
				    SIGFRAME_MAGIC) {
					fp = (struct sigframe *)i;
					break;
				}
		if (fp == 0) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		r->eax = fp->eax; r->ebx = fp->ebx; r->ecx = fp->ecx;
		r->edx = fp->edx; r->esi = fp->esi; r->edi = fp->edi;
		r->ebp = fp->ebp; r->ds = fp->ds; r->es = fp->es;
		r->eip = fp->eip; r->cs = fp->cs; r->eflags = fp->eflags;
		r->uesp = fp->uesp; r->ss = fp->ss;
		cur->uk_p_sigmask = fp->mask;
		break;
	}
	case 52:			/* sigpending */
		r->eax = 0;
		break;
	case 54: {			/* ioctl(fd, request, arg) */
		unsigned fd = r->ebx;
		unsigned long req = r->ecx;
		unsigned arg = r->edx;

		if (fd > 2) {
			r->eax = (unsigned)-ENOTTY;
			break;
		}
		switch (req) {
		case TIOCGETA:
			r->eax = (unsigned)console_termios(arg, 1);
			break;
		case TIOCSETA:
		case TIOCSETAW:
		case TIOCSETAF:
			r->eax = (unsigned)console_termios(arg, 0);
			break;
		case TIOCGWINSZ: {
			struct winsize *w = (struct winsize *)arg;

			if (!user_ok(arg, sizeof(*w))) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			w->ws_row = 25;
			w->ws_col = 80;
			w->ws_xpixel = 0;
			w->ws_ypixel = 0;
			r->eax = 0;
			break;
		}
		case TIOCGPGRP:
			if (!user_ok(arg, 4)) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			*(int *)arg = console_fg_pgrp;
			r->eax = 0;
			break;
		case TIOCSPGRP:
			if (!user_ok(arg, 4)) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			console_fg_pgrp = *(int *)arg;
			r->eax = 0;
			break;
		default:
			r->eax = (unsigned)-ENOTTY;
			break;
		}
		break;
	}
	case 60: {			/* umask(mask) */
		unsigned old = umask_val;

		umask_val = r->ebx;
		r->eax = old;
		break;
	}
	case 189: {			/* stat(path, sb) */
		char path[128];
		struct vnode *vp = 0;
		unsigned mode = 0, size = 0;

		if (path_arg(r->ebx, path, sizeof(path)) < 0 ||
		    !user_ok(r->ecx, STAT_SIZE)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		if (vfs_namei(path, &vp) < 0) {
			r->eax = (unsigned)-ENOENT;
			break;
		}
		vp->ops->getattr(vp, &mode, &size);
		fill_stat(mode, size, (unsigned *)r->ecx);
		vn_put(vp);
		r->eax = 0;
		break;
	}
	case 188: {			/* fstat(fd, sb) */
		if (!user_ok(r->ecx, STAT_SIZE)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		if (r->ebx < 3) {
			fill_stat(0020620, 0, (unsigned *)r->ecx);
			r->eax = 0;
			break;
		}
		if (r->ebx >= NFD || !cur->uk_fds[r->ebx].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (cur->uk_fds[r->ebx].vp != 0) {
			unsigned mode = 0, size = 0;

			cur->uk_fds[r->ebx].vp->ops->getattr(
			    cur->uk_fds[r->ebx].vp, &mode, &size);
			fill_stat(mode, size, (unsigned *)r->ecx);
			r->eax = 0;
			break;
		}
		if (cur->uk_fds[r->ebx].type == FD_TMPFILE) {
			fill_stat(0100644,
			    tmpfs[cur->uk_fds[r->ebx].node].size,
			    (unsigned *)r->ecx);
			r->eax = 0;
			break;
		}
		if (cur->uk_fds[r->ebx].type == FD_TMPDIR) {
			fill_stat(0040755, 4096, (unsigned *)r->ecx);
			r->eax = 0;
			break;
		}
		fill_stat(cur->uk_fds[r->ebx].type == FD_DIR ? 0040755 : 0100755,
		    cur->uk_fds[r->ebx].len, (unsigned *)r->ecx);
		r->eax = 0;
		break;
	}
	case 196: {			/* getdirentries(fd, buf, n, basep) */
		unsigned fd = r->ebx, buf = r->ecx, nb = r->edx, basep = r->esi;
		struct kfd *fp;
		char name[48];
		unsigned elba, esize, pos;
		int eflags, rv, namlen, reclen;

		if (fd >= NFD || !cur->uk_fds[fd].used ||
		    (cur->uk_fds[fd].type != FD_DIR &&
		     cur->uk_fds[fd].type != FD_TMPDIR)) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (!user_ok(buf, nb) || nb < 16) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		fp = &cur->uk_fds[fd];
		if (fp->vp != 0) {
			int rv2, namlen2, reclen2, isd2 = 0;

			rv2 = fp->vp->ops->readdir(fp->vp, &fp->off, name,
			    &isd2);
			if (rv2 < 0) {
				r->eax = (unsigned)-EIO;
				break;
			}
			if (basep != 0 && user_ok(basep, 4))
				*(unsigned *)basep = 0;
			if (rv2 > 0) {
				r->eax = 0;
				break;
			}
			namlen2 = strlen(name);
			reclen2 = (8 + namlen2 + 1 + 3) & ~3;
			if ((unsigned)reclen2 > nb) {
				r->eax = (unsigned)-EINVAL;
				break;
			}
			bzero((char *)buf, reclen2);
			*(unsigned *)(buf + 0) = fp->off;
			*(unsigned short *)(buf + 4) = reclen2;
			((char *)buf)[6] = isd2 ? 4 : 8;
			((char *)buf)[7] = namlen2;
			bcopy(name, (char *)buf + 8, namlen2 + 1);
			r->eax = reclen2;
			break;
		}
		if (fp->type == FD_TMPDIR) {
			int i;

			for (i = fp->dpos; i < NTMPFS && !tmpfs[i].used; i++)
				;
			if (basep != 0 && user_ok(basep, 4))
				*(unsigned *)basep = 0;
			if (i >= NTMPFS) {
				fp->dpos = NTMPFS;
				r->eax = 0;
				break;
			}
			fp->dpos = i + 1;
			namlen = strlen(tmpfs[i].name);
			reclen = (8 + namlen + 1 + 3) & ~3;
			if ((unsigned)reclen > nb) {
				r->eax = (unsigned)-EINVAL;
				break;
			}
			bzero((char *)buf, reclen);
			*(unsigned *)(buf + 0) = i + 1;
			*(unsigned short *)(buf + 4) = reclen;
			((char *)buf)[6] = tmpfs[i].isdir ? 4 : 8;
			((char *)buf)[7] = namlen;
			bcopy(tmpfs[i].name, (char *)buf + 8, namlen + 1);
			r->eax = reclen;
			break;
		}
		pos = fp->dpos;
		rv = iso_readdir(fp->lba, fp->dsize, &pos, name, &elba,
		    &esize, &eflags);
		if (rv < 0) {
			r->eax = (unsigned)-EIO;
			break;
		}
		if (basep != 0 && user_ok(basep, 4))
			*(unsigned *)basep = 0;
		if (rv > 0) {		/* end of directory */
			fp->dpos = pos;
			r->eax = 0;
			break;
		}
		namlen = strlen(name);
		reclen = (8 + namlen + 1 + 3) & ~3;
		if ((unsigned)reclen > nb) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		bzero((char *)buf, reclen);
		*(unsigned *)(buf + 0) = elba;		/* d_fileno */
		*(unsigned short *)(buf + 4) = reclen;	/* d_reclen */
		((char *)buf)[6] = (eflags & 2) ? 4 : 8;	/* d_type */
		((char *)buf)[7] = namlen;		/* d_namlen */
		bcopy(name, (char *)buf + 8, namlen + 1);
		fp->dpos = pos;
		r->eax = reclen;
		break;
	}
	case 199: {			/* lseek(fd, off, whence) */
		unsigned fd = r->ebx, off = r->ecx, wh = r->edx;

		if (fd >= NFD || !cur->uk_fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (cur->uk_fds[fd].vp != 0) {
			unsigned mode = 0, size = 0;

			cur->uk_fds[fd].vp->ops->getattr(cur->uk_fds[fd].vp,
			    &mode, &size);
			if (wh == 0)
				cur->uk_fds[fd].off = off;
			else if (wh == 1)
				cur->uk_fds[fd].off += off;
			else
				cur->uk_fds[fd].off = size;
			r->eax = cur->uk_fds[fd].off;
			break;
		}
		if (cur->uk_fds[fd].type == FD_TMPFILE) {
			if (wh == 0)
				cur->uk_fds[fd].off = off;
			else if (wh == 1)
				cur->uk_fds[fd].off += off;
			else
				cur->uk_fds[fd].off = tmpfs[cur->uk_fds[fd].node].size;
			r->eax = cur->uk_fds[fd].off;
			break;
		}
		if (cur->uk_fds[fd].type == FD_TMPDIR) {
			cur->uk_fds[fd].dpos = (wh == 1) ? cur->uk_fds[fd].dpos + off
			    : 0;
			r->eax = cur->uk_fds[fd].dpos;
			break;
		}
		if (cur->uk_fds[fd].type == FD_DIR) {
			if (wh == 0)
				cur->uk_fds[fd].dpos = off;
			else if (wh == 1)
				cur->uk_fds[fd].dpos += off;
			else
				cur->uk_fds[fd].dpos = cur->uk_fds[fd].dsize;
			r->eax = cur->uk_fds[fd].dpos;
			break;
		}
		if (wh == 0)
			cur->uk_fds[fd].off = off;
		else if (wh == 1)
			cur->uk_fds[fd].off += off;
		else
			cur->uk_fds[fd].off = cur->uk_fds[fd].len;
		r->eax = cur->uk_fds[fd].off;
		break;
	}
	case 89:			/* getdtablesize */
		r->eax = NFD;
		break;
	case 197: {			/* mmap(addr, len, prot, flags, fd) */
		int rv = do_mmap(r->ebx, r->ecx, (int)r->edx, (int)r->esi,
		    (int)r->edi);

		r->eax = (unsigned)rv;
		break;
	}
	case 73:			/* munmap(addr, len) */
		r->eax = (unsigned)do_munmap(r->ebx, r->ecx);
		break;
	case 74:			/* mprotect(addr, len, prot) */
		r->eax = 0;		/* pages are mapped eagerly with prot */
		break;
	case 65:			/* msync(addr, len) */
		r->eax = (unsigned)do_msync(r->ebx, r->ecx);
		break;
	case 7:				/* wait4(pid, status, options, rusage) */
		do_wait4((int)r->ebx, r->ecx, (int)r->edx, r);
		break;
	case 2:				/* fork */
		r->eax = (unsigned)do_fork(r);
		break;
	case 66:			/* vfork */
		r->eax = (unsigned)do_vfork(r);
		break;
	case 92: {			/* fcntl(fd, cmd, arg) */
		int fd = (int)r->ebx, cmd = (int)r->ecx, arg = (int)r->edx;

		if (fd < 0 || fd >= NFD || !cur->uk_fds[fd].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (cmd == 0) {		/* F_DUPFD: lowest free fd >= arg */
			int newfd;

			for (newfd = arg; newfd < NFD; newfd++)
				if (!cur->uk_fds[newfd].used)
					break;
			if (newfd >= NFD) {
				r->eax = (unsigned)-EMFILE;
				break;
			}
			cur->uk_fds[newfd] = cur->uk_fds[fd];
			fd_ref(&cur->uk_fds[newfd]);
			if (cur->uk_fds[newfd].type == FD_PIPE &&
			    cur->uk_fds[newfd].pipeidx >= 0)
				pipe_addref(&pipes[cur->uk_fds[newfd].pipeidx],
				    cur->uk_fds[newfd].rw);
			r->eax = newfd;
			break;
		}
		if (cmd >= 1 && cmd <= 4) {	/* F_GETFD/F_SETFD/F_GETFL/F_SETFL */
			r->eax = 0;
			break;
		}
		r->eax = (unsigned)-EINVAL;
		break;
	}
	case 41: {			/* dup(fd) */
		int from = (int)r->ebx, to;

		if (from < 0 || from >= NFD || !cur->uk_fds[from].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		for (to = 3; to < NFD; to++)
			if (!cur->uk_fds[to].used)
				break;
		if (to >= NFD) {
			r->eax = (unsigned)-EMFILE;
			break;
		}
		cur->uk_fds[to] = cur->uk_fds[from];
		fd_ref(&cur->uk_fds[to]);
		if (cur->uk_fds[to].type == FD_PIPE && cur->uk_fds[to].pipeidx >= 0)
			pipe_addref(&pipes[cur->uk_fds[to].pipeidx],
			    cur->uk_fds[to].rw);
		r->eax = to;
		break;
	}
	case 42: {			/* pipe(fds) */
		struct pipe *pp;
		unsigned fds[2];
		int i, nfd = 0;

		if (!user_ok(r->ebx, 2 * sizeof(int))) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		pp = pipe_alloc();
		if (pp == 0) {
			r->eax = (unsigned)-EMFILE;
			break;
		}
		spin_lock(&pipe_lock);
		pp->readers = 1;
		pp->writers = 1;
		spin_unlock(&pipe_lock);
		for (i = 3; i < NFD && nfd < 2; i++) {
			if (!cur->uk_fds[i].used) {
				fds[nfd++] = i;
				bzero(&cur->uk_fds[i], sizeof(cur->uk_fds[i]));
				cur->uk_fds[i].used = 1;
				cur->uk_fds[i].type = FD_PIPE;
				cur->uk_fds[i].pipeidx = pp - pipes;
				cur->uk_fds[i].d = 0;
				cur->uk_fds[i].len = 0;
				cur->uk_fds[i].off = 0;
				cur->uk_fds[i].lba = 0;
				cur->uk_fds[i].dsize = 0;
				cur->uk_fds[i].dpos = 0;
				cur->uk_fds[i].rw = (nfd == 1) ? 1 : 2;
			}
		}
		if (nfd < 2) {
			spin_lock(&pipe_lock);
			pp->used = 0;
			spin_unlock(&pipe_lock);
			r->eax = (unsigned)-EMFILE;
			break;
		}
		((int *)r->ebx)[0] = fds[0];
		((int *)r->ebx)[1] = fds[1];
		r->eax = 0;
		break;
	}
	case 90: {			/* dup2(from, to) */
		int from = (int)r->ebx, to = (int)r->ecx;

		if (from < 0 || from >= NFD || to < 0 || to >= NFD ||
		    !cur->uk_fds[from].used) {
			r->eax = (unsigned)-EBADF;
			break;
		}
		if (from == to) {
			r->eax = to;
			break;
		}
		fd_close(to);
		cur->uk_fds[to] = cur->uk_fds[from];
		fd_ref(&cur->uk_fds[to]);
		if (cur->uk_fds[to].type == FD_PIPE && cur->uk_fds[to].pipeidx >= 0)
			pipe_addref(&pipes[cur->uk_fds[to].pipeidx],
			    cur->uk_fds[to].rw);
		r->eax = to;
		break;
	}
	case 10: {			/* unlink(path) */
		char path[128];

		if (path_arg(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		r->eax = (unsigned)vfs_remove(path, 0);
		break;
	}
	case 128: {			/* rename(old, new) */
		char oldp[128], newp[128];

		if (path_arg(r->ebx, oldp, sizeof(oldp)) < 0 ||
		    path_arg(r->ecx, newp, sizeof(newp)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		r->eax = (unsigned)vfs_rename(oldp, newp);
		break;
	}
	case 9:				/* link: no hard links yet */
		r->eax = (unsigned)-EROFS;
		break;
	case 15:			/* chmod: modes are fixed for now */
		r->eax = 0;
		break;
	case 136: {			/* mkdir(path, mode) */
		char path[128];
		struct vnode *vp = 0;
		int rv;

		if (path_arg(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		rv = vfs_create(path, 1, &vp);
		if (vp)
			vn_put(vp);
		r->eax = (unsigned)rv;
		break;
	}
	case 137: {			/* rmdir(path) */
		char path[128];

		if (path_arg(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		r->eax = (unsigned)vfs_remove(path, 1);
		break;
	}
	case 12: {			/* chdir(path) */
		char path[128];
		struct vnode *vp = 0;
		int rv;

		if (path_arg(r->ebx, path, sizeof(path)) < 0) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		rv = vfs_namei(path, &vp);
		if (rv < 0) {
			r->eax = (unsigned)rv;
			break;
		}
		if (vp->type != VDIR) {
			vn_put(vp);
			r->eax = (unsigned)-ENOTDIR;
			break;
		}
		vn_put(vp);
		bcopy(path, cur->uk_p_cwd, sizeof(cur->uk_p_cwd));
		r->eax = 0;
		break;
	}
	case 326: {			/* getcwd(buf, len) */
		unsigned buf = r->ebx, len = r->ecx;
		unsigned n = strlen(cur->uk_p_cwd) + 1;

		if (len < n) {
			r->eax = (unsigned)-ERANGE;
			break;
		}
		if (!user_ok(buf, n)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		bcopy(cur->uk_p_cwd, (void *)buf, n);
		r->eax = buf;
		break;
	}
	case 116: {			/* gettimeofday(timeval *, timezone *) */
		unsigned tvp = r->ebx, tzp = r->ecx;
		struct timeval tv;

		if (tvp != 0 && !user_ok(tvp, sizeof(tv))) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		if (tzp != 0 && !user_ok(tzp, 8)) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		tv.tv_sec = rtc_epoch();
		tv.tv_usec = 0;
		if (tvp != 0)
			bcopy(&tv, (void *)tvp, sizeof(tv));
		if (tzp != 0)
			bzero((void *)tzp, 8);
		r->eax = 0;
		break;
	}
	case 164: {			/* uname(struct utsname *) */
		unsigned buf = r->ebx;
		struct utsname *up = (struct utsname *)buf;

		if (!user_ok(buf, sizeof(*up))) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		bzero(up, sizeof(*up));
		bcopy("BSD45", up->sysname, sizeof("BSD45"));
		bcopy("bsd45", up->nodename, sizeof("bsd45"));
		bcopy("1.0", up->release, sizeof("1.0"));
		bcopy("4.4BSD-Lite2 i386 ELF kernel", up->version,
		    sizeof("4.4BSD-Lite2 i386 ELF kernel"));
		bcopy("i386", up->machine, sizeof("i386"));
		r->eax = 0;
		break;
	}
	case 440:			/* thread_create(entry,arg,stack) */
		r->eax = (unsigned)thread_create(r);
		break;
	case 441:			/* thread_exit(code) */
		thread_exit_current((int)r->ebx);
		r->eax = 0;
		break;
	case 442:			/* thread_join(tid) */
		r->eax = (unsigned)thread_join((int)r->ebx, r);
		break;
	case 443:			/* thread_self() */
		r->eax = (unsigned)thread_self();
		break;
	case 444:			/* thread_yield() */
		thread_yield(r);
		r->eax = 0;
		break;
	case 445:			/* thread_detach(tid) */
		r->eax = (unsigned)thread_detach((int)r->ebx);
		break;
	case 446:			/* sched_setprio(prio): 0 lowest .. 19 highest */
		if (cur_thread != 0 && (int)r->ebx >= 0 && (int)r->ebx < NPRIO) {
			cur_thread->t_prio = (int)r->ebx;
			cur_thread->t_base_prio = (int)r->ebx;
			cur_thread->t_curprio = sched_effective_prio(cur_thread);
			r->eax = 0;
			need_resched = 1;	/* re-evaluate after priority change */
		} else
			r->eax = (unsigned)-EINVAL;
		break;
	case 447:			/* sched_getprio() */
		r->eax = (unsigned)(cur_thread != 0 ?
		    sched_effective_prio(cur_thread) : PRI_NORMAL);
		break;
	case 451: {			/* tgkill(tid, sig) - thread-directed */
		int tid = (int)r->ebx, sig = (int)r->ecx;
		struct kthread *t;

		if (sig <= 0 || sig >= NSIG) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		t = kthread_user_find(tid);
		if (t == 0) {
			r->eax = (unsigned)-ESRCH;
			break;
		}
		t->t_sig |= SIG_TO_MASK(sig);
		if (t->t_state == KT_BLOCK)
			sched_runnable(t, t->t_oncpu >= 0 ? t->t_oncpu :
			    cpu_id());
		proc_wake(t->t_proc);
		r->eax = 0;
		break;
	}
	case 450: {			/* access_w(const u16 *path) - W path */
#if KSTR_UTF16
		unsigned short ubuf[128], kbuf[128];
		struct kstr s;
		struct vnode *vp = 0;
		unsigned i, n = 0;

		for (i = 0; i < 128; i++) {
			if (!user_ok(r->ebx + i * 2, 2)) {
				r->eax = (unsigned)-EFAULT;
				break;
			}
			ubuf[i] = *(unsigned short *)(r->ebx + i * 2);
			if (ubuf[i] == 0)
				break;
			n++;
		}
		if (i >= 128 || n == 0) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		kstr_init(&s, kbuf, sizeof(kbuf));
		if (kstr_set_w(&s, ubuf, n * 2) < 0) {
			r->eax = (unsigned)-EINVAL;
			break;
		}
		i = (unsigned)vfs_namei_k(&s, &vp);
		if (vp != 0)
			vn_put(vp);
		r->eax = i;
#else
		r->eax = (unsigned)-ENOSYS;
#endif
		break;
	}

	/* ---- S300: KP-4/6/7 IPC and shared-page syscalls -------------- */
	case SYS_IPC_PORT_REGISTER:	/* port_register(port) */
		r->eax = (unsigned)port_register(r->ebx);
		break;
	case SYS_IPC_PORT_RELEASE:	/* port_release(port) */
		r->eax = (unsigned)port_release(r->ebx);
		break;
	case SYS_IPC_SEND: {		/* ipc_send(port, struct ipc_msg *) */
		struct ipc_msg m;
		int n;

		if (!user_ok(r->ecx, sizeof(m))) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		bcopy((void *)r->ecx, &m, sizeof(m));
		m.from = (unsigned)cur->uk_pid;	/* 内核填发送方 pid */
		/* 中文：KVFS 回复优先——fs_server 把回复发到端口1 时，若有内核
		 * 转发事务在途，直接回填等待者并唤醒，不入回复队列。
		 * EN: KVFS reply first - when fs_server sends a reply to port 1
		 * and a kernel forwarding transaction is in flight, complete the
		 * waiter directly and wake it, bypassing the reply queue. */
		if (kvfs_reply_deliver(r->ebx, &m)) {
			r->eax = 0;
			break;
		}
		/* 中文：会合优先——若有人阻塞在 recv，直接投递并唤醒，不入队。
		 * EN: rendezvous first - if someone is blocked in recv, deliver
		 * directly and wake them, bypassing the queue. */
		if (ipc_deliver_waiter(r->ebx, &m)) {
			r->eax = 0;
			break;
		}
		n = ipc_send(r->ebx, &m);
		r->eax = (unsigned)n;
		break;
	}
	case SYS_IPC_RECV: {		/* ipc_recv(port, struct ipc_msg *) */
		struct ipc_msg m;
		int n;

		if (!user_ok(r->ecx, sizeof(m))) {
			r->eax = (unsigned)-EFAULT;
			break;
		}
		n = ipc_recv(r->ebx, &m);
		if (n == 0) {
			bcopy(&m, (void *)r->ecx, sizeof(m));
			r->eax = 0;
			break;
		}
		if (n != -EAGAIN) {	/* -EIO（服务退出）等：直接返回 */
			r->eax = (unsigned)n;
			break;
		}
		/* 中文：队列空且端口有属主——真正阻塞。保存完整 syscall 上下文到
		 * uk_regs，登记等待参数，然后调用 proc_sched 让出 CPU；发送方
		 * 或端口释放路径会写回 uk_regs.eax 并唤醒，恢复后如 syscall 返回。
		 * EN: empty queue with a live owner - block for real.  Save the full
		 * syscall context in uk_regs, record wait args, then proc_sched().
		 * The sender or port-release path writes uk_regs.eax and wakes us;
		 * on resume it looks like the syscall returned. */
		cur->uk_regs = *r;
		cur->uk_ipc_waiting = 1;
		cur->uk_ipc_port = r->ebx;
		cur->uk_ipc_msguser = r->ecx;
		kthread_block_tcb(0);
		proc_sched();
		/* 唤醒后从 uk_regs 恢复用户态，不会执行到这里 */
		break;
	}
	case SYS_IPC_MAP_SHARED:	/* map_shared(pid, va, pa, prot) */
		r->eax = (unsigned)map_shared(r->ebx, r->ecx, r->edx, r->esi);
		break;
	case SYS_IPC_UNMAP_SHARED:	/* unmap_shared(pid, va) */
		r->eax = (unsigned)unmap_shared(r->ebx, r->ecx);
		break;
	case SYS_IPC_PAGE_ALLOC:	/* ipc page_alloc() -> physical address */
		r->eax = vfs_page_alloc();
		break;
	case SYS_IPC_PAGE_FREE:		/* ipc page_free(pa) */
		vfs_page_free(r->ebx);
		r->eax = 0;
		break;
	}

	/* S10.2: deliver one pending signal on the way back to user mode */
	if (cur != 0)
		sig_deliver(cur, r);
	return (0);
}

/*
 * Trap entry point (called from usermode_asm.s).  The old personality
 * subsystem (subsys/) is gone: syscalls dispatch straight to bsd45_syscall().
 */
/* 中文：陷阱入口：personality 子系统已删除，系统调用直接分发到 bsd45_syscall */
/* EN: Trap entry: the personality subsystem is gone, dispatch straight to bsd45_syscall */
void
syscall_dispatch(struct sysregs *r)
{

	/*
	 * S413: kernel entries serialise on the big kernel lock.  It is
	 * released on the way back to user mode and (via smp_bkl_drop) before
	 * any context switch, so a blocking syscall does not stall the other
	 * CPUs; user mode itself always runs unlocked, which is where the
	 * parallelism comes from.
	 * S413：内核入口统一进大内核锁；回用户态时释放，切换线程前由
	 * smp_bkl_drop 释放，所以阻塞系统调用不会拖住别的核；用户态本身始终
	 * 不带锁，并行度就来自这里。
	 */
	smp_bkl_enter();
	(void)bsd45_syscall(r);		/* writes the result into r->eax itself */
	/*
	 * Syscall exit is a safe point: drain deferred work before deciding
	 * whether to switch threads.
	 * 系统调用出口是安全点：先排空延迟工作，再决定是否切换线程。
	 */
	softirq_dispatch();
	/*
	 * Kernel-mode safe-point preemption: the syscall is complete, so the
	 * current user context can be saved and another thread scheduled.
	 * 内核态安全点抢占：系统调用已完成，可保存当前用户上下文并调度
	 * 其它线程；不在内核执行中途抢占。
	 */
	/* Safe point: returning to user mode.  A kernel critical section
	 * (preempt_cnt > 0) or an opted-out thread defers the switch. */
	if (need_resched && cur != 0 && cur_thread != 0 &&
	    cur_thread->t_preempt_cnt == 0 &&
	    cur_thread->t_base_ipl == 0 && cur_thread->t_preemptable) {
		need_resched = 0;
		if (cur_thread->t_tid == cur->uk_pid) {
			cur->uk_regs = *r;
			cur->uk_state = P_RUN;
		} else {
			cur_thread->t_regs = *r;
			cur_thread->t_state = KT_RUN;
		}
		/* proc_sched() drops the BKL itself before resuming a
		 * thread, and never returns. */
		proc_sched();
	}
	smp_bkl_exit();
}

/* 中文：安装系统调用门：委托 HAL 的 hal_syscall_gate_init 完成 IDT 设置 */
/* EN: Install the system-call gate: delegate to the HAL's hal_syscall_gate_init for IDT setup */
void
syscall_gate_init(void)
{

	hal_syscall_gate_init();
}

/* 中文：启动首个用户程序：经 VFS 读取并构建映像，初始化 pid 1 进程与主线程，安装系统调用门后经 enter_user 切到用户态 */
/* EN: Start the first user program: read and build the image via VFS, init pid 1 and its main thread, install the syscall gate and switch to user mode via enter_user */
void
start_prog(const char *path, const char *script)
{
	struct file *f;
	const unsigned char *img;
	unsigned len;
	unsigned pd, entry, esp;
	char *argv[4];
	int argc;
	char *envp[2];

	img = 0;
	len = 0;
	{
		struct vnode *vp = 0;
		unsigned char *buf;
		int rv;

		if (vfs_namei(path, &vp) < 0 || vp->type != VREG) {
			printf("no image for %s (vp=%x type=%d)\n", path,
			    (unsigned)vp, vp ? (int)vp->type : -1);
			return;
		}
		len = vp->size;
		buf = cd_map(len);
		if (buf == 0) {
			printf("no window for %s\n", path);
			vn_put(vp);
			return;
		}
		rv = vp->ops->read(vp, 0, buf, len);
		vn_put(vp);
		if (rv < 0 || (unsigned)rv != len) {
			printf("read failed for %s\n", path);
			return;
		}
		printf("exec %s: via vnode (%d bytes)\n", path, len);
		img = buf;
	}
	argv[0] = (char *)path;
	argc = 1;
	if (script != 0) {
		argv[argc++] = "-c";
		argv[argc++] = (char *)script;
	}
	envp[0] = "PATH=/bin:/sbin";

	pd = build_image(img, len, &entry, argv, argc, envp, 1, &esp);


	/* pid 1: the first user process, no parent */
	bzero(procs, (unsigned)NPROC * (unsigned)sizeof(struct process));
	cur = &procs[0];
	cur->uk_state = P_RUN;
	cur->uk_pid = 1;
	cur->uk_ppid = 0;
	cur->uk_pd = pd;
	cur->uk_brk_mapped = USER_HEAP_START;
	cur->uk_p_mmap_base = mmap_base;
	cur->uk_p_cwd[0] = '/';
	cur->uk_p_cwd[1] = 0;
	proc_setcomm(cur, path);
	{
		int fd;

		for (fd = 0; fd < 3; fd++) {
			cur->uk_fds[fd].used = 1;
			cur->uk_fds[fd].type = FD_CONSOLE;
			cur->uk_fds[fd].rw = (fd == 0) ? 1 : 2;
			cur->uk_fds[fd].pipeidx = -1;
			cur->uk_fds[fd].node = -1;
		}
	}
	next_pid = 2;
	(void)proc_add_thread(cur, 1);
	cur->uk_p_next_tid = TID_USER_BASE + 1 * TID_STRIDE;
	if (cur->uk_p_thread != 0) {
		hal_kernel_stack_set(cur->uk_p_thread->t_stack_top);
		/* S413: the pid-1 TCB becomes runnable only now that its
		 * address space, stack and registers are all in place. */
		sched_runnable(cur->uk_p_thread, cpu_id());
		/*
		 * S413: and it belongs to the CPU that is about to run it.
		 * enter_user() below jumps straight into ring 3 without going
		 * through the scheduler, so without this claim an AP could
		 * claim the same TCB and run pid 1 on a second CPU (same
		 * kernel stack, same address space) - which is exactly how the
		 * first SMP port died, during init's execve.
		 * S413：它属于即将运行它的这个核。下面的 enter_user 直接跳进 ring3，
		 * 不经过调度器；不在这里占位的话，AP 会领走同一个 TCB，在两个核上同时
		 * 跑 pid 1（同一内核栈、同一地址空间）——S413 第一版就死在 init 的
		 * execve 期间。
		 */
		(void)kthread_claim_cpu(cur->uk_p_thread, cpu_id());
	}

	syscall_gate_init();
	printf("exec %s: entry %x pd %x\n", path, entry, pd);
	enter_user(pd, entry, esp);
}

/* 中文：启动用户态入口：以 /sbin/init 和给定脚本调用 start_prog */
/* EN: Userland startup entry: call start_prog with /sbin/init and the given script */
void
start_userland(const char *script)
{

	start_prog("/sbin/init", script);
}

