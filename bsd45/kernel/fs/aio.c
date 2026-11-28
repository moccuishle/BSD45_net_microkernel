/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * aio.c - asynchronous half of the VFS I/O split (os.md mechanism 5).
 *
 * The VFS read/write entry points are shared: a synchronous kiocb runs the
 * backend in the caller, while KI_ASYNC queues the same kiocb to this worker
 * and returns -EIOCBQUEUED.  The worker runs the backend on its own kernel
 * stack and calls ki_complete() exactly once; a real port would plug the
 * block-device completion softirq in at that point.
 * VFS 读写入口是共用的：同步 kiocb 在调用者上下文执行后端，KI_ASYNC 则把
 * 同一个 kiocb 排给本 worker 并返回 -EIOCBQUEUED；worker 在自己的内核栈上
 * 执行后端并恰好调用一次 ki_complete()。真实移植可在这里接入块设备完成软中断。
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/smp.h>

#include "fs/vfs.h"
#include "thread/kthread.h"

static struct kiocb	*aio_head, *aio_tail;
static struct kthread	*aio_worker_thread;
static unsigned		aio_queued, aio_done, aio_pending;
/* PROBE (temporary): submits that found the worker awake. */
static int		aio_probe;
/*
 * The worker sleeps on "aio_head != 0".  Testing that condition and then
 * calling kthread_block() left a window in which a submit ran kthread_wake()
 * on a still-running worker, which ignored the wake, and the worker then
 * slept with the request queued - an aio read that never completes, i.e. a
 * process asleep forever with every CPU idle.  aio_q_lock closes it the same
 * way aio_wait_sys() does: the condition is tested and KT_BLOCK published in
 * one critical section.
 * 中文：worker 睡在 "aio_head != 0" 上。先判条件再调 kthread_block() 留下了
 * 窗口：提交方在中间调 kthread_wake()，此时 worker 仍是 KT_RUN，唤醒被忽略，
 * worker 随后带着请求睡下——这次 aio 读永远不会完成，进程永久睡死、整机空转。
 * 用 aio_q_lock 按 aio_wait_sys() 同样的办法合上：判条件与发布 KT_BLOCK 在
 * 同一个临界区内完成。
 */
static struct spinlock	aio_q_lock;

/* 中文：异步 I/O 工作线程主循环：从队列取出 kiocb，用同步后端执行读或写并在完成后调用其完成回调 */
/* EN: Async I/O worker thread main loop: dequeue a kiocb, run the synchronous read/write backend and invoke its completion callback when done */
static void
aio_worker(void *arg)
{

	(void)arg;
	for (;;) {
		struct kiocb *ki;
		void (*cb)(struct kiocb *, int);
		int r;

		/*
		 * Test the queue and publish KT_BLOCK as one atomic step
		 * against vfs_submit_async(); otherwise a submit landing in
		 * between is lost and we sleep with work queued.
		 * 判空与发布 KT_BLOCK 相对 vfs_submit_async() 是一个原子动作，
		 * 否则夹在中间的提交会丢，我们带着待办睡下。
		 */
		spin_lock(&aio_q_lock);
		if (aio_head == 0) {
			kthread_block_prepare();
			spin_unlock(&aio_q_lock);
			kthread_block_commit();
			continue;
		}
		ki = aio_head;
		aio_head = ki->ki_next;
		if (aio_head == 0)
			aio_tail = 0;
		ki->ki_next = 0;
		aio_pending--;
		spin_unlock(&aio_q_lock);
		ki->ki_state = KI_ACTIVE;

		cb = ki->ki_complete;
		ki->ki_complete = 0;
		ki->ki_flags = KI_SYNC;
		r = (ki->ki_op == KI_READ) ?
		    vfs_read_sync(ki->ki_vp, ki->ki_off, ki->ki_buf,
		    ki->ki_len) :
		    vfs_write_sync(ki->ki_vp, ki->ki_off, ki->ki_buf,
		    ki->ki_len);
		ki->ki_result = r;
		ki->ki_state = KI_DONE;
		aio_done++;
		ki->ki_complete = cb;
		if (cb != 0)
			cb(ki, r);
	}
}

