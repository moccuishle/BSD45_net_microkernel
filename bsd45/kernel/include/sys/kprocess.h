/*-
 * Copyright (c) 1986, 1989, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)proc.h	8.15 (Berkeley) 5/19/95
 */

#ifndef _SYS_PROC_H_
#define	_SYS_PROC_H_

#include <machine/kprocess.h>		/* Machine-dependent proc substruct. */
#include <sys/select.h>			/* For struct selinfo. */
#include <sys/queue.h>
#include <sys/kproc.h>			/* Kernel-mode process runtime state + KLT list */

/*
 * One structure allocated per session.
 */
struct	session {
	int	s_count;		/* Ref cnt; pgrps in session. */
	struct	process *s_leader;		/* Session leader. */
	struct	vnode *s_ttyvp;		/* Vnode of controlling terminal. */
	struct	tty *s_ttyp;		/* Controlling terminal. */
	char	s_login[MAXLOGNAME];	/* Setlogin() name. */
};

/*
 * One structure allocated per process group.
 */
struct	pgrp {
	LIST_ENTRY(pgrp) pg_hash;	/* Hash chain. */
	LIST_HEAD(, proc) pg_members;	/* Pointer to pgrp members. */
	struct	session *pg_session;	/* Pointer to session. */
	pid_t	pg_id;			/* Pgrp id. */
	int	pg_jobc;	/* # procs qualifying pgrp for job control */
};

/*
 * Description of a process.
 *
 * This structure contains the information needed to manage a thread of
 * control, known in UN*X as a process; it has references to substructures
 * containing descriptions of things that the process uses, but may share
 * with related processes.  The process structure and the substructures
 * are always addressible except for those marked "(PROC ONLY)" below,
 * which might be addressible only on a processor on which the process
 * is running.
 */
struct	process {
	struct	process *p_forw;		/* Doubly-linked run/sleep queue. */
	struct	process *p_back;
	LIST_ENTRY(proc) p_list;	/* List of all processes. */

	/* substructures: */
	struct	pcred *p_cred;		/* Process owner's identity. */
	struct	filedesc *p_fd;		/* Ptr to open files structure. */
	struct	pstats *p_stats;	/* Accounting/statistics (PROC ONLY). */
	struct	plimit *p_limit;	/* Process limits. */
	struct	vmspace *p_vmspace;	/* Address space. */
	struct	sigacts *p_sigacts;	/* Signal actions, state (PROC ONLY). */

#define	p_ucred		p_cred->pc_ucred
#define	p_rlimit	p_limit->pl_rlimit

	int	p_flag;			/* P_* flags. */
	char	p_stat;			/* S* process status. */
	char	p_pad1[3];

	pid_t	p_pid;			/* Process identifier. */
	LIST_ENTRY(proc) p_pglist;	/* List of processes in pgrp. */
	struct	process *p_pptr;	 	/* Pointer to parent process. */
	LIST_ENTRY(proc) p_sibling;	/* List of sibling processes. */
	LIST_HEAD(, proc) p_children;	/* Pointer to list of children. */

/* The following fields are all zeroed upon creation in fork. */
#define	p_startzero	p_oppid

	pid_t	p_oppid;	 /* Save parent pid during ptrace. XXX */
	int	p_dupfd;	 /* Sideways return value from fdopen. XXX */

	/* scheduling */
	u_int	p_estcpu;	 /* Time averaged value of p_cpticks. */
	int	p_cpticks;	 /* Ticks of cpu time. */
	fixpt_t	p_pctcpu;	 /* %cpu for this process during p_swtime */
	void	*p_wchan;	 /* Sleep address. */
	char	*p_wmesg;	 /* Reason for sleep. */
	u_int	p_swtime;	 /* Time swapped in or out. */
	u_int	p_slptime;	 /* Time since last blocked. */

	struct	itimerval p_realtimer;	/* Alarm timer. */
	struct	timeval p_rtime;	/* Real time. */
	u_quad_t p_uticks;		/* Statclock hits in user mode. */
	u_quad_t p_sticks;		/* Statclock hits in system mode. */
	u_quad_t p_iticks;		/* Statclock hits processing intr. */

