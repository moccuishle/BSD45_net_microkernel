/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * hal_arch.s - i386 port I/O and CPU control for the HAL.
 *
 * The C side (kernel/arch/i386/hal/hal.h) only declares these functions;
 * no inline assembly is allowed in kernel C sources.
 */
	.text

/* unsigned char hal_inb(int port) */
	.globl	_hal_inb
	.align	4
_hal_inb:
	movl	4(%esp), %edx
	xorl	%eax, %eax
	inb	%dx, %al
	ret

/* void hal_outb(int port, int value) */
	.globl	_hal_outb
	.align	4
_hal_outb:
	movl	4(%esp), %edx
	movl	8(%esp), %eax
	outb	%al, %dx
	ret

/* void hal_outw(int port, unsigned value) */
	.globl	_hal_outw
	.align	4
_hal_outw:
	movl	4(%esp), %edx
	movl	8(%esp), %eax
	outw	%ax, %dx
	ret

/* void hal_insw(int port, void *addr, unsigned count) */
	.globl	_hal_insw
	.align	4
_hal_insw:
	pushl	%edi
	movl	8(%esp), %edx
	movl	12(%esp), %edi
	movl	16(%esp), %ecx
	cld
	rep
	insw
	popl	%edi
	ret

/* void hal_outsw(int port, const void *addr, unsigned count) */
	.globl	_hal_outsw
	.align	4
_hal_outsw:
	pushl	%esi
	movl	8(%esp), %edx
	movl	12(%esp), %esi
	movl	16(%esp), %ecx
	cld
	rep
	outsw
	popl	%esi
	ret

/* void cpu_load_cr3(unsigned pd) */
	.globl	_cpu_load_cr3
	.align	4
_cpu_load_cr3:
	movl	4(%esp), %eax
	movl	%eax, %cr3
	ret

/* unsigned cpu_read_cr3(void) */
	.globl	_cpu_read_cr3
	.align	4
_cpu_read_cr3:
	movl	%cr3, %eax
	ret

/* void cpu_halt(void) */
	.globl	_cpu_halt
	.align	4
_cpu_halt:
	cli
	hlt
	ret

/* void cpu_cli(void) */
	.globl	_cpu_cli
	.align	4
_cpu_cli:
	cli
	ret

/* void cpu_sti(void) */
	.globl	_cpu_sti
	.align	4
_cpu_sti:
	sti
	ret

/*
 * void cpu_idle(void) - enable interrupts and stop until the next one.
 *
 * S432: the scheduler's idle loop used to spell this out as
 * `__asm __volatile("sti; hlt")` in kernel/sys/usermode.c, which asmble.md
 * forbids.  The pair must stay a single routine: an interrupt arriving
 * between an sti and a hlt would otherwise be serviced and then the CPU
 * would sleep through the following tick.
 * S432：调度器空闲循环原先在 kernel/sys/usermode.c 里内联 `sti; hlt`，
 * 违反 asmble.md。两者必须成对且不可分割——若中断落在 sti 与 hlt 之间，
 * 处理完就会一直睡到再下一个时钟。
 */
	.globl	_cpu_idle
	.align	4
_cpu_idle:
	sti
	hlt
	ret

/* unsigned cpu_read_esp(void) */
	.globl	_cpu_read_esp
	.align	4
_cpu_read_esp:
	movl	%esp, %eax
	ret

/* unsigned hal_rdtsc(void) - low 32 bits of the TSC */
	.globl	_hal_rdtsc
	.align	4
_hal_rdtsc:
	rdtsc
	ret

/* void cpu_write_esp(unsigned) */
	.globl	_cpu_write_esp
	.align	4
_cpu_write_esp:
	movl	4(%esp), %esp
	ret

/* void cpu_breakpoint(void) */
	.globl	_cpu_breakpoint
	.align	4
_cpu_breakpoint:
	int3
	ret

/* void kgdb_return_to_user(void) - restore the trap frame and iret */
	.globl	_kgdb_return_to_user
	.align	4
_kgdb_return_to_user:
	pop	%es
	pop	%ds
	popal
	nop
	addl	$8, %esp
	iret

/* floating point helpers (npx.c) */
	.globl	_npx_init
	.align	4
_npx_init:
	fninit
	ret

	.globl	_npx_init_finite
	.align	4
_npx_init_finite:
	finit
	ret

	.globl	_npx_clear
	.align	4
_npx_clear:
	fnclex
	ret

/* void npx_save(void *area) */
	.globl	_npx_save
	.align	4
_npx_save:
	movl	4(%esp), %eax
	fnsave	(%eax)
	ret

/* void npx_restore(void *area) */
	.globl	_npx_restore
	.align	4
_npx_restore:
	movl	4(%esp), %eax
	frstor	(%eax)
	ret

/* unsigned short npx_get_control(void) */
	.globl	_npx_get_control
	.align	4
_npx_get_control:
	sub	$4, %esp
	fnstcw	(%esp)
	movzwl	(%esp), %eax
	add	$4, %esp
	ret

/* unsigned short npx_get_status(void) */
	.globl	_npx_get_status
	.align	4
_npx_get_status:
	sub	$4, %esp
	fnstsw	(%esp)
	movzwl	(%esp), %eax
	add	$4, %esp
	ret

/* void npx_set_control(unsigned short) */
	.globl	_npx_set_control
	.align	4
_npx_set_control:
	sub	$4, %esp
	movzwl	8(%esp), %eax
	movw	%ax, (%esp)
	fldcw	(%esp)
	add	$4, %esp
	ret
