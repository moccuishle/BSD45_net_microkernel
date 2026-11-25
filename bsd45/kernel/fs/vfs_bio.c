/*
 * vfs_bio.c - S17: a real buffer cache in the 4.5BSD style.
 *
 * The 4.5BSD vfs_bio.c in kernel/sys has its bodies deleted (bread() is just
 * "return EIO"), and this port never compiled it: it needs struct buf
 * lists, sleep/wakeup and bdevsw.  This file implements the same *algorithm*
 * in the port's own terms:
 *
 *	- 2048-byte blocks (one ISO9660 sector), 64 buffers, each backed by a
 *	  physical page mapped into a kernel window with pmap_map();
 *	- a hash table (dev,blkno) -> buffer and a single LRU list;
 *	- bread()/breada()/brelse()/bwrite()/bdwrite();
 *	- hit/miss/read/readahead accounting for /dev/vmstat.
 *
 * There is no sleep/wakeup here: the driver is synchronous PIO, so a miss
 * simply reads the sector.  Dirty buffers are written back by bwrite()/
 * bdwrite(); the only device today is the read-only CD, so writes report
 * EROFS (the path is kept so an ATA disk can be plugged in later).
 *
 * Layer rule: this file talks to the driver (atapi_read) and the page
 * allocator (vfs_page_*), never to the process table or the VFS vnodes.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/smp.h>

#include "atapi.h"
#include "ata.h"
#include "vfs.h"

#ifndef PAGE_SIZE
#define	PAGE_SIZE	4096
#endif
#ifndef VM_PROT_READ
#define	VM_PROT_READ	0x1
#define	VM_PROT_WRITE	0x2
#endif

extern void pmap_map(vm_offset_t, vm_offset_t, vm_offset_t, int);
extern void bcopy(const void *, void *, unsigned);
extern void bzero(void *, unsigned);
extern int strcmp(const char *, const char *);

#define	NBUF		64
#define	NBIOHASH	32
#define	BIOSIZE		2048		/* CD sector */

#define	BUF_VA		0xFE800000	/* kernel window: 64 x 4K pages */
#define	BUF_PAGE	4096

#define	B_VALID		0x01
#define	B_DIRTY		0x02

struct buf {
	unsigned	b_dev;
	unsigned	b_blkno;
	unsigned	b_flags;
	unsigned	b_refcnt;
	unsigned char	*b_data;
	unsigned	b_pa;
	int		b_hnext;	/* hash chain */
	int		b_lprev, b_lnext;	/* LRU (inactive) list */
};

static struct buf bufs[NBUF];
static int bio_hash[NBIOHASH];
static int bio_lru_head, bio_lru_tail;
static int bio_inited;

static unsigned bio_hits, bio_misses, bio_reads, bio_readahead;
static unsigned bio_dirty;

/*
 * P2: buffer-cache table lock.  It covers the hash chains, the LRU,
 * b_refcnt and the accounting counters, but never spans a device access:
 * the sector read in bread()/breada() and the write-back in bio_evict()
 * run with the lock dropped, because both are synchronous PIO and would
 * otherwise spin every other CPU for the duration of the transfer.
 * A buffer being filled is off the hash and off the LRU, so it is
 * invisible to other CPUs until it is published.
 * Lock order: bio_lock -> page_lock (bio_init -> vfs_page_alloc).
 */
/* 中文：P2 缓冲缓存表锁：保护哈希链、LRU、b_refcnt 与统计计数，但不跨越设备访问——扇区读与回写都在放锁后执行；正在填充的缓冲区不在哈希与 LRU 上，其它 CPU 看不到。 */
static struct spinlock bio_lock;

/* EN: Returns the hash bucket for a device and block number, hashing blkno with the 2654435761 multiplier, adding dev and taking the result modulo NBIOHASH. */
/* 中文：计算设备号与块号对应的哈希桶：先用乘子 2654435761 散列 blkno，加上 dev 后对 NBIOHASH 取模并返回。 */
static int
bio_bucket(unsigned dev, unsigned blkno)
{

	return ((int)((blkno * 2654435761u + dev) % NBIOHASH));
}

