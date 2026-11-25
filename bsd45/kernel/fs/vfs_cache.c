/*
 * vfs_cache.c - S17: the directory name cache (4.5BSD vfs_cache.c's role).
 *
 * The 4.5BSD file keys entries on (dvp->v_id, name) and holds a reference on
 * the result.  This port has a LIFO vnode pool, so the same v_id trick is
 * used here: vn_alloc() hands out a fresh v_id and vn_put() clears it when
 * the slot is released, which also purges everything that pointed at it.
 *
 * The cache deliberately does NOT hold a vnode reference: a cache entry is a
 * hint.  namecache_lookup() validates vp->v_id and only then bumps vp->ref,
 * so a stale entry can never resurrect a freed vnode slot.  Lookup results
 * are entered by vfs_namei(); create/remove/rename purge the affected name.
 *
 * Layer rule: this file only touches struct vnode (its v_id, ref) and the
 * vnode pool's id allocator, never the process table or a backend.
 */

#include <sys/param.h>
#include <sys/systm.h>

#include "vfs.h"

extern void bcopy(const void *, void *, unsigned);
extern void bzero(void *, unsigned);
extern int strcmp(const char *, const char *);
extern unsigned strlen(const char *);

#define	NCACHE		64
#define	NCHASH		32
#define	NC_NAMELEN	48

struct ncent {
	int		used;
	unsigned	dv_id;
	unsigned	v_id;
	char		name[NC_NAMELEN];
	struct vnode	*vp;
	int		hnext;
	int		lprev, lnext;	/* LRU of live entries */
};

static struct ncent nc[NCACHE];
static int nc_hash[NCHASH];
static int nc_lru_head, nc_lru_tail;
static int nc_inited;

static unsigned nc_hits, nc_misses;

/* EN: Hashes a directory v_id and entry name into a name-cache bucket: starts at dv_id*31, folds each name byte as h*33+c, and returns the sum modulo NCHASH. */
/* 中文：把目录 v_id 与名字散列到名字缓存桶：初值为 dv_id*31，逐字节做 h*33+字符 折叠，最后对 NCHASH 取模返回。 */
static unsigned
nc_hashfn(unsigned dv_id, const char *name)
{
	unsigned h = dv_id * 31;

	while (*name)
		h = h * 33 + (unsigned char)*name++;
	return (h % NCHASH);
}

/* ---------------------------------------------------------------- LRU */

/* EN: Unlinks entry i from the doubly linked LRU list of live entries, fixing nc_lru_head and nc_lru_tail when it is an end, then sets both its links to -1. */
/* 中文：把序号 i 的表项从活跃项 LRU 双向链表上摘除，若位于表头或表尾则修正 nc_lru_head/nc_lru_tail，随后把它的两个链接置 -1。 */
static void
nc_lru_remove(int i)
{
	struct ncent *e = &nc[i];

	if (e->lprev >= 0)
		nc[e->lprev].lnext = e->lnext;
	else if (nc_lru_head == i)
		nc_lru_head = e->lnext;
	if (e->lnext >= 0)
		nc[e->lnext].lprev = e->lprev;
	else if (nc_lru_tail == i)
		nc_lru_tail = e->lprev;
	e->lprev = e->lnext = -1;
}

/* EN: Appends entry i to the LRU tail, i.e. the most recently used end: links it after nc_lru_tail and makes it the new tail, or sets it as head when the list was empty. */
/* 中文：把序号 i 的表项挂到 LRU 链尾（最近使用端）：链接到 nc_lru_tail 之后并成为新表尾，链表原为空时同时置为表头。 */
static void
nc_lru_append(int i)
{
	struct ncent *e = &nc[i];

	e->lprev = nc_lru_tail;
	e->lnext = -1;
	if (nc_lru_tail >= 0)
		nc[nc_lru_tail].lnext = i;
	else
		nc_lru_head = i;
	nc_lru_tail = i;
}

/* ---------------------------------------------------------------- hash */

