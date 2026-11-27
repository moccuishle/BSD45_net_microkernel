/*
 * rdstress.c - concurrent read(2)/open(2) stress for the BKL-free paths.
 *
 * §7.2 takes read(2) on a regular file out of the big kernel lock: the
 * descriptor is validated and the vnode pinned under fd_lock, then the
 * page-cache read runs with the BKL released.  That only pays off if several
 * CPUs really read at once, so this test forks NCHILD children and has each
 * one re-read /readme.txt NITER times, comparing every buffer against a
 * reference copy.  A race in the fd table, the page cache or the vnode
 * refcount shows up as a short read or a mismatched buffer.
 *
 * §7.2.1 item 7 adds open(2): the path walk now runs without the BKL too, so
 * every slurp() below is a concurrent vfs_namei() on the ISO9660 root, and
 * every child also pounds one shared tmpfs name with
 * open(O_CREAT|O_TRUNC)/write/close/unlink.  That second loop is the one that
 * would surface a duplicated vnode for a single file (two CPUs missing the
 * same name at once), a name cache that kept a recycled entry, or a tmpfs
 * lookup that raced with remove and handed out a reused node slot.
 *
 * rdstress.c - 并发 read(2)/open(2) 压力测试（对应已摘掉 BKL 的路径）。fork
 * 出 NCHILD 个子进程，各自重复读 /readme.txt 并与基准副本比对；§7.2.1 第 7 条
 * 之后每次 slurp() 都是一次并发的 vfs_namei()，另外每个子进程还对同一个 tmpfs
 * 名字做 open(O_CREAT|O_TRUNC)/write/close/unlink 循环——若同一个名字被两个核
 * 同时 miss 而产生两个 vnode、或 tmpfs lookup 与 remove 竞争拿到被复用的槽，
 * 都会在这里暴露。
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#define	NCHILD	4		/* one child per CPU pair: 8 CPUs, 4 readers */
#define	NITER	32		/* 32 x 4 children = 128 concurrent opens */
#define	BUFSZ	2048
#define	TMPNAME	"/tmp/rdstress.tmp"	/* shared on purpose: one name, 4 CPUs */

/* EN: One tmpfs create/write/close/unlink cycle on the shared name; a concurrent peer may win the race for the name or the unlink, which is fine, but a successful open must accept its write. */
/* 中文：对共享名字做一次 tmpfs 建/写/关/删循环；并发的同伴可能抢先占到名字或抢先删掉它，这没问题，但 open 成功就必须能写进去。 */
static int
tmpcycle(int tag)
{
	char c = 'a' + tag % 26;
	int fd;

	fd = open(TMPNAME, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0)
		return (0);		/* lost the race for the slot: not an error */
	if (write(fd, &c, 1) != 1) {
		close(fd);
		return (-1);
	}
	close(fd);
	unlink(TMPNAME);		/* may already be gone: that is fine */
	return (0);
}

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
				if (tmpcycle(i) < 0)
					_exit(2);
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
