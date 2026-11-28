/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * softirq.c - deferred work queue (replaces the old IPL-based softirq).
 *
 * A small fixed queue of (fn,arg) callbacks plus per-id soft handlers.  A
 * kernel thread drains the queue on its own stack; syscall exit also calls
 * softirq_dispatch() as a safe point.  No interrupt priority levels.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/smp.h>

#include "sys/platform.h"
#include "thread/kthread.h"

#define	NDEFER	64

static void		(*soft_handlers[NSOFT])(void);
static unsigned		soft_runs[NSOFT];
static struct kthread	*soft_thread;
static unsigned		soft_total;
/* PROBE (temporary): deferred callbacks lost to a full queue. */
static unsigned		soft_drops;

/* deferred (fn,arg) queue */
static void		(*defer_fn[NDEFER])(void *);
static void		*defer_arg[NDEFER];
static unsigned	defer_head, defer_tail, defer_pending;
/*
 * The ring is global, but softirq_defer() is called from every CPU and
 * softirq_dispatch() runs both on the softirq thread and at syscall exit on
 * any CPU.  Without a lock two producers can claim the same slot (one
 * callback lost) and two consumers can pop the same entry, and defer_pending
 * drifts away from the real depth.  The lock covers the ring only; callbacks
 * are run after it is dropped.
 * 中文：队列是全局的，但 softirq_defer() 从每个核调用，softirq_dispatch()
 * 既在 softirq 线程跑、也在任意核的系统调用出口跑。不加锁时两个生产者会
 * 抢到同一个槽（丢一个回调），两个消费者会取走同一条，defer_pending 也会
 * 和真实深度脱节。锁只保护环形队列本身，回调在放锁之后执行。
 */
static struct spinlock	defer_lock;

/* EN: Run one deferred callback. */
static void
softirq_run(void *arg)
{
	int id = (int)arg;

	if (id < 0 || id >= NSOFT)
		return;
	soft_total++;
	soft_runs[id]++;
	if (soft_handlers[id] != 0)
		soft_handlers[id]();
}

/* EN: Drain all deferred callbacks and run pending soft handlers. */
void
softirq_dispatch(void)
{
	void (*fn)(void *);
	void *arg;

	for (;;) {
		spin_lock(&defer_lock);
		if (defer_head == defer_tail) {
			spin_unlock(&defer_lock);
			break;
		}
		fn = defer_fn[defer_head];
		arg = defer_arg[defer_head];
		defer_fn[defer_head] = 0;
		defer_arg[defer_head] = 0;
		defer_head = (defer_head + 1) % NDEFER;
		defer_pending--;
		spin_unlock(&defer_lock);
		if (fn != 0)
			fn(arg);	/* run outside the ring lock */
	}
}

/* EN: Softirq kernel thread: dispatch deferred work, else block. */
static void
softirq_thread(void *arg)
{

	(void)arg;
	for (;;) {
		/*
		 * Same lost-wakeup shape as the aio worker: testing the ring and
		 * then blocking lets a producer queue work and wake a thread
		 * that is still KT_RUN, after which it sleeps with work pending.
		 * Publish KT_BLOCK inside the ring lock instead.
		 * 与 aio worker 同一种丢唤醒形状：先判空再阻塞时，生产者可能在
		 * 中间入队并唤醒一个仍是 KT_RUN 的线程，随后它带着待办睡下。改为
		 * 在队列锁内发布 KT_BLOCK。
		 */
		spin_lock(&defer_lock);
		if (defer_head == defer_tail) {
			kthread_block_prepare();
			spin_unlock(&defer_lock);
			kthread_block_commit();
			continue;
		}
		spin_unlock(&defer_lock);
		softirq_dispatch();
	}
}

/* EN: Page-reclaim softirq handler: reclaim 8 pages from the page cache. */
static void
soft_reclaim(void)
{

	(void)pagecache_reclaim(8);
}

/* EN: Initialize softirqs: clear tables and spawn the softirq kernel thread. */
void
softirq_init(void)
{

	soft_total = 0;
	defer_head = defer_tail = defer_pending = 0;
	spin_init(&defer_lock);
	bzero(soft_handlers, sizeof(soft_handlers));
	bzero(soft_runs, sizeof(soft_runs));
	bzero(defer_fn, sizeof(defer_fn));
	bzero(defer_arg, sizeof(defer_arg));
	soft_handlers[SOFT_RECLAIM] = soft_reclaim;
	soft_thread = kthread_spawn("softirq", softirq_thread, 0);
	if (soft_thread == 0)
		printf("softirq: no kernel thread\n");
}

/* EN: Queue a deferred callback (fn,arg); wakes the softirq thread. */
void
softirq_defer(void (*fn)(void *), void *arg)
{
	unsigned next;

	if (fn == 0)
		return;
	spin_lock(&defer_lock);
	next = (defer_tail + 1) % NDEFER;
	if (next == defer_head) {	/* queue full: drop */
		/*
		 * PROBE (temporary): a dropped aio completion strands its waiter
		 * forever, so this must never be silent while we are hunting the
		 * §7.1 soak hang.
		 */
		soft_drops++;
		if (soft_drops <= 8)
			printf("softirq: queue full (pending %u), dropped callback"
			    " #%u\n", defer_pending, soft_drops);
		spin_unlock(&defer_lock);
		return;
	}
	defer_fn[defer_tail] = fn;
	defer_arg[defer_tail] = arg;
	defer_tail = next;
	defer_pending++;
	spin_unlock(&defer_lock);
	if (soft_thread != 0)
		kthread_wake(soft_thread);
}

/* EN: Register handler fn for softirq id, ignoring out-of-range ids. */
void
softint_register(int id, void (*fn)(void))
{

	if (id >= 0 && id < NSOFT)
		soft_handlers[id] = fn;
}

/* EN: Schedule softirq id by queuing softirq_run(id) and waking the thread. */
void
softint_schedule(int id)
{

	if (id < 0 || id >= NSOFT)
		return;
	softirq_defer(softirq_run, (void *)id);
}

/* EN: Print softirq statistics. */
void
softirq_print(void)
{
	int i;

	printf("softirq: total %d pending %d thread %s\n", soft_total,
	    defer_pending, soft_thread ? "up" : "down");
	for (i = 0; i < NSOFT; i++)
		printf("softirq: %d runs %d handler %s\n", i, soft_runs[i],
		    soft_handlers[i] ? "yes" : "no");
}

/* EN: Return the total number of softirq executions. */
unsigned
softirq_count(void)
{

	return (soft_total);
}

/* ---------------------------------------------------------------- test */

static volatile unsigned soft_test_count;

/* EN: Softirq handler for self-test: increment soft_test_count. */
static void
soft_test_handler(void)
{

	soft_test_count++;
}

/* EN: Softirq self-test: schedule SOFT_TEST and spin until it fires. */
int
softirq_selftest(void)
{
	int i;

	soft_test_count = 0;
	softint_register(SOFT_TEST, soft_test_handler);
	softint_schedule(SOFT_TEST);
	for (i = 0; i < 100 && soft_test_count == 0; i++) {
		softirq_dispatch();
		kthread_run();
	}
	softint_register(SOFT_TEST, 0);
	return ((int)soft_test_count);
}