/* EN: One-shot buffer-cache setup: clears hash chains and stats, then gives each of the NBUF buffers a vfs_page_alloc() page mapped RW at BUF_VA and chains them into the LRU list. */
/* 中文：只执行一次的缓冲缓存初始化：清空哈希链和统计计数，为 NBUF 个缓冲区各分配一页并用 pmap_map 以读写方式映射到 BUF_VA，再串成 LRU 链表。 */
void
bio_init(void)
{
	int i;

	if (bio_inited)
		return;
	/*
	 * Called from vfs_init() before smp_init() brings the APs online,
	 * so the table is built uniprocessor; bio_inited is raised last.
	 */
	/* 中文：由 vfs_init() 在 smp_init() 启动 AP 之前调用，构建期是单 CPU；bio_inited 最后置位。 */
	for (i = 0; i < NBIOHASH; i++)
		bio_hash[i] = -1;
	bio_lru_head = bio_lru_tail = -1;
	bio_hits = bio_misses = bio_reads = bio_readahead = 0;
	bio_dirty = 0;
	for (i = 0; i < NBUF; i++) {
		struct buf *bp = &bufs[i];

		bzero(bp, sizeof(*bp));
		bp->b_pa = vfs_page_alloc();
		bp->b_data = (unsigned char *)(BUF_VA + i * BUF_PAGE);
		pmap_map((vm_offset_t)bp->b_data, (vm_offset_t)bp->b_pa,
		    (vm_offset_t)bp->b_pa + BUF_PAGE,
		    VM_PROT_READ | VM_PROT_WRITE);
		bp->b_hnext = -1;
		bp->b_lprev = i - 1;
		bp->b_lnext = (i + 1 < NBUF) ? i + 1 : -1;
	}
	bio_lru_head = 0;
	bio_lru_tail = NBUF - 1;
	bio_inited = 1;
}

/* ---------------------------------------------------------------- LRU */

/*
 * Every bio_ helper below runs with bio_lock held.
 */
/* 中文：下面所有 bio_ 内部辅助函数都在持有 bio_lock 时调用。 */

/* EN: Unlinks buffer i from the doubly linked LRU list, repairing the neighbours and bio_lru_head/bio_lru_tail as needed, then clears its b_lprev and b_lnext links. */
/* 中文：把缓冲区 i 从双向 LRU 链表中摘下，按需修正相邻节点以及 bio_lru_head/bio_lru_tail，最后把它的 b_lprev、b_lnext 置为 -1。 */
static void
bio_lru_remove(int i)
{
	struct buf *bp = &bufs[i];

	if (bp->b_lprev >= 0)
		bufs[bp->b_lprev].b_lnext = bp->b_lnext;
	else if (bio_lru_head == i)
		bio_lru_head = bp->b_lnext;
	if (bp->b_lnext >= 0)
		bufs[bp->b_lnext].b_lprev = bp->b_lprev;
	else if (bio_lru_tail == i)
		bio_lru_tail = bp->b_lprev;
	bp->b_lprev = bp->b_lnext = -1;
}

/* EN: Appends buffer i to the tail of the LRU list, linking it after the old bio_lru_tail and making it the new tail, or setting it as head when the list was empty. */
/* 中文：把缓冲区 i 挂到 LRU 链表尾部：前驱指向原 bio_lru_tail 并把 i 设为新尾；链表原本为空时则同时设为头。 */
static void
bio_lru_append(int i)
{
	struct buf *bp = &bufs[i];

	bp->b_lprev = bio_lru_tail;
	bp->b_lnext = -1;
	if (bio_lru_tail >= 0)
		bufs[bio_lru_tail].b_lnext = i;
	else
		bio_lru_head = i;
	bio_lru_tail = i;
}

/* ---------------------------------------------------------------- hash */

/* EN: Walks the hash chain for the buffer's bucket and returns the index of the buffer whose b_dev and b_blkno match, or -1 when the block is not cached. */
/* 中文：沿目标桶的哈希链查找，返回 b_dev 与 b_blkno 都匹配的缓冲区下标；该块不在缓存中时返回 -1。 */
static int
bio_find(unsigned dev, unsigned blkno)
{
	int i;

	for (i = bio_hash[bio_bucket(dev, blkno)]; i >= 0;
	    i = bufs[i].b_hnext)
		if (bufs[i].b_dev == dev && bufs[i].b_blkno == blkno)
			return (i);
	return (-1);
}

