/*
 * pagecache.c - S17: the file page cache (this port's vnode_pager).
 *
 * 4.5BSD splits this over vm_object.c / vm_page.c / vnode_pager.c.  Those
 * files need the whole vm_map/pager machinery, which this port does not
 * have, so the same job is done here in the port's terms:
 *
 *	- NPCACHE 4K pages, one physical page each, mapped into a kernel
 *	  window with pmap_map();
 *	- a hash table (cache key, page index) and an LRU;
 *	- a miss fills the page with vp->ops->readraw(), i.e. the backend's
 *	  uncached read (ISO9660: buffer cache; tmpfs: node data page);
 *	- read() is layered on pagecache_read(), so a second cat/exec of the
 *	  same file is served from memory;
 *	- mmap() gets a referenced page from pagecache_get() and maps it into
 *	  the user page directory (MAP_SHARED); MAP_PRIVATE gets a copy.
 *
 * Cache key: an ISO9660 file is immutable and identified by its LBA, so
 * pages survive close/reopen (pagecache_key() = v_key).  tmpfs is mutable,
 * so its pages are keyed on the vnode id and dropped when the vnode dies.
 *
 * Pages are preallocated at boot so the cache cannot "leak" pages into the
 * S10.1 accounting: page inuse returns to the same baseline.
 *
 * Layer rule: this file talks to vnodeops and the page allocator only.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/smp.h>

#include "vfs.h"
#include "thread/kthread.h"

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

#define	NPCACHE		128
#define	PCHASH		32
#define	PCACHE_VA	0xFE900000

struct pcent {
	int		used;
	int		refs;		/* mappings / in-flight reads */
	int		onlru;		/* linked on the eviction list */
	unsigned	key;		/* pagecache_key() of the vnode */
	unsigned	pindex;
	unsigned	pa;
	unsigned char	*va;
	int		hnext;
	int		lprev, lnext;	/* LRU of evictable entries */
};

static struct pcent pcs[NPCACHE];
static int pc_hash[PCHASH];
static int pc_lru_head, pc_lru_tail;
static int pc_inited;

static unsigned pc_hits, pc_misses, pc_fills, pc_evicts;

/*
 * P2: page-cache table lock.  It covers the hash buckets, the LRU, refs
 * and the hit/miss counters.  The fill path drops it around
 * ops->readraw(), which is synchronous ATAPI PIO: holding a spinlock
 * across that read would spin every other CPU for the whole transfer.
 * Lock order: vfs_lock -> pc_lock (vn_free -> pagecache_purge_vp);
 * pc_lock -> page_lock (pagecache_init -> vfs_page_alloc).
 */
/* 中文：P2 页缓存表锁：保护哈希桶、LRU、refs 与命中计数；填充路径在 ops->readraw() 前后放锁，避免持锁做 ATAPI 同步 PIO。 */
static struct spinlock pc_lock;

/* A read-only backend with a stable identity may use its v_key. */
/* EN: Returns 0 for a null vnode, else the vnode's stable v_key when set (immutable extents) or its v_id for tmpfs, the key used by the hash and LRU. */
/* 中文：空 vnode 返回 0；否则优先返回已设置的稳定 v_key（不可变数据区），tmpfs 则返回 v_id，作为哈希与 LRU 使用的键。 */
unsigned
pagecache_key(struct vnode *vp)
{

	if (vp == 0)
		return (0);
	if (vp->v_key != 0)
		return (vp->v_key);
	return (vp->v_id);		/* tmpfs: tied to this vnode */
}

/* EN: Computes the hash bucket index as (key * 2654435761 + pindex) % PCHASH. */
/* 中文：按 (key * 2654435761 + pindex) % PCHASH 计算哈希桶下标。 */
static unsigned
pc_hashfn(unsigned key, unsigned pindex)
{

	return ((key * 2654435761u + pindex) % PCHASH);
}

/* ---------------------------------------------------------------- LRU */

/*
 * Every pc_* helper below runs with pc_lock held; they are the "locked"
 * half of the API and never touch a device or the page allocator.
 */
