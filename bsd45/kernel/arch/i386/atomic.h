/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * atomic.h - i386 atomic operations (arch-private header).
 *
 * Per the kernel assembly rule (asmble.md), all inline assembly lives under
 * kernel/arch/<arch>/.  This header declares the implementations in atomic.c;
 * generic code (e.g. sys/smp.h, sys/sync.h) includes this header and calls
 * the functions.  As of S432 no generic header carries an `__asm` block any
 * more: the lock-prefixed instructions all live in atomic.c.
 * S432：通用头文件不再包含任何 `__asm`，带 lock 前缀的指令全部收在 atomic.c。
 *
 * Naming follows the C11 atomics convention, so the return value is never a
 * guess at the call site:
 *   atomic_add()        -> value AFTER  the add
 *   atomic_fetch_add()  -> value BEFORE the add
 *   atomic_cmpxchg()    -> value BEFORE the compare-and-swap
 *   atomic_xchg()       -> value BEFORE the exchange
 * 命名沿用 C11 约定：调用点不必猜返回的是加之前还是加之后。
 */
#ifndef _ARCH_I386_ATOMIC_H_
#define _ARCH_I386_ATOMIC_H_

typedef volatile int	atomic_t;

void	atomic_set(atomic_t *v, int x);
int	atomic_read(atomic_t *v);
void	atomic_inc(atomic_t *v);
void	atomic_dec(atomic_t *v);
int	atomic_add(atomic_t *v, int x);		/* returns the NEW value */
int	atomic_fetch_add(atomic_t *v, int x);	/* returns the OLD value */
int	atomic_cmpxchg(atomic_t *v, int old, int new); /* returns OLD value */
int	atomic_xchg(atomic_t *v, int new);	/* returns previous value */

void	sync_mb(void);				/* mfence */
void	sync_rmb(void);				/* lfence */
void	sync_wmb(void);				/* sfence */

/*
 * S432: 16-bit atomic add.  A physical page's COW refcount is an
 * `unsigned short` (that table is one byte per page, mapped into a 1 MB
 * window), so the 32-bit primitives above do not fit.  Returns the value
 * before the add, which is what the "am I the last user" decision in
 * page_put() needs.
 * 16 位原子加：物理页 COW 引用计数是 unsigned short（表按每页一字节映射在
 * 1MB 窗口内），上面的 32 位原语用不上。返回加之前的旧值——page_put() 判断
 * "我是不是最后一个使用者"正需要这个。
 */
int	atomic_xadd16(unsigned short *v, int x);

#endif /* _ARCH_I386_ATOMIC_H_ */