/* 中文：把一个 kiocb 提交到异步 I/O 队列并唤醒 worker，返回 EIOCBQUEUED 表示请求已入队 */
/* EN: Submit a kiocb to the async I/O queue and wake the worker, returning EIOCBQUEUED to indicate the request was queued */
int
vfs_submit_async(struct kiocb *ki)
{

	if (ki == 0)
		return (-EINVAL);
	ki->ki_state = KI_QUEUED;
	ki->ki_next = 0;
	/*
	 * Enqueue under the same lock the worker tests the queue with, so
	 * either it sees this request before it blocks, or it is already
	 * KT_BLOCK and the wake below lands.
	 * 与 worker 判空用同一把锁入队：要么它在阻塞前看到这条请求，要么它已是
	 * KT_BLOCK，下面的唤醒一定生效。
	 */
	spin_lock(&aio_q_lock);
	if (aio_tail != 0)
		aio_tail->ki_next = ki;
	else
		aio_head = ki;
	aio_tail = ki;
	aio_queued++;
	aio_pending++;
	spin_unlock(&aio_q_lock);
	if (aio_worker_thread != 0) {
		kthread_wake(aio_worker_thread);
		/*
		 * PROBE (temporary): a wake that left the worker blocked with
		 * work queued is exactly the lost-wakeup hang; say so instead
		 * of letting the box go quiet.
		 * 探针：唤醒后 worker 仍阻塞且队列非空，就是丢唤醒挂起的现场。
		 */
		if (aio_worker_thread->t_state == KT_BLOCK && aio_head != 0 &&
		    aio_probe++ < 8)
			printf("aio: worker blocked with %u queued after wake\n",
			    aio_pending);
	}
	return (EIOCBQUEUED);
}

/* 中文：初始化异步 I/O 队列计数并创建 aio worker 内核线程 */
/* EN: Initialize the async I/O queue counters and spawn the aio worker kernel thread */
void
vfs_aio_init(void)
{

	aio_head = aio_tail = 0;
	aio_queued = aio_done = aio_pending = 0;
	spin_init(&aio_q_lock);
	aio_worker_thread = kthread_spawn("aio", aio_worker, 0);
	if (aio_worker_thread == 0)
		printf("aio: no worker thread\n");
}

/* 中文：返回异步 I/O 的已排队、已完成与待处理请求计数 */
/* EN: Return the async I/O queued, completed and pending request counters */
void
vfs_aio_stats(unsigned *queued, unsigned *done, unsigned *pending)
{

	*queued = aio_queued;
	*done = aio_done;
	*pending = aio_pending;
}

static volatile int aio_test_done;
static volatile int aio_test_result;

/* 中文：异步自检用的完成回调，记录操作结果并设置完成标志 */
/* EN: Completion callback for the async self-test that records the operation result and sets the done flag */
static void
aio_test_complete(struct kiocb *ki, int r)
{

	(void)ki;
	aio_test_result = r;
	aio_test_done = 1;
}

/* 中文：自检：对 /readme.txt 分别做同步与异步读，校验异步请求已入队且两次读出的数据一致 */
/* EN: Self-test that reads /readme.txt both synchronously and asynchronously, verifying the async request queued and both buffers match */
int
vfs_aio_selftest(void)
{
	struct vnode *vp = 0;
	struct kiocb sync_ki, async_ki;
	char sync_buf[64], async_buf[64];
	int i, sv, av;

	if (vfs_namei("/readme.txt", &vp) < 0 || vp == 0) {
		printf("aiotest: no /readme.txt\n");
		return (-1);
	}
	bzero(&sync_ki, sizeof(sync_ki));
	bzero(&async_ki, sizeof(async_ki));
	sync_ki.ki_vp = vp;
	sync_ki.ki_op = KI_READ;
	sync_ki.ki_flags = KI_SYNC;
	sync_ki.ki_off = 0;
	sync_ki.ki_buf = sync_buf;
	sync_ki.ki_len = 32;
	sv = vfs_read_sync(vp, 0, sync_buf, 32);

	async_ki.ki_vp = vp;
	async_ki.ki_op = KI_READ;
	async_ki.ki_flags = KI_ASYNC;
	async_ki.ki_off = 0;
	async_ki.ki_buf = async_buf;
	async_ki.ki_len = 32;
	async_ki.ki_complete = aio_test_complete;
	aio_test_done = 0;
	aio_test_result = -1;
	av = vfs_read_async(&async_ki);
	for (i = 0; i < 200 && !aio_test_done; i++)
		kthread_run();
	vn_put(vp);

	printf("aiotest: sync %d async submit %d result %d\n", sv, av,
	    aio_test_result);
	if (av != EIOCBQUEUED) {
		printf("aiotest: FAIL (async did not queue)\n");
		return (-1);
	}
	if (aio_test_result != sv || bcmp(sync_buf, async_buf,
	    (unsigned)(sv > 0 ? sv : 0)) != 0) {
		printf("aiotest: FAIL (data mismatch)\n");
		return (-1);
	}
	printf("aiotest: PASS\n");
	return (0);
}