/* EN: Searches the buffer's own hash bucket for its index using a pointer-to-pointer walk, splices it out of the chain and sets b_hnext to -1; does nothing if not found. */
/* 中文：在缓冲区所属哈希桶中用二级指针遍历找到其下标，把它从链上摘除并将 b_hnext 置 -1；未找到则不做任何事。 */
static void
bio_hash_remove(int i)
{
	int b = bio_bucket(bufs[i].b_dev, bufs[i].b_blkno);
	int *pp = &bio_hash[b];

	while (*pp >= 0) {
		if (*pp == i) {
			*pp = bufs[i].b_hnext;
			bufs[i].b_hnext = -1;
			return;
		}
		pp = &bufs[*pp].b_hnext;
	}
}

/* EN: Inserts buffer i at the head of the hash chain for its b_dev and b_blkno, storing the previous chain head in b_hnext. */
/* 中文：把缓冲区 i 头插到其 b_dev、b_blkno 对应的哈希链，原链头存入 b_hnext。 */
static void
bio_hash_insert(int i)
{
	int b = bio_bucket(bufs[i].b_dev, bufs[i].b_blkno);

	bufs[i].b_hnext = bio_hash[b];
	bio_hash[b] = i;
}

/* ---------------------------------------------------------------- I/O */

/* EN: Reads one block into data; only dev BDEV_CD is supported (via atapi_read), returning -EINVAL for any other device and -EIO when the ATAPI read fails. */
/* 中文：读取一个块到 data：目前只支持 dev 为 BDEV_CD 的情况（经 atapi_read 完成），其他设备返回 -EINVAL，ATAPI 读失败返回 -EIO。 */
static int
bio_read_block(unsigned dev, unsigned blkno, void *data)
{

	if (dev == BDEV_CD) {
		if (atapi_read(blkno, 1, data) < 0)
			return (-EIO);
		return (0);
	}
	if (dev == BDEV_WD) {
		/* 2048-byte buffer cache block = 4 x 512 ATA sectors. */
		if (ata_read(blkno * 4, 4, data) < 0)
			return (-EIO);
		return (0);
	}
	return (-EINVAL);
}

/*
 * Detach the least recently used buffer so it can be reused.  The buffer
 * keeps its b_dev/b_blkno/b_flags until the caller resets it, because a
 * dirty buffer still has to be written back after the lock is dropped.
 * Returns the index, or -1 when nothing is evictable.
 */
/* EN: Detaches the LRU-head buffer from the LRU (and the hash when B_VALID) and returns its index, leaving dev/blkno/flags intact so the caller can write it back unlocked; -1 when the LRU is empty. */
/* 中文：把 LRU 链头缓冲区从 LRU（B_VALID 时也从哈希）摘下并返回其下标，保留 dev/blkno/flags 以便调用方在放锁后回写；LRU 为空返回 -1。 */
static int
bio_evict(void)
{
	int i;

	i = bio_lru_head;
	if (i < 0)
		return (-1);
	bio_lru_remove(i);
	if (bufs[i].b_flags & B_VALID)
		bio_hash_remove(i);
	return (i);
}

/* EN: Clears the recycled buffer's flags, device, block number and reference count so it can be re-targeted at a new block. */
/* 中文：清空被回收缓冲区的标志、设备号、块号与引用计数，以便重新指向新块。 */
static void
bio_reset(int i)
{

	bufs[i].b_flags = 0;
	bufs[i].b_dev = 0;
	bufs[i].b_blkno = 0;
	bufs[i].b_refcnt = 0;
}

/* EN: Takes the lock, detaches an LRU buffer, and reports through *dirty whether a write-back is owed (the dirty flag is cleared and bio_dirty decremented under the lock). */
/* 中文：加锁摘出一个 LRU 缓冲区；若它原先是脏的，则清掉 B_DIRTY、递减 bio_dirty 并通过 *dirty 告诉调用方需要回写。 */
static int
bio_take(int *dirty)
{
	int i;

	*dirty = 0;
	spin_lock(&bio_lock);
	i = bio_evict();
	if (i >= 0 && (bufs[i].b_flags & B_DIRTY)) {
		bufs[i].b_flags &= ~B_DIRTY;
		if (bio_dirty > 0)
			bio_dirty--;
		*dirty = 1;
	}
	spin_unlock(&bio_lock);
	return (i);
}

