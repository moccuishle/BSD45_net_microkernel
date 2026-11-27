/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * context_arch.c - i386 implementation of the context HAL.
 *
 * This is the only place that knows about the TSS ring-0 stack pointer,
 * the kswitch assembly entry, the ring-3 CS bits and the IF flag.  The
 * scheduler calls the hooks declared at the top of this file.
 * 本文件是 i386 的上下文切换层：只有它知道 TSS ring0 栈指针、kswitch 汇编
 * 入口、ring3 的 CS 位和 IF 标志；调度器只调用本文件顶部声明的钩子。
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/user.h>

#include <machine/segments.h>

#include "arch/i386/io.h"
#include "sys/platform.h"
#include "sys/smp.h"
#include "thread/kthread.h"

/*
 * Context-switch hooks (formerly declared in hal/context.h).  These are the
 * generic kernel-side entry points the scheduler calls; the i386 specifics
 * (TSS esp0, kswitch, CR3, IF flag, IDT gate) live here.
 */
struct sysregs;
void	hal_kernel_stack_set(unsigned top);
void	hal_switch_stack(unsigned *old_sp, unsigned new_sp);
int	hal_intr_from_user(const struct sysregs *r);
void	hal_intr_user_resume(struct sysregs *r);
unsigned hal_user_stack_top(void);
void	hal_address_space_activate(unsigned pd);
unsigned hal_address_space_current(void);
void	hal_cpu_halt(void);
void	hal_cpu_idle(void);
void	hal_syscall_gate_init(void);

extern struct user *proc0paddr;
extern void syscall_entry(void);

/* 中文：设置本 CPU 的 ring0 栈顶。S413 起每个 CPU 有自己的 TSS，写错 TSS 会让
 * 另一个核的开中断/系统调用落到本核正在用的栈上。
 * EN: Set this CPU's ring-0 stack top.  Since S413 every CPU owns a TSS; the
 * old single proc0 TSS meant an AP's syscall would land on the BSP's stack. */
void
hal_kernel_stack_set(unsigned top)
{
	struct i386tss *t = cpu_tss(cpu_id());

	/*
	 * S414 diagnostic: the ring-0 stack must come from the stack pool
	 * (0xFC000000..) or an AP carrier stack in .bss.  A TCB-pool address
	 * (0xFEF00000..) means somebody handed the scheduler a struct
	 * kthread instead of a stack - which is how the BSP ended up
	 * executing with esp=0xFEF001E0 and dying on #SS.
	 */
	if (top >= KTHREAD_TABLE_VA && top < KTHREAD_TABLE_VA + 0x100000u)
		printf("smp: cpu%d esp0 %x is inside the TCB pool\n",
		    cpu_id(), top);
	if (t != 0)
		t->tss_esp0 = (int)top;
}

/* 中文：通过 kswitch 汇编例程把当前栈指针切换到给定的新栈，old_sp 用于保存原栈指针 */
/* EN: Switch the current stack pointer to a new stack via the kswitch assembly routine, with old_sp saving the previous pointer */
void
hal_switch_stack(unsigned *old_sp, unsigned new_sp)
{

	kswitch(old_sp, new_sp);
}

/* 中文：判断一次中断是否来自用户态：检查保存的 CS 寄存器低两位是否为 3（ring 3） */
/* EN: Determine whether an interrupt came from user mode by checking if the saved CS register's low two bits are 3 (ring 3) */
int
hal_intr_from_user(const struct sysregs *r)
{

	return (r != 0 && (r->cs & 3) == 3);
}

/* 中文：为用户态中断返回做准备，在保存的 EFLAGS 中置位 IF，使用户态恢复后中断处于开启状态 */
/* EN: Prepare for returning to user mode after an interrupt by setting IF in the saved EFLAGS so interrupts are enabled on resume */
void
hal_intr_user_resume(struct sysregs *r)
{

	if (r != 0)
		r->eflags |= 0x200;	/* IF=1: user mode runs with interrupts on */
}

/* 中文：返回 i386 平台上用户态栈的预设栈顶地址 */
/* EN: Return the preset top address of the user-mode stack on the i386 platform */
unsigned
hal_user_stack_top(void)
{

	return (0xBFC00000u);
}

/* 中文：通过加载 CR3 控制寄存器来激活给定的页目录，切换当前地址空间 */
/* EN: Activate a given page directory by loading it into control register CR3, switching the current address space */
void
hal_address_space_activate(unsigned pd)
{

	cpu_load_cr3(pd);
}

/* 中文：读取 CR3 控制寄存器，返回当前激活的页目录物理地址 */
/* EN: Read control register CR3 and return the physical address of the currently active page directory */
unsigned
hal_address_space_current(void)
{

	return (cpu_read_cr3());
}

/* 中文：暂停 CPU 执行，调用底层 cpu_halt 让处理器进入停机状态 */
/* EN: Halt CPU execution by calling the low-level cpu_halt to put the processor into a stopped state */
void
hal_cpu_halt(void)
{

	cpu_halt();
}

/*
 * EN: Wait for the next interrupt: `sti; hlt` as one indivisible pair.
 * S432: the idle loop in sys/usermode.c used to inline exactly this, which
 * asmble.md forbids outside arch/.  Kept separate from hal_cpu_halt()
 * because that one disables interrupts first and never returns to the loop
 * (it is the "no runnable process" stop), while this one must be woken by
 * the next timer tick.
 * 中文：等下一个中断——`sti; hlt` 必须成对且不可分割。S432 之前这是
 * sys/usermode.c 空闲循环里的内联汇编。与 hal_cpu_halt() 的区别：后者先
 * 关中断且不再回到循环（"无进程可运行"的停机），本函数必须被下一次
 * 时钟唤醒。
 */
void
hal_cpu_idle(void)
{

	cpu_idle();
}

/* 中文：在 IDT 中安装 0x80 号系统调用门，指向 syscall_entry 并设为用户态可见的 386 陷阱门 */
/* EN: Install the 0x80 system-call gate in the IDT pointing at syscall_entry as a user-visible 386 trap gate */
void
hal_syscall_gate_init(void)
{

	setidt(0x80, (char *)&syscall_entry, SDT_SYS386TGT, SEL_UPL);
}
