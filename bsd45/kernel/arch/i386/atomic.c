/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * atomic.c - i386 atomic operations and memory barriers.
 *
 * As of S432 this is the only file outside arch/i386 that spells out the
 * lock-prefixed instructions, and it is itself under arch/i386 - which is
 * what asmble.md asks for: no assembly anywhere else in kernel/.
 * Before S432 the same sequences were duplicated as static __inline
 * functions in sys/smp.h (atomic_cmpxchg/atomic_xchg/atomic_add) and in
 * sys/usermode.c (page_ref_add's `lock xaddw`); both call sites now go
 * through this file so only one copy of each instruction exists.
 * S432 起，带 lock 前缀的指令只在 arch/i386 内出现（本文件），符合 asmble.md
 * "kernel/ 中 arch/ 之外不得有汇编"的要求。此前同样序列在 sys/smp.h 里有
 * 一份内联重复、sys/usermode.c 里还有一份 16 位版本，现在统一收敛到这里。
 */
#include "arch/i386/atomic.h"

void
atomic_set(atomic_t *v, int x)
{

	*v = x;
}

int
atomic_read(atomic_t *v)
{

	return (*v);
}

void
atomic_inc(atomic_t *v)
{

	__asm __volatile("lock incl %0" : "+m" (*v));
}

void
atomic_dec(atomic_t *v)
{

	__asm __volatile("lock decl %0" : "+m" (*v));
}

/* EN: Atomically add x, returning the value AFTER the add. */
int
atomic_add(atomic_t *v, int x)
{
	int old = x;

	__asm __volatile("lock; xaddl %0,%1"
	    : "+r" (old), "+m" (*v)
	    :
	    : "memory");
	return (old + x);
}

/* EN: Atomically add x, returning the value BEFORE the add. */
int
atomic_fetch_add(atomic_t *v, int x)
{
	int old = x;

	__asm __volatile("lock; xaddl %0,%1"
	    : "+r" (old), "+m" (*v)
	    :
	    : "memory");
	return (old);
}

int
atomic_cmpxchg(atomic_t *v, int old, int new)
{
	int prev;

	__asm __volatile("lock cmpxchgl %2,%1"
	    : "=a" (prev), "+m" (*v)
	    : "r" (new), "0" (old) : "memory");
	return (prev);
}

/* EN: Exchange, returning the previous value (the old atomic_test_and_set). */
int
atomic_xchg(atomic_t *v, int new)
{
	int prev = new;

	__asm __volatile("xchgl %0,%1"
	    : "=r" (prev), "+m" (*v) : "0" (new) : "memory");
	return (prev);
}

/*
 * EN: 16-bit atomic add, returns the value BEFORE the add.
 * 中文：16 位原子加，返回加之前的旧值。
 */
int
atomic_xadd16(unsigned short *v, int x)
{
	unsigned short old = (unsigned short)x;

	__asm __volatile("lock; xaddw %0,%1"
	    : "+r" (old), "+m" (*v)
	    :
	    : "memory");
	return ((int)old);
}

void
sync_mb(void)
{

	__asm __volatile("mfence" ::: "memory");
}

void
sync_rmb(void)
{

	__asm __volatile("lfence" ::: "memory");
}

void
sync_wmb(void)
{

	__asm __volatile("sfence" ::: "memory");
}