/* 中文：下面所有 pc_* 辅助函数都在持有 pc_lock 时调用，属于已加锁的内部接口，不访问设备与页分配器。 */

/* EN: Unlinks entry i from the doubly linked LRU index list, repairing pc_lru_head/pc_lru_tail, and clears its links and onlru; no-op if it is not on the LRU. */
/* 中文：把条目 i 从双向 LRU 索引链中摘除并修正 pc_lru_head/pc_lru_tail，清空链指针与 onlru；不在 LRU 上则直接返回。 */
static void
pc_lru_del(int i)
{
	struct pcent *e = &pcs[i];

	if (!e->onlru)
		return;
	if (e->lprev >= 0)
		pcs[e->lprev].lnext = e->lnext;
	else if (pc_lru_head == i)
		pc_lru_head = e->lnext;
	if (e->lnext >= 0)
		pcs[e->lnext].lprev = e->lprev;
	else if (pc_lru_tail == i)
		pc_lru_tail = e->lprev;
	e->lprev = e->lnext = -1;
	e->onlru = 0;
}

/* EN: Appends entry i to the tail of the LRU list (most recently used end), fixing the head when the list was empty and setting onlru; no-op if already queued. */
/* 中文：把条目 i 追加到 LRU 链尾（最近使用端），链表为空时同步更新表头并置 onlru；已在链上则不重复插入。 */
static void
pc_lru_add(int i)
{
	struct pcent *e = &pcs[i];

	if (e->onlru)
		return;
	e->lprev = pc_lru_tail;
	e->lnext = -1;
	if (pc_lru_tail >= 0)
		pcs[pc_lru_tail].lnext = i;
	else
		pc_lru_head = i;
	pc_lru_tail = i;
	e->onlru = 1;
}

/* ---------------------------------------------------------------- hash */

/* EN: Walks the hash bucket chain for (key, pindex) and returns the index of the first used entry whose key and page index match, or -1 when absent. */
/* 中文：沿 (key, pindex) 对应的哈希桶链查找，返回首个 used 且 key 与页索引均匹配的条目下标，未命中返回 -1。 */
static int
pc_find(unsigned key, unsigned pindex)
{
	int i;

	for (i = pc_hash[pc_hashfn(key, pindex)]; i >= 0; i = pcs[i].hnext)
		if (pcs[i].used && pcs[i].key == key &&
		    pcs[i].pindex == pindex)
			return (i);
	return (-1);
}

/* EN: Unlinks entry i from its hash bucket by chasing the chain pointers, so later pc_find() calls for that key and pindex no longer see it. */
/* 中文：沿桶链指针把条目 i 从其哈希桶中摘除，使后续对该 key 与页索引的 pc_find() 不再命中。 */
static void
pc_hash_remove(int i)
{
	int b = pc_hashfn(pcs[i].key, pcs[i].pindex);
	int *pp = &pc_hash[b];

	while (*pp >= 0) {
		if (*pp == i) {
			*pp = pcs[i].hnext;
			pcs[i].hnext = -1;
			return;
		}
		pp = &pcs[*pp].hnext;
	}
}

/* EN: Pushes entry i onto the head of the bucket selected by pc_hashfn() from its key and pindex, making it visible to pc_find(). */
/* 中文：按条目的 key 与 pindex 经 pc_hashfn() 定位桶，把条目 i 头插入该桶，使其可被 pc_find() 查到。 */
static void
pc_hash_insert(int i)
{
	int b = pc_hashfn(pcs[i].key, pcs[i].pindex);

	pcs[i].hnext = pc_hash[b];
	pc_hash[b] = i;
}

/* EN: If entry i is used, unhooks it from the hash, clears used/refs/key/pindex and puts it back on the LRU; the mapped page itself is kept for reuse. */
/* 中文：若条目 i 正在使用，则将其从哈希表摘除、清空 used/refs/key/pindex 并重新挂回 LRU；已映射的页本身保留以便复用。 */
static void
pc_release(int i)
{
	struct pcent *e = &pcs[i];

	if (!e->used)
		return;
	pc_hash_remove(i);
	e->used = 0;
	e->refs = 0;
	e->key = e->pindex = 0;
	pc_lru_add(i);
}