	int	p_traceflag;		/* Kernel trace points. */
	struct	vnode *p_tracep;	/* Trace to vnode. */

	int	p_siglist;		/* Signals arrived but not delivered. */

	struct	vnode *p_textvp;	/* Vnode of executable. */

	short	p_locks;		/* DEBUG: lockmgr count of held locks */
	short	p_simple_locks;		/* DEBUG: count of held simple locks */
	long	p_spare[2];		/* pad to 256, avoid shifting eproc. */

/* End area that is zeroed on creation. */
#define	p_endzero	p_hash.le_next

	/*
	 * Not copied, not zero'ed.
	 * Belongs after p_pid, but here to avoid shifting proc elements.
	 */
	LIST_ENTRY(proc) p_hash;	/* Hash chain. */

/* The following fields are all copied upon creation in fork. */
#define	p_startcopy	p_sigmask

	sigset_t p_sigmask;	/* Current signal mask. */
	sigset_t p_sigignore;	/* Signals being ignored. */
	sigset_t p_sigcatch;	/* Signals being caught by user. */

	u_char	p_priority;	/* Process priority. */
	u_char	p_usrpri;	/* User-priority based on p_cpu and p_nice. */
	char	p_nice;		/* Process "nice" value. */
	char	p_comm[MAXCOMLEN+1];

	struct 	pgrp *p_pgrp;	/* Pointer to process group. */

/* End area that is copied on creation. */
#define	p_endcopy	p_thread

	void	*p_thread;	/* Id for this "thread"; Mach glue. XXX */
	struct	user *p_addr;	/* Kernel virtual addr of u-area (PROC ONLY). */
	struct	mdproc p_md;	/* Any machine-dependent fields. */

	u_short	p_xstat;	/* Exit status for wait; also stop signal. */
	u_short	p_acflag;	/* Accounting flags. */
	struct	rusage *p_ru;	/* Exit information. XXX */