/* EN: Returns a referenced buffer for dev/blkno: a hash hit just bumps b_refcnt; a miss evicts (writing back outside the lock), reads the block, then publishes it only if no other CPU published the same block meanwhile; NULL on eviction or read failure. */
/* 中文：取得 dev/blkno 对应缓冲并增加引用：命中只递增 b_refcnt；未命中则淘汰（回写在锁外）、读块，并在插入哈希前二次确认没有别的 CPU 抢先发布同一块；淘汰或读失败返回 NULL。 */
struct buf *
bread(unsigned dev, unsigned blkno)
{
	int i, j, dirty, rv;
	struct buf *bp;

	if (!bio_inited)
		bio_init();
	spin_lock(&bio_lock);
	i = bio_find(dev, blkno);
	if (i >= 0) {
		bio_hits++;
		bio_lru_remove(i);
		bufs[i].b_refcnt++;
		bp = &bufs[i];
		spin_unlock(&bio_lock);
		return (bp);
	}
	spin_unlock(&bio_lock);

	i = bio_take(&dirty);
	if (i < 0)
		return (0);
	bp = &bufs[i];
	if (dirty)
		(void)bwrite(bp);	/* write-back: no lock held */

	spin_lock(&bio_lock);
	j = bio_find(dev, blkno);
	if (j >= 0) {			/* lost the race: reuse the published one */
		bio_reset(i);
		bio_lru_append(i);
		bufs[j].b_refcnt++;
		bio_hits++;
		bp = &bufs[j];
		spin_unlock(&bio_lock);
		return (bp);
	}
	bp->b_dev = dev;
	bp->b_blkno = blkno;
	bp->b_refcnt = 1;
	bp->b_flags = 0;
	spin_unlock(&bio_lock);

	rv = bio_read_block(dev, blkno, bp->b_data);

	spin_lock(&bio_lock);
	if (rv < 0) {
		bio_reset(i);
		bio_lru_append(i);
		spin_unlock(&bio_lock);
		return (0);
	}
	j = bio_find(dev, blkno);
	if (j >= 0) {			/* another CPU published it while we read */
		bio_reset(i);
		bio_lru_append(i);
		bufs[j].b_refcnt++;
		bio_hits++;
		bp = &bufs[j];
		spin_unlock(&bio_lock);
		return (bp);
	}
	bp->b_flags = B_VALID;
	bio_hash_insert(i);
	bio_misses++;
	bio_reads++;
	spin_unlock(&bio_lock);
	return (bp);
}

/*
 * Prefetch n blocks starting at blkno.  The buffers are left on the LRU with
 * a zero reference, so a later bread() gets a hit; if the cache is full the
 * prefetch simply stops.
 */
/* EN: Prefetches n consecutive blocks, skipping cached ones; each other is evicted, read with b_refcnt 0, hash-inserted and LRU-appended, stopping on the first failure. */
/* 中文：预读 n 个连续块，已缓存的跳过；其余各块先淘汰缓冲区，以 b_refcnt=0 读入后插入哈希链并挂到 LRU，遇到首次失败即停止。 */
void
breada(unsigned dev, unsigned blkno, unsigned n)
{
	unsigned k;

	if (!bio_inited)
		bio_init();
	for (k = 0; k < n; k++) {
		int i, dirty, rv;
		struct buf *bp;

		spin_lock(&bio_lock);
		if (bio_find(dev, blkno + k) >= 0) {
			spin_unlock(&bio_lock);
			continue;
		}
		spin_unlock(&bio_lock);

		i = bio_take(&dirty);
		if (i < 0)
			break;
		bp = &bufs[i];
		if (dirty)
			(void)bwrite(bp);	/* write-back: no lock held */

		spin_lock(&bio_lock);
		bp->b_dev = dev;
		bp->b_blkno = blkno + k;
		bp->b_refcnt = 0;
		bp->b_flags = 0;
		spin_unlock(&bio_lock);

		rv = bio_read_block(dev, bp->b_blkno, bp->b_data);

		spin_lock(&bio_lock);
		if (rv < 0) {
			bio_reset(i);
			bio_lru_append(i);
			spin_unlock(&bio_lock);
			break;
		}
		if (bio_find(dev, blkno + k) >= 0) {
			/* already published by someone else: keep the cache unique */
			bio_reset(i);
			bio_lru_append(i);
			spin_unlock(&bio_lock);
			continue;
		}
		bp->b_flags = B_VALID;
		bio_hash_insert(i);
		bio_lru_append(i);
		bio_readahead++;
		bio_reads++;
		spin_unlock(&bio_lock);
	}
}