/*
 * Take the least recently used evictable entry.  Entries with refs > 0 are
 * mapped into a process (or being copied) and are not on the LRU at all, so
 * they can never be chosen here: returning 0 means "cache full".
 */
/* EN: Takes the LRU head entry for reuse, unlinking it and clearing refs/key/pindex while bumping pc_evicts if it held a cached page; returns -1 when nothing is evictable. */
/* 中文：取 LRU 表头条目复用：先摘链并清空 refs/key/pindex，若原本缓存着页则累加 pc_evicts；无页可淘汰时返回 -1。 */
static int
pc_take(void)
{
	int i;

	i = pc_lru_head;
	if (i < 0)
		return (-1);
	pc_lru_del(i);
	if (pcs[i].used) {
		pc_hash_remove(i);
		pcs[i].used = 0;
		pc_evicts++;
	}
	pcs[i].refs = 0;
	pcs[i].key = pcs[i].pindex = 0;
	return (i);
}

/* ---------------------------------------------------------------- init */

/* EN: Idempotent setup: clears hash buckets and LRU, allocates and kernel-maps one page per NPCACHE entry at PCACHE_VA, queues every entry on the LRU and zeroes all counters. */
/* 中文：幂等的一次性初始化：清空哈希桶与 LRU，为 NPCACHE 个条目各分配一个物理页并映射到 PCACHE_VA，把全部条目挂入 LRU，并将各计数清零。 */
void
pagecache_init(void)
{
	int i;

	if (pc_inited)
		return;
	/*
	 * Runs from vfs_init() while the machine is still uniprocessor
	 * (before smp_init() starts the APs), so the table needs no lock
	 * here; pc_inited is raised last so a late caller never sees a
	 * half-built cache.
	 */
	/* 中文：本函数由 vfs_init() 在单 CPU 阶段调用（smp_init() 尚未启动 AP），因此无需加锁；pc_inited 最后置位，避免其它 CPU 看到半成品。 */
	for (i = 0; i < PCHASH; i++)
		pc_hash[i] = -1;
	pc_lru_head = pc_lru_tail = -1;
	for (i = 0; i < NPCACHE; i++) {
		struct pcent *e = &pcs[i];

		bzero(e, sizeof(*e));
		e->pa = vfs_page_alloc();
		e->va = (unsigned char *)(PCACHE_VA + i * PAGE_SIZE);
		pmap_map((vm_offset_t)e->va, (vm_offset_t)e->pa,
		    (vm_offset_t)e->pa + PAGE_SIZE,
		    VM_PROT_READ | VM_PROT_WRITE);
		e->hnext = -1;
		e->lprev = e->lnext = -1;
		e->onlru = 0;
		pc_lru_add(i);
	}
	pc_hits = pc_misses = pc_fills = pc_evicts = 0;
	pc_inited = 1;
}

/* ---------------------------------------------------------------- API */

/*
 * Get file page `pindex' of vp, filling it from the backend on a miss.
 * Returns its physical address and stores the kernel VA in *vap; the page
 * comes back referenced, so the caller must pagecache_unref() it.
 */