	/*
	 * BSD45-port process runtime state, appended at the tail so legacy
	 * offsets used by genassym.c/machdep.c stay valid.  struct process is the
	 * authoritative kernel process object: it owns the address space (uk_pd),
	 * resources and a KLT list (uk_threads).  The uk_ prefix avoids colliding
	 * with legacy BSD p_* fields.
	 */
	int		uk_state;
	int		uk_pipeidx;
	int		uk_piperw;
	unsigned	uk_pipeat;
	unsigned	uk_pipelen;
	int		uk_pid;
	int		uk_ppid;
	unsigned	uk_pd;			/* page directory = address space */
	unsigned	uk_brk_mapped;
	struct kfd	uk_fds[KPROC_NFD];
	struct sysregs	uk_regs;	/* main thread's resume context */
	int		uk_exitcode;
	int		uk_wait_pid;
	unsigned	uk_wait_status;
	int		uk_vfork_wait;
	int		uk_vfork_child;
	unsigned	uk_p_sig;
	unsigned	uk_p_sigmask;
	unsigned	uk_p_sigignore;
	unsigned	uk_p_sigcatch;
	unsigned	uk_p_sighand[KPROC_NSIG];
	int		uk_p_pgrp;
	char		uk_p_cwd[KPROC_CWDLEN];
	char		uk_p_comm[KPROC_MAXCOMMLEN];
	int		uk_p_stopsig;
	int		uk_p_termsig;
	int		uk_p_cont;
	unsigned	uk_wait_options;
	unsigned	uk_p_mmap_base;
	struct kvma	uk_p_vma[KPROC_NMAP];
	struct kthread	*uk_p_thread;	/* running thread of this process */
	struct kthread	*uk_p_threads;	/* head of this process's KLT list */
	int		uk_p_nthreads;
	/* S425: proc_exit() could not quiesce a CPU that still had this
	 * address space loaded, so the page directory was deliberately not
	 * returned to the allocator (leaked, with a warning) instead of
	 * handing a live CPU a freed page table. */
	int		uk_pd_pinned;
	/* S425: the main thread exited before its siblings; uk_exitcode
	 * already holds the status the process must finally report. */
	int		uk_p_maingone;
	int		uk_p_next_tid;
	int		uk_aio_wait_token;
	int		uk_uring_wait;
	/* Process-level fairness (BSD estcpu style): decayed CPU usage is
	 * charged here so a process with 100 threads does not get 100x CPU. */
	unsigned	uk_estcpu;	/* decayed per-process CPU usage */
	int		uk_rq_threads;	/* runnable threads this process has on the RQ */
	/* S400: IPC rendezvous resume state.  When a user thread blocks in
	 * ipc_recv(), we save its port + message user address here and stash the
	 * full syscall context in uk_regs.  The sender / port-release path writes
	 * the result into uk_regs.eax and copies the message back, then wakes the
	 * process via proc_wake(); on resume the syscall appears to have returned.
	 * S400: IPC 会合续传状态。用户线程在 ipc_recv 阻塞时，把端口与消息用户
	 * 地址存这里，完整 syscall 上下文存 uk_regs。发送方/端口释放路径把结果
	 * 写进 uk_regs.eax 并拷回消息，再经 proc_wake 唤醒；恢复时 syscall 像正常返回。
	 */
	int		uk_ipc_waiting;		/* nonzero: blocked in ipc_recv / nonzero:阻塞在 ipc_recv */
	unsigned	uk_ipc_port;		/* port being waited on / 等待的端口 */
	unsigned	uk_ipc_msguser;		/* user address of msg buffer / msg 缓冲用户地址 */
	/*
	 * KVFS: kernel-side /srv/ transparent forwarding resume state.
	 * A user thread blocked in a forwarded open/read/write has its full
	 * syscall context saved in uk_regs plus these fields; the IPC reply
	 * handler fills the result back and wakes it.  Serialized: only one
	 * in-flight kernel forwarding transaction at a time (cooperative CPU).
	 * KVFS: 内核态 /srv/ 透明转发续传状态。阻塞在转发 open/read/write 的
	 * 用户线程把完整 syscall 上下文存 uk_regs 外加这些字段；IPC 回复处理
	 * 程序回填结果并唤醒。串行化：同一时刻只有一个在途内核转发事务。
	 */
	int		uk_kvfs_waiting;	/* nonzero: blocked in kernel VFS forward */
	unsigned	uk_kvfs_op;		/* SRV_* op being awaited / 等待的 SRV_* 操作 */
	unsigned	uk_kvfs_buf;		/* user buffer VA for READ result / READ 回填用户缓冲 */
	unsigned	uk_kvfs_len;		/* requested length / 请求长度 */
	unsigned	uk_kvfs_handle;		/* srv handle for this op / 本操作的服务句柄 */
	/* per-process virtual fds for /srv/ paths (0x4000 base, handle+1 / 0=free) */
	unsigned	uk_kvfs_fds[8];
};

#define	p_session	p_pgrp->pg_session
#define	p_pgid		p_pgrp->pg_id

/* Status values. */
#define	SIDL	1		/* Process being created by fork. */
#define	SRUN	2		/* Currently runnable. */
#define	SSLEEP	3		/* Sleeping on an address. */
#define	SSTOP	4		/* Process debugging or suspension. */
#define	SZOMB	5		/* Awaiting collection by parent. */

/* These flags are kept in p_flags. */
#define	P_ADVLOCK	0x00001	/* Process may hold a POSIX advisory lock. */
#define	P_CONTROLT	0x00002	/* Has a controlling terminal. */
#define	P_INMEM		0x00004	/* Loaded into memory. */
#define	P_NOCLDSTOP	0x00008	/* No SIGCHLD when children stop. */
#define	P_PPWAIT	0x00010	/* Parent is waiting for child to exec/exit. */
#define	P_PROFIL	0x00020	/* Has started profiling. */
#define	P_SELECT	0x00040	/* Selecting; wakeup/waiting danger. */
#define	P_SINTR		0x00080	/* Sleep is interruptible. */
#define	P_SUGID		0x00100	/* Had set id privileges since last exec. */
#define	P_SYSTEM	0x00200	/* System proc: no sigs, stats or swapping. */
#define	P_TIMEOUT	0x00400	/* Timing out during sleep. */
#define	P_TRACED	0x00800	/* Debugged process being traced. */
#define	P_WAITED	0x01000	/* Debugging process has waited for child. */
#define	P_WEXIT		0x02000	/* Working on exiting. */
#define	P_EXEC		0x04000	/* Process called exec. */