/* EN: Walks the hash bucket for (dv_id, name) and returns the index of the first used entry whose dv_id and name match exactly, or -1 when none does. */
/* 中文：在 (dv_id, name) 对应的哈希桶链上遍历，返回第一个 used 置位且 dv_id 与名字完全匹配的表项下标，都不匹配则返回 -1。 */
static int
nc_find(unsigned dv_id, const char *name)
{
	int i;

	for (i = nc_hash[nc_hashfn(dv_id, name)]; i >= 0;
	    i = nc[i].hnext)
		if (nc[i].used && nc[i].dv_id == dv_id &&
		    strcmp(nc[i].name, name) == 0)
			return (i);
	return (-1);
}

/* EN: Unlinks entry i from its hash bucket: recomputes the bucket, walks the hnext chain, clears nc[i].hnext to -1, and silently does nothing when not found. */
/* 中文：把序号 i 从哈希桶中摘除：重新算出桶号，沿 hnext 链遍历，把 nc[i].hnext 清为 -1；链上找不到时静默返回。 */
static void
nc_hash_remove(int i)
{
	int b = nc_hashfn(nc[i].dv_id, nc[i].name);
	int *pp = &nc_hash[b];

	while (*pp >= 0) {
		if (*pp == i) {
			*pp = nc[i].hnext;
			nc[i].hnext = -1;
			return;
		}
		pp = &nc[*pp].hnext;
	}
}

/* EN: Inserts entry i at the head of the hash bucket chosen by its dv_id and name, saving the old bucket head in nc[i].hnext. */
/* 中文：按表项的 dv_id 与名字选定哈希桶，把序号 i 插入该桶链首，并把原来的桶首存入 nc[i].hnext。 */
static void
nc_hash_insert(int i)
{
	int b = nc_hashfn(nc[i].dv_id, nc[i].name);

	nc[i].hnext = nc_hash[b];
	nc_hash[b] = i;
}

/* EN: Frees a cache slot: returns early if unused, otherwise unlinks it from the LRU and hash chains and clears used, vp, dv_id, v_id and the stored name; vnode refs are untouched. */
/* 中文：释放一个缓存表项：未使用则直接返回，否则从 LRU 链与哈希链上摘除，并清空 used、vp、dv_id、v_id 和名字；不触碰 vnode 引用计数。 */
static void
nc_drop(int i)
{
	struct ncent *e = &nc[i];

	if (!e->used)
		return;
	nc_lru_remove(i);
	nc_hash_remove(i);
	e->used = 0;
	e->vp = 0;
	e->dv_id = e->v_id = 0;
	e->name[0] = 0;
}

/* ---------------------------------------------------------------- API */

/* EN: One-time, nc_inited-guarded setup: sets all NCHASH buckets to -1, resets the LRU head/tail, zeroes each NCACHE slot with links -1, and clears hit and miss counters. */
/* 中文：由 nc_inited 保证只执行一次的初始化：把 NCHASH 个哈希桶置 -1，复位 LRU 头尾，逐个清零 NCACHE 个表项并令链接为 -1，同时清零命中与未命中计数。 */
void
namecache_init(void)
{
	int i;

	if (nc_inited)
		return;
	nc_inited = 1;
	for (i = 0; i < NCHASH; i++)
		nc_hash[i] = -1;
	nc_lru_head = nc_lru_tail = -1;
	for (i = 0; i < NCACHE; i++) {
		bzero(&nc[i], sizeof(nc[i]));
		nc[i].hnext = -1;
		nc[i].lprev = nc[i].lnext = -1;
	}
	nc_hits = nc_misses = 0;
}

/*
 * Return a referenced vnode for (dv, name), or 0 on a miss.  A stale entry
 * (vnode slot freed/reused) is dropped on the spot.
 */