/* EN: Fetches page pindex of vp: hit bumps pc_hits and refs; miss zeroes an LRU page, fills it via ops->readraw and hashes it; returns pa and *vap, 0 when full (schedules SOFT_RECLAIM). */
/* 中文：查找 vp 的页 pindex：命中则累加 pc_hits 并增加引用，未命中则从 LRU 取页清零、经 ops->readraw 填充、插入哈希并累加 misses/fills；返回物理地址并置 *vap，缓存满时调度 SOFT_RECLAIM 并返回 0。 */
unsigned
pagecache_get(struct vnode *vp, unsigned pindex, unsigned char **vap)
{
	struct pcent *e;
	unsigned key, mode = 0, size = 0, start, n;
	int i, rv;

	if (!pc_inited)
		pagecache_init();
	if (vp == 0 || vap == 0)
		return (0);
	key = pagecache_key(vp);
	if (key == 0)
		return (0);
	spin_lock(&pc_lock);
	i = pc_find(key, pindex);
	if (i >= 0) {
		e = &pcs[i];
		pc_hits++;
		pc_lru_del(i);
		e->refs++;
		*vap = e->va;
		spin_unlock(&pc_lock);
		return (e->pa);
	}
	i = pc_take();
	if (i < 0) {
		spin_unlock(&pc_lock);
		/* S21: ask the softirq thread to reclaim in its own context */
		softint_schedule(SOFT_RECLAIM);
		return (0);
	}
	/*
	 * Take ownership of the entry for the fill: it is off the hash and
	 * off the LRU with refs=1, so pc_find() cannot see it, pc_take()
	 * cannot recycle it and purge skips it (refs != 0).  The read below
	 * therefore runs with no lock held.
	 */
	/* 中文：填充期间独占该条目：它已脱离哈希与 LRU 且 refs=1，pc_find() 看不到、pc_take() 拿不走、purge 会跳过，故下面的读操作无需持锁。 */
	e = &pcs[i];
	e->used = 1;
	e->key = key;
	e->pindex = pindex;
	e->refs = 1;
	spin_unlock(&pc_lock);

	bzero(e->va, PAGE_SIZE);
	if (vp->ops->getattr)
		vp->ops->getattr(vp, &mode, &size);
	start = pindex * PAGE_SIZE;
	n = (size > start) ? (size - start) : 0;
	if (n > PAGE_SIZE)
		n = PAGE_SIZE;
	rv = 0;
	if (n > 0 && vp->ops->readraw != 0)
		rv = vp->ops->readraw(vp, start, e->va, n);

	spin_lock(&pc_lock);
	if (rv < 0) {
		e->used = 0;
		e->refs = 0;
		e->key = e->pindex = 0;
		pc_lru_add(i);
		spin_unlock(&pc_lock);
		return (0);
	}
	pc_hash_insert(i);
	pc_misses++;
	pc_fills++;
	*vap = e->va;
	spin_unlock(&pc_lock);
	return (e->pa);
}

/*
 * Copy len bytes at file offset off through the page cache.  Returns the
 * number of bytes copied, or -ENOMEM if the cache has no unreferenced page.
 */
/* EN: Copies len bytes at offset off through the page cache, clamped to st_size, calling pagecache_get() and pagecache_unref_key(); returns bytes copied, or -ENOMEM if no page is free. */
/* 中文：把 vp 在偏移 off 处的 len 字节经页缓存复制到 buf，先用 ops->getattr 按 st_size 截断，逐页调用 pagecache_get() 与 pagecache_unref_key()；返回复制字节数，首片页即不可得时返回 -ENOMEM。 */
int
pagecache_read(struct vnode *vp, unsigned off, void *buf, unsigned len)
{
	unsigned done = 0, key, mode = 0, size = 0;
	char *p = (char *)buf;

	/*
	 * Clamp to EOF here: the page cache zero-fills the tail of the last
	 * page (mmap relies on that), but read(2) must stop at st_size or a
	 * cat would loop forever on zero bytes.
	 */
	if (vp->ops->getattr)
		vp->ops->getattr(vp, &mode, &size);
	if (off >= size)
		return (0);
	if (len > size - off)
		len = size - off;
	key = pagecache_key(vp);
	while (done < len) {
		unsigned pindex = (off + done) / PAGE_SIZE;
		unsigned poff = (off + done) % PAGE_SIZE;
		unsigned n = PAGE_SIZE - poff;
		unsigned char *va;
		unsigned pa;

		if (n > len - done)
			n = len - done;
		pa = pagecache_get(vp, pindex, &va);
		if (pa == 0)
			return (done > 0 ? (int)done : -ENOMEM);
		bcopy(va + poff, p + done, n);
		pagecache_unref_key(key, pindex);
		done += n;
	}
	return ((int)done);
}

