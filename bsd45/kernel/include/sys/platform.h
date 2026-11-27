/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * platform.h - platform-facing kernel hooks (formerly hal/ipl.h, hal/clock.h,
 * hal/context.h).  The hal/ tree was removed; the still-needed declarations
 * live here.  IPL/preemption is gone -- softirq is now a simple deferred queue.
 */
#ifndef _SYS_PLATFORM_H_
#define _SYS_PLATFORM_H_

/*
 * Saved user context, filled by syscall_entry/resume_user and built by the
 * irq0 trap frame.  Formerly in subsys/subsys.h, moved here when subsys/ was
 * removed.  Field order must match usermode_asm.s push order exactly.
 */
struct sysregs {
	unsigned ds, es, edi, esi, ebp, esp, ebx, edx, ecx, eax;
	unsigned eip, cs, eflags, uesp, ss;
};

/* ---- clock / timer (arch/i386/timer.c) ---- */
void	clock_init(void);
void	clock_irq(struct sysregs *r);
void	clock_print(void);
void	clock_lat_print(void);
unsigned clock_get_ticks(void);
unsigned clock_quantum_get(void);

/* ---- context switch hooks (arch/i386/ctx.c) ---- */
void	hal_kernel_stack_set(unsigned top);
void	hal_switch_stack(unsigned *old_sp, unsigned new_sp);
int	hal_intr_from_user(const struct sysregs *r);
void	hal_intr_user_resume(struct sysregs *r);
unsigned hal_user_stack_top(void);
void	hal_address_space_activate(unsigned pd);
unsigned hal_address_space_current(void);
void	hal_cpu_halt(void);
void	hal_cpu_idle(void);		/* S432: sti+hlt for the idle loop */
void	hal_syscall_gate_init(void);

/*
 * Preemption threshold used by kthread's t_base_ipl field (formerly an
 * IPL level).  Kept so a thread can still mark itself non-preemptable.
 */
#define	KTHREAD_NO_PREEMPT	5

/* ---- deferred work (thread/softirq.c), replaced the old IPL softirq ---- */
#define	NSOFT		4
#define	SOFT_RECLAIM	0
#define	SOFT_INPUT	1
#define	SOFT_NET	2
#define	SOFT_TEST	3
void	softirq_init(void);
void	softirq_defer(void (*fn)(void *), void *arg);
void	softirq_dispatch(void);
void	softint_register(int id, void (*fn)(void));
void	softint_schedule(int id);
void	softirq_print(void);
unsigned softirq_count(void);
int	softirq_selftest(void);

#endif /* !_SYS_PLATFORM_H_ */