/* EN: Returns a referenced vnode for (dv, name): validates the cached v_id, moves the entry to the LRU tail and bumps vp->ref; stale or missing entries are dropped and 0 returned. */
/* 中文：按 (dv, name) 返回已加引用的 vnode：校验缓存中的 v_id，把表项移到 LRU 尾并递增 vp->ref；表项过期或不存在则删除并返回 0。 */
struct vnode *
namecache_lookup(struct vnode *dv, const char *name)
{
	int i;
	struct vnode *vp;

	if (!nc_inited || dv == 0 || name == 0 || name[0] == 0 ||
	    strlen(name) >= NC_NAMELEN)
		return (0);
	i = nc_find(dv->v_id, name);
	if (i < 0) {
		nc_misses++;
		return (0);
	}
	vp = nc[i].vp;
	if (vp == 0 || vp->v_id == 0 || vp->v_id != nc[i].v_id) {
		/* the cached vnode was recycled: this is a miss */
		nc_drop(i);
		nc_misses++;
		return (0);
	}
	nc_hits++;
	nc_lru_remove(i);
	nc_lru_append(i);
	vn_ref(vp);
	return (vp);
}

/* EN: Maps (dv, name) to vp: refreshes an existing entry in place, else takes a free slot or evicts the LRU head, then hashes and appends it; names of NC_NAMELEN or more are ignored. */
/* 中文：把 (dv, name) 映射到 vp：已有表项则原地刷新，否则取空闲槽或淘汰 LRU 表头，再插入哈希链与 LRU 尾；名字长度达到 NC_NAMELEN 则忽略。 */
void
namecache_enter(struct vnode *dv, const char *name, struct vnode *vp)
{
	int i, n, len;

	if (!nc_inited || dv == 0 || vp == 0 || name == 0 || name[0] == 0)
		return;
	len = strlen(name);
	if (len >= NC_NAMELEN)
		return;
	if ((i = nc_find(dv->v_id, name)) >= 0) {
		nc[i].vp = vp;
		nc[i].v_id = vp->v_id;
		nc_lru_remove(i);
		nc_lru_append(i);
		return;
	}
	/* free slot first, then the oldest live entry */
	for (i = 0; i < NCACHE; i++)
		if (!nc[i].used)
			break;
	if (i == NCACHE) {
		i = nc_lru_head;
		if (i < 0)
			return;
		nc_drop(i);
	}
	n = i;
	nc[n].used = 1;
	nc[n].dv_id = dv->v_id;
	nc[n].v_id = vp->v_id;
	nc[n].vp = vp;
	bcopy(name, nc[n].name, len);
	nc[n].name[len] = 0;
	nc_hash_insert(n);
	nc_lru_append(n);
}

/* EN: Drops the one cache entry matching directory v_id and name via nc_drop; does nothing when the cache is uninitialized or the name is not cached. */
/* 中文：通过 nc_drop 删除与目录 v_id 及名字匹配的那一个缓存表项；缓存未初始化或该名字未被缓存时什么都不做。 */
void
namecache_purge(struct vnode *dv, const char *name)
{
	int i;

	if (!nc_inited || dv == 0 || name == 0)
		return;
	i = nc_find(dv->v_id, name);
	if (i >= 0)
		nc_drop(i);
}

/* EN: Scans all NCACHE slots and drops every entry that points at vp by pointer or matches its v_id, so a recycled vnode leaves no stale name hints. */
/* 中文：遍历全部 NCACHE 个表项，删除所有以指针指向 vp 或 v_id 与之相符的条目，使被回收的 vnode 不残留过期名字提示。 */
void
namecache_purge_vp(struct vnode *vp)
{
	int i;

	if (!nc_inited || vp == 0)
		return;
	for (i = 0; i < NCACHE; i++)
		if (nc[i].used && (nc[i].vp == vp ||
		    (vp->v_id != 0 && nc[i].v_id == vp->v_id)))
			nc_drop(i);
}

/* EN: Reports NCACHE as total, counts slots with used set, and returns the accumulated nc_hits and nc_misses counters; no locking is performed. */
/* 中文：返回总数 NCACHE、统计 used 置位的已用槽数，并给出累计的 nc_hits 与 nc_misses 计数；过程中不加锁。 */
void
namecache_stats(unsigned *total, unsigned *used, unsigned *hits,
    unsigned *misses)
{
	int i, nused = 0;

	for (i = 0; i < NCACHE; i++)
		nused += nc[i].used;
	*total = NCACHE;
	*used = nused;
	*hits = nc_hits;
	*misses = nc_misses;
}