/* EN: Drops one reference on the (key, pindex) entry and re-queues it on the LRU once refs reaches zero; returns 1 if found, 0 if uninitialised, key 0 or not cached. */
/* 中文：释放 (key, pindex) 条目的一次引用，refs 归零时重新挂回 LRU；找到返回 1，未初始化、key 为 0 或未缓存则返回 0。 */
unsigned
pagecache_unref_key(unsigned key, unsigned pindex)
{
	int i;

	if (!pc_inited || key == 0)
		return (0);
	spin_lock(&pc_lock);
	i = pc_find(key, pindex);
	if (i < 0) {
		spin_unlock(&pc_lock);
		return (0);
	}
	if (pcs[i].refs > 0) {
		pcs[i].refs--;
		if (pcs[i].refs == 0)
			pc_lru_add(i);
	}
	spin_unlock(&pc_lock);
	return (1);
}

/* EN: Thin wrapper over pagecache_unref_key() that derives the key from vp with pagecache_key() and returns 0 for a null vnode. */
/* 中文：对 pagecache_unref_key() 的简单封装：用 pagecache_key() 从 vp 求出键，vp 为空时返回 0。 */
unsigned
pagecache_unref(struct vnode *vp, unsigned pindex)
{

	return (vp ? pagecache_unref_key(pagecache_key(vp), pindex) : 0);
}

/*
 * Drop all unreferenced pages of a vnode.  Referenced pages (an active
 * mmap) stay: munmap()/exit drops the reference later.  Only called for
 * mutable (tmpfs) vnodes; ISO9660 pages are immutable and keyed by LBA.
 */
/* EN: Releases every used, unreferenced page whose key equals vp->v_id; returns early for a null vnode or an immutable vnode with v_key set, and leaves referenced (mapped) pages alone. */
/* 中文：释放 key 等于 vp->v_id 的所有 used 且引用计数为 0 的页；vp 为空或 v_key 非 0（不可变）时直接返回，仍被映射引用的页保持不动。 */
void
pagecache_purge_vp(struct vnode *vp)
{
	unsigned key;
	int i;

	if (!pc_inited || vp == 0)
		return;
	if (vp->v_key != 0)
		return;			/* immutable: keep the pages */
	key = vp->v_id;
	if (key == 0)
		return;
	spin_lock(&pc_lock);
	for (i = 0; i < NPCACHE; i++)
		if (pcs[i].used && pcs[i].key == key && pcs[i].refs == 0)
			pc_release(i);
	spin_unlock(&pc_lock);
}

/* EN: Counts used entries and reports NPCACHE as total plus the accumulated hit, miss, fill and evict counters through the six output pointers. */
/* 中文：统计 used 条目数，并通过六个输出指针返回总条目数 NPCACHE 以及累计的命中、缺失、填充、淘汰计数。 */
void
pagecache_stats(unsigned *total, unsigned *used, unsigned *hits,
    unsigned *misses, unsigned *fills, unsigned *evicts)
{
	int i, nused = 0;

	spin_lock(&pc_lock);
	for (i = 0; i < NPCACHE; i++)
		nused += pcs[i].used;
	*total = NPCACHE;
	*used = nused;
	*hits = pc_hits;
	*misses = pc_misses;
	*fills = pc_fills;
	*evicts = pc_evicts;
	spin_unlock(&pc_lock);
}

/*
 * S21: drop up to n unreferenced cached pages.  Called by the softirq
 * thread's reclaim handler, not by the allocating thread.
 */
/* EN: Evicts up to n unreferenced cached pages by repeatedly calling pc_take() and returns how many were evicted; the eviction count is already booked in pc_evicts by pc_take(). */
/* 中文：反复调用 pc_take() 最多淘汰 n 个未被引用的缓存页并返回实际淘汰数；淘汰计数已由 pc_take() 记入 pc_evicts。 */
int
pagecache_reclaim(unsigned n)
{
	int i, count = 0;

	if (!pc_inited)
		return (0);
	spin_lock(&pc_lock);
	while (count < (int)n) {
		i = pc_take();
		if (i < 0)
			break;
		count++;
	}
	spin_unlock(&pc_lock);
	if (count > 0)
		pc_evicts += 0;		/* pc_take already counted */
	return (count);
}
