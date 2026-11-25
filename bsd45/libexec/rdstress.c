/*
 * rdstress.c - concurrent read(2) stress for the first BKL-free path.
 *
 * §7.2 takes read(2) on a regular file out of the big kernel lock: the
 * descriptor is validated and the vnode pinned under fd_lock, then the
 * page-cache read runs with the BKL released.  That only pays off if several
 * CPUs really read at once, so this test forks NCHILD children and has each
 * one re-read /readme.txt NITER times, comparing every buffer against a
 * reference copy.  A race in the fd table, the page cache or the vnode
 * refcount shows up as a short read or a mismatched buffer.
 *
 * rdstress.c - 第一条摘掉 BKL 的 read(2) 路径的并发压力测试。fork 出 NCHILD
 * 个子进程，各自重复读 /readme.txt 并把每次结果与基准副本比对；fd 表、页缓存
 * 或 vnode 引用计数上的竞态会表现为短读或内容不符。
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#define	NCHILD	4		/* one child per CPU pair: 8 CPUs, 4 readers */
#define	NITER	32		/* 32 x 4 children = 128 concurrent opens */
#define	BUFSZ	2048

/* EN: Read the whole file through one open/read/close cycle; -1 if open fails. */
/* 中文：用一次 open/read/close 读完整文件；open 失败返回 -1。 */
static int
slurp(char *buf, int sz)
{
	int fd, total = 0, n;

	fd = open("/readme.txt", O_RDONLY);
	if (fd < 0)
		return (-1);
	while (total < sz - 1) {
		n = read(fd, buf + total, sz - 1 - total);
		if (n <= 0)
			break;
		total += n;
	}
	close(fd);
	buf[total] = 0;
	return (total);
}

/* EN: Read the same file from NCHILD processes at once and report PASS/FAIL. */
/* 中文：让 NCHILD 个进程同时反复读同一文件，输出 PASS/FAIL。 */
int
main(void)
{
	char expect[BUFSZ], buf[BUFSZ];
	pid_t kids[NCHILD];
	int i, k, explen, bad = 0, st;

	explen = slurp(expect, sizeof(expect));
	if (explen <= 0) {
		printf("rdstress: cannot read /readme.txt\n");
		return (1);
	}
	printf("rdstress: %d byte reference, %d children x %d reads\n",
	    explen, NCHILD, NITER);

	for (i = 0; i < NCHILD; i++) {
		pid_t p = fork();

		if (p == 0) {
			for (k = 0; k < NITER; k++) {
				int n = slurp(buf, sizeof(buf));

				if (n != explen || strcmp(buf, expect) != 0)
					_exit(1);
			}
			_exit(0);
		}
		if (p < 0) {
			printf("rdstress: fork failed at child %d\n", i);
			return (1);
		}
		kids[i] = p;
	}
	for (i = 0; i < NCHILD; i++) {
		st = 0;
		if (wait4(kids[i], &st, 0, (struct rusage *)0) < 0 || st != 0) {
			printf("rdstress: child %d failed (status %d)\n", i, st);
			bad++;
		}
	}
	printf("rdstress: %s\n", bad ? "FAIL" : "PASS");
	return (bad ? 1 : 0);
}
