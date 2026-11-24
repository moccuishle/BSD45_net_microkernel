/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * pthmain.c - S425 regression: the main thread calls pthread_exit() while
 * worker threads are still alive.
 *
 * Note the distinction from `return` in main: that goes through exit(2),
 * which terminates the whole process (correct POSIX semantics) via
 * proc_exit().  Only pthread_exit(3) retires the calling thread and leaves
 * the process alive, and that is the path that was broken.
 *
 * Before the fix thread_exit_current() marked the main TCB KT_ZOMBIE and
 * then called kthread_block_tcb(), whose multi-threaded branch rewrites
 * t_state to KT_BLOCK.  The TCB was therefore neither a zombie nor
 * runnable, uk_p_nthreads was never decremented, and nothing ever finished
 * the process when the last worker exited: the parent's wait4() blocked
 * forever and the address space, TCB and page directory leaked.
 *
 * Expected now: main exits at once, both workers still run to completion,
 * the last one to exit runs proc_exit(), and the shell prompt comes back
 * with nothing left in `ps`.
 */
#include <stdio.h>
#include <pthread.h>

static volatile int done;

static void *
worker(void *arg)
{
	int i, n = (int)arg;

	/* burn CPU so main is guaranteed to exit first */
	for (i = 0; i < 3000000; i++)
		;
	printf("pthmain: worker %d done\n", n);
	done++;
	return (0);
}

int
main(void)
{
	pthread_t t[2];
	int i;

	printf("pthmain: main starts 2 workers, then pthread_exit\n");
	for (i = 0; i < 2; i++)
		if (pthread_create(&t[i], 0, worker, (void *)i) < 0) {
			printf("pthmain: create %d FAIL\n", i);
			return (1);
		}
	/* main leaves first; the workers outlive it and finish the process */
	pthread_exit(0);
	return (1);		/* not reached */
}