/* EN: Drops a reference to bp, ignoring pointers outside bufs[]; when b_refcnt reaches zero the buffer is put back on the LRU tail unless it is already linked there. */
/* 中文：释放对 bp 的引用：bp 不在 bufs[] 范围内则直接返回；引用计数减到 0 后，若该缓冲区尚未挂在 LRU 上，就重新挂到链尾。 */
void
brelse(struct buf *bp)
{
	int i;

	if (bp == 0 || bp < bufs || bp >= bufs + NBUF)
		return;
	i = bp - bufs;
	spin_lock(&bio_lock);
	if (bp->b_refcnt > 0)
		bp->b_refcnt--;
	if (bp->b_refcnt == 0) {
		/* only re-queue if it is not already on the inactive list */
		if (bp->b_lprev < 0 && bp->b_lnext < 0 &&
		    bio_lru_tail != i)
			bio_lru_append(i);
	}
	spin_unlock(&bio_lock);
}

/* EN: Writes a buffer back, but there is no writable device yet: it returns -EINVAL for a NULL bp, -EROFS when bp belongs to the read-only CD device, and -ENXIO otherwise. */
/* 中文：回写缓冲区，但目前没有可写设备：bp 为 NULL 返回 -EINVAL，属于只读 CD 设备返回 -EROFS，其余情况返回 -ENXIO。 */
int
bwrite(struct buf *bp)
{

	if (bp == 0)
		return (-EINVAL);
	if (bp->b_dev == BDEV_CD)
		return (-EROFS);		/* the CD is read-only */
	if (bp->b_dev == BDEV_WD) {
		if (ata_write(bp->b_blkno * 4, 4, bp->b_data) < 0)
			return (-EIO);
		spin_lock(&bio_lock);
		bp->b_flags &= ~B_DIRTY;
		spin_unlock(&bio_lock);
		return (0);
	}
	return (-ENXIO);
}

/* EN: Delayed write: marks bp B_DIRTY and bumps bio_dirty, and if it is unreferenced and unlinked it is appended to the LRU for later write-back. */
/* 中文：延迟写：给 bp 置上 B_DIRTY 并递增 bio_dirty；若它没有引用且未挂在链表上，则追加到 LRU 等待以后回写。 */
void
bdwrite(struct buf *bp)
{

	if (bp == 0)
		return;
	spin_lock(&bio_lock);
	bp->b_flags |= B_DIRTY;
	bio_dirty++;
	if (bp->b_refcnt == 0 && bp->b_lprev < 0 && bp->b_lnext < 0)
		bio_lru_append(bp - bufs);
	spin_unlock(&bio_lock);
}

/* EN: Returns the buffer's b_data pointer to its mapped page, or NULL when bp is NULL. */
/* 中文：返回缓冲区 b_data 指向的映射页地址；bp 为 NULL 时返回 NULL。 */
unsigned char *
buf_data(struct buf *bp)
{

	return (bp ? bp->b_data : 0);
}

/* EN: Fills caller-supplied counters with buffer-pool figures under bio_lock: total NBUF, used counted by scanning B_VALID, and the bio_hits/misses/reads/readahead/dirty globals. */
/* 中文：在 bio_lock 保护下把缓冲池统计数据写入调用方计数器：总数 NBUF、扫描 B_VALID 得到的已用数，以及 bio_hits/misses/reads/readahead/dirty。 */
void
bio_stats(unsigned *total, unsigned *used, unsigned *hits, unsigned *misses,
    unsigned *reads, unsigned *readahead, unsigned *dirty)
{
	int i, nused = 0;

	spin_lock(&bio_lock);
	for (i = 0; i < NBUF; i++)
		if (bufs[i].b_flags & B_VALID)
			nused++;
	*total = NBUF;
	*used = nused;
	*hits = bio_hits;
	*misses = bio_misses;
	*reads = bio_reads;
	*readahead = bio_readahead;
	*dirty = bio_dirty;
	spin_unlock(&bio_lock);
}