/* Should probably be changed into a hold count. */
#define	P_NOSWAP	0x08000	/* Another flag to prevent swap out. */
#define	P_PHYSIO	0x10000	/* Doing physical I/O. */

/* Should be moved to machine-dependent areas. */
#define	P_OWEUPC	0x20000	/* Owe process an addupc() call at next ast. */

/*
 * MOVE TO ucred.h?
 *
 * Shareable process credentials (always resident).  This includes a reference
 * to the current user credentials as well as real and saved ids that may be
 * used to change ids.
 */
struct	pcred {
	struct	ucred *pc_ucred;	/* Current credentials. */
	uid_t	p_ruid;			/* Real user id. */
	uid_t	p_svuid;		/* Saved effective user id. */
	gid_t	p_rgid;			/* Real group id. */
	gid_t	p_svgid;		/* Saved effective group id. */
	int	p_refcnt;		/* Number of references. */
};

#ifdef KERNEL
/*
 * We use process IDs <= PID_MAX; PID_MAX + 1 must also fit in a pid_t,
 * as it is used to represent "no process group".
 */
#define	PID_MAX		30000
#define	NO_PID		30001

#define SESS_LEADER(p)	((p)->p_session->s_leader == (p))
#define	SESSHOLD(s)	((s)->s_count++)
#define	SESSRELE(s) {							\
	if (--(s)->s_count == 0)					\
		FREE(s, M_SESSION);					\
}

#define	PIDHASH(pid)	(&pidhashtbl[(pid) & pidhash])
extern LIST_HEAD(pidhashhead, proc) *pidhashtbl;
extern u_long pidhash;

#define	PGRPHASH(pgid)	(&pgrphashtbl[(pgid) & pgrphash])
extern LIST_HEAD(pgrphashhead, pgrp) *pgrphashtbl;
extern u_long pgrphash;

extern struct process *curproc;		/* Current running proc. */
extern struct process proc0;		/* Process slot for swapper. */
extern int nprocs, maxproc;		/* Current and max number of procs. */

LIST_HEAD(proclist, proc);
extern struct proclist allproc;		/* List of all processes. */
extern struct proclist zombproc;	/* List of zombie processes. */
struct process *initproc, *pageproc;	/* Process slots for init, pager. */

#define	NQS	32			/* 32 run queues. */
int	whichqs;			/* Bit mask summary of non-empty Q's. */
struct	prochd {
	struct	process *ph_link;		/* Linked list of running processes. */
	struct	process *ph_rlink;
} qs[NQS];

struct process *pfind __P((pid_t));	/* Find process by id. */
struct pgrp *pgfind __P((pid_t));	/* Find process group by id. */

int	chgproccnt __P((uid_t uid, int diff));
int	enterpgrp __P((struct process *p, pid_t pgid, int mksess));
void	fixjobc __P((struct process *p, struct pgrp *pgrp, int entering));
int	inferior __P((struct process *p));
int	leavepgrp __P((struct process *p));
void	mi_switch __P((void));
void	pgdelete __P((struct pgrp *pgrp));
void	procinit __P((void));
void	resetpriority __P((struct process *));
void	setrunnable __P((struct process *));
void	setrunqueue __P((struct process *));
void	sleep __P((void *chan, int pri));
int	tsleep __P((void *chan, int pri, char *wmesg, int timo));
void	unsleep __P((struct process *));
void	wakeup __P((void *chan));
#endif	/* KERNEL */
#endif	/* !_SYS_PROC_H_ */
