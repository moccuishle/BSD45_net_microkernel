/*
 * vfs.c - the vnode layer: vnode pool, namei(), mount table.
 *
 * Backends live behind vnodeops:
 *	ISO9660  (this file, read-only root, (lba, size) in the vnode)
 *	tmpfs    (tmpfs.c, writable /tmp, one page per file)
 * Keep this file free of backend internals: it may only use the primitives
 * listed in docs/kernel-boundary.md (S100.1).
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/smp.h>

#include "atapi.h"
#include "isofs.h"
#include "ffs/ffs.h"
#include "jlfs/jlfs.h"
#include "vfs.h"

extern void bcopy(const void *, void *, unsigned);
extern void bzero(void *, unsigned);
extern int strcmp(const char *, const char *);
extern int strncmp(const char *, const char *, unsigned);
extern char *strchr(const char *, int);
extern unsigned strlen(const char *);

#define	ISO_SECTOR	2048

struct vnodeops iso_vnodeops;

static struct vnode iso_rootv;
struct vnode *rootvnode = &iso_rootv;

/* EN: Which filesystem backs "/".  glue.c sets this before vfs_init() based on whether we booted from the ISO or the installed disk. */
static int vfs_root_src = VFS_ROOT_ISO;

/*
 * P2: vnode pool lock.  It covers slot allocation, refcounts, v_id and the
 * directory-retention ring.  Backend lookups/reads still run under the BKL:
 * they may block on ATAPI, so they must never be called with this spinlock
 * held.  Lock order: vfs_lock -> pc_lock (vn_free -> pagecache_purge_vp).
 */
static struct spinlock vfs_lock;

/* 中文：设置作为根文件系统来源的后端（ISO/tmpfs/FFS），由 glue.c 在 vfs_init() 前调用 */
/* EN: Set which filesystem backend backs the root (ISO/tmpfs/FFS), called by glue.c before vfs_init() */
void
vfs_set_root_source(int source)
{

	vfs_root_src = source;
}

/* 中文：返回当前配置的根文件系统来源后端 */
/* EN: Return the currently configured root filesystem backend */
int
vfs_root_source(void)
{

	return (vfs_root_src);
}

/* ---------------------------------------------------------------- helpers */

#define	VNODES		96
static struct vnode vnode_pool[VNODES];
static int vnode_used[VNODES];
static int vnode_free[VNODES];		/* LIFO of reusable slots */
static int vnode_nfree;
static unsigned vn_alloc_total, vn_reuse_total;
static unsigned vn_next_id = 1;

/*
 * S17: a tiny vnode cache for directories.  Files come and go with every
 * open(), but keeping the handful of directory vnodes alive is what makes
 * the name cache useful: root/bin/sbin lookups repeat on every exec and a
 * retained directory keeps its name-cache entries valid.
 */
#define	NDCACHE		8
static struct vnode *dir_cache[NDCACHE];
static int vnode_dcached[VNODES];
static int dir_cache_next;

static void vn_free_locked(struct vnode *vp);

/*
 * S17: caches key on v_id, never on the vnode pointer, because slots are
 * recycled.  vn_newid() is also used by the two statically allocated mount
 * roots (ISO and tmpfs), which do not come from vn_alloc().
 */
/* EN: Returns the current vn_next_id and then increments it, handing out a monotonically increasing unique vnode id; the read-modify-write is un locked. */
/* 中文：返回 vn_next_id 的当前值后自增，发放单调递增的唯一 vnode id；该读-改-写过程未加锁。 */
unsigned
vn_newid(void)
{
	unsigned id;

	spin_lock(&vfs_lock);
	id = vn_next_id++;
	spin_unlock(&vfs_lock);
	return (id);
}

/* EN: Increment a vnode's reference count under the pool lock. */
/* 中文：在 vnode 池锁保护下递增引用计数。 */
void
vn_ref(struct vnode *vp)
{

	if (vp == 0)
		return;
	spin_lock(&vfs_lock);
	vp->ref++;
	spin_unlock(&vfs_lock);
}

/* EN: Boot reset: rebuilds the vnode_free[] LIFO with indices VNODES-1..0, clears vnode_used[]/vnode_dcached[], zeroes dir_cache[] and resets dir_cache_next. */
/* 中文：启动时重置 vnode 记账：从 VNODES-1 递减到 0 重建 vnode_free[] 后进先出表，清除 vnode_used[] 与 vnode_dcached[]，清零 dir_cache[] 并复位 dir_cache_next。 */
static void
vn_init(void)
{
	int i;

	vnode_nfree = 0;
	for (i = VNODES - 1; i >= 0; i--) {
		vnode_used[i] = 0;
		vnode_dcached[i] = 0;
		vnode_free[vnode_nfree++] = i;
	}
	for (i = 0; i < NDCACHE; i++)
		dir_cache[i] = 0;
	dir_cache_next = 0;
}

/*
 * Release a vnode slot: drop everything that keys on it, then make the slot
 * reusable.  vn_put() calls this only when the vnode is not an actively
 * used directory.
 */
/* EN: Frees a slot: ignores NULL, iso_rootv, the tmpfs root and bad indices, purges name and page cache entries, clears ref/v_id/v_key and vnode_used/vnode_dcached. */
/* 中文：把 vnode 槽归还池中：忽略 NULL、iso_rootv、tmpfs 根以及越界下标，然后清除其名字缓存与页缓存项，将 ref/v_id/v_key 与 vnode_used/vnode_dcached 清零。 */
static void
vn_free_locked(struct vnode *vp)
{
	int i;

	if (vp == 0 || vp == &iso_rootv || tmpfs_is_root(vp) || ffs_is_root(vp) || jlfs_is_root(vp))
		return;
	i = vp - vnode_pool;
	if (i < 0 || i >= VNODES)
		return;
	namecache_purge_vp(vp);
	pagecache_purge_vp(vp);
	vp->ref = 0;
	vp->v_id = 0;
	vp->v_key = 0;
	vnode_used[i] = 0;
	vnode_dcached[i] = 0;
}

/*
 * Retain a directory vnode with ref == 0.  The slot stays busy, so the name
 * cache entries under it remain valid; the small ring evicts the oldest
 * directory once all slots are taken.
 */
/* EN: Keeps an unreferenced dir vnode in the NDCACHE ring so its name cache stays valid: takes a free slot, else vn_free()s the oldest unreferenced entry, else vn_free()s vp itself. */
/* 中文：把未引用的目录 vnode 保留在 NDCACHE 环中以免其名字缓存项失效：有空槽则占空槽，否则淘汰最旧的未引用项并用 vn_free() 释放它；若全部被引用则直接 vn_free() vp。 */
static void
dir_cache_keep_locked(struct vnode *vp)
{
	int i = vp - vnode_pool;
	int k, j;

	if (i < 0 || i >= VNODES || vnode_dcached[i])
		return;
	for (k = 0; k < NDCACHE; k++) {
		j = (dir_cache_next + k) % NDCACHE;
		if (dir_cache[j] == 0) {
			dir_cache[j] = vp;
			vnode_dcached[i] = 1;
			dir_cache_next = (j + 1) % NDCACHE;
			return;
		}
	}
	/* all slots busy: evict the oldest unreferenced directory */
	for (k = 0; k < NDCACHE; k++) {
		j = (dir_cache_next + k) % NDCACHE;
		if (dir_cache[j]->ref == 0) {
			struct vnode *old = dir_cache[j];

			vnode_dcached[old - vnode_pool] = 0;
			dir_cache[j] = vp;
			vnode_dcached[i] = 1;
			dir_cache_next = (j + 1) % NDCACHE;
			vn_free_locked(old);
			return;
		}
	}
	/* every retained directory is in use: do not keep this one */
	vn_free_locked(vp);
}

/* EN: Finds the first unused slot, marks it used, bumps vn_alloc_total, zeroes the vnode, sets ref=1, node=-1 and a fresh v_id via vn_newid(); 0 when the pool is full. */
/* 中文：扫描池中第一个空闲槽并标记已用，递增 vn_alloc_total，清零 vnode，设置 ref=1、node=-1 以及由 vn_newid() 取得的新 v_id；VNODES 个槽全忙时返回 0。 */
struct vnode *
vn_alloc(void)
{
	struct vnode *vp = 0;
	int i;

	spin_lock(&vfs_lock);
	for (i = 0; i < VNODES; i++) {
		if (vnode_used[i])
			continue;
		vnode_used[i] = 1;
		vn_alloc_total++;
		bzero(&vnode_pool[i], sizeof(vnode_pool[i]));
		vnode_pool[i].ref = 1;
		vnode_pool[i].node = -1;
		vnode_pool[i].v_id = vn_next_id++;
		vp = &vnode_pool[i];
		break;
	}
	spin_unlock(&vfs_lock);
	return (vp);
}

/* EN: Drops a reference (ignoring NULL, iso_rootv, tmpfs root); at ref 0 it clears ref and sends VDIR with nonzero v_key (ISO9660 dirs) to dir_cache_keep(), others to vn_free(). */
/* 中文：递减一个引用，忽略 NULL、iso_rootv 与 tmpfs 根；ref 降到 0 时清零 ref，并把 v_key 非 0 的 VDIR 节点（ISO9660 目录）交给 dir_cache_keep()，其余交给 vn_free()。 */
void
vn_put(struct vnode *vp)
{

	if (vp == 0 || vp == &iso_rootv || tmpfs_is_root(vp) || ffs_is_root(vp) || jlfs_is_root(vp))
		return;
	spin_lock(&vfs_lock);
	if (--vp->ref <= 0) {
		vp->ref = 0;
		/*
		 * S17: keep read-only (ISO9660) directory vnodes around so the
		 * name cache keeps hitting on the root -> bin -> file walks of
		 * execve().  tmpfs directories are created and removed at run
		 * time and must be released, or every mkdir would pin a slot.
		 * Regular files really are released too (the page cache keeps
		 * their data keyed by LBA for ISO9660).
		 */
		if (vp->type == VDIR && vp->v_key != 0)
			dir_cache_keep_locked(vp);
		else
			vn_free_locked(vp);
	}
	spin_unlock(&vfs_lock);
}

/* EN: Counts used slots by summing vnode_used[], reports VNODES as *vtotal, that sum as *vused and the remainder as *vfree, then lets vfs_page_stats() fill *pnalloc/*pnfree/*pninuse. */
/* 中文：累加 vnode_used[] 统计已用槽数，输出 *vtotal=VNODES、*vused=已用数、*vfree=剩余数，再由 vfs_page_stats() 填写 *pnalloc/*pnfree/*pninuse。 */
void
vfs_stats(unsigned *vtotal, unsigned *vused, unsigned *vfree,
    unsigned *pnalloc, unsigned *pnfree, unsigned *pninuse)
{

	int i, used = 0;

	spin_lock(&vfs_lock);
	for (i = 0; i < VNODES; i++)
		used += vnode_used[i];
	spin_unlock(&vfs_lock);
	*vtotal = VNODES;
	*vused = used;
	*vfree = VNODES - used;
	vfs_page_stats(pnalloc, pnfree, pninuse);
}
/* ---------------------------------------------------------------- ISO9660 */

/* EN: Walks the ISO dir via iso_readdir() from cookie 0 for the name, then vn_alloc()s a vnode (-ENOMEM) with type, ops, lba, size, v_key = 0x80000000|lba; -EIO on error, else -ENOENT. */
/* 中文：用 iso_readdir() 从 cookie 0 遍历 ISO9660 目录直到名字相等，随后 vn_alloc() 分配 vnode（失败返回 -ENOMEM）并填 type、iso_vnodeops、lba、size 及只读页缓存键 v_key = 0x80000000|lba；readdir 出错返回 -EIO，否则 -ENOENT。 */
static int
iso_scan(struct vnode *dv, const char *name, struct vnode **vpp)
{
	char nm[48];
	unsigned lba, size, pos = 0;
	int isdir, rv;

	while ((rv = iso_readdir(dv->lba, dv->size, &pos, nm, &lba, &size,
	    &isdir)) == 0) {
		if (strcmp(nm, name) == 0) {
			struct vnode *vp = vn_alloc();

			if (vp == 0)
				return (-ENOMEM);
			vp->type = isdir ? VDIR : VREG;
			vp->ops = &iso_vnodeops;
			vp->lba = lba;
			vp->size = size;
			/* S17: stable page-cache key; ISO files are immutable */
			vp->v_key = 0x80000000u | (lba & 0x7fffffff);
			*vpp = vp;
			return (0);
		}
	}
	return (rv < 0 ? -EIO : -ENOENT);
}

/* EN: Needs a directory (-ENOTDIR), calls iso_scan(); on -ENOENT with no '.' in name it retries with '.elf' appended when it fits want[48], returning first success or last error. */
/* 中文：要求是目录（否则 -ENOTDIR）并调用 iso_scan()；若返回 -ENOENT 且名字不含 '.'，则在能放入 want[48] 时追加 .elf 再扫一次，返回首次成功或最后的错误。 */
static int
iso_lookup_op(struct vnode *dv, const char *name, struct vnode **vpp)
{
	char want[48];
	int rv;

	if (dv->type != VDIR)
		return (-ENOTDIR);
	rv = iso_scan(dv, name, vpp);
	if (rv == -ENOENT && strchr(name, '.') == 0) {
		unsigned n = strlen(name);

		if (n + 5 < sizeof(want)) {
			bcopy(name, want, n);
			bcopy(".elf", want + n, 5);
			rv = iso_scan(dv, want, vpp);
		}
	}
	return (rv);
}

/* EN: Reports ISO9660 entries uniformly as mode 0040755 for a directory or 0100755 (executable) for a file, copies vp->size into *size and returns 0. */
/* 中文：对 ISO9660 项统一报告权限：目录为 0040755、文件为 0100755（可执行），把 vp->size 写入 *size 并返回 0。 */
static int
iso_getattr_op(struct vnode *vp, unsigned *mode, unsigned *size)
{

	*mode = vp->type == VDIR ? 0040755 : 0100755;
	*size = vp->size;
	return (0);
}

/*
 * S17: read() is layered on the page cache.  iso_readraw_op() is the
 * uncached path the page cache uses to fill a 4K page: it still goes
 * through the buffer cache, so a 4K page costs two cached 2K sectors and
 * sequential access gets readahead.
 */
/* EN: Uncached page fill: -EISDIR for a dir, 0 past EOF, clips len; bread()s 2048-byte sectors with bcopy/brelse and breada()s up to 4 sectors of readahead; -EIO on failure. */
/* 中文：无缓存填页读取：目录返回 -EISDIR，越过 EOF 返回 0，先截断 len；按 2048 字节扇区循环 bread() 并 bcopy/brelse，最多 4 扇区 breada() 预读且受文件长度限制；失败返回 -EIO。 */
static int
iso_readraw_op(struct vnode *vp, unsigned off, void *buf, unsigned len)
{
	unsigned done = 0;
	unsigned nsec = (vp->size + ISO_SECTOR - 1) / ISO_SECTOR;
	char *p = (char *)buf;

	if (vp->type == VDIR)
		return (-EISDIR);
	if (off >= vp->size)
		return (0);
	if (len > vp->size - off)
		len = vp->size - off;
	while (done < len) {
		unsigned pos = off + done;
		unsigned secoff = pos % ISO_SECTOR;
		unsigned n = ISO_SECTOR - secoff;
		unsigned blk = pos / ISO_SECTOR;
		struct buf *bp;

		if (n > len - done)
			n = len - done;
		bp = bread(BDEV_CD, vp->lba + blk);
		if (bp == 0)
			return (-EIO);
		bcopy(buf_data(bp) + secoff, p + done, n);
		brelse(bp);
		done += n;
		/* sequential readahead, clamped to this file's sectors */
		if (blk + 1 < nsec) {
			unsigned ra = nsec - blk - 1;

			if (ra > 4)
				ra = 4;
			breada(BDEV_CD, vp->lba + blk + 1, ra);
		}
	}
	return ((int)done);
}

/* EN: Thin wrapper that returns -EISDIR for a directory vnode and otherwise delegates the read to pagecache_read() on the ISO9660 file vnode. */
/* 中文：薄封装：vnode 为目录时返回 -EISDIR，否则把读取交给 ISO9660 文件 vnode 的 pagecache_read()。 */
static int
iso_read_op(struct vnode *vp, unsigned off, void *buf, unsigned len)
{

	if (vp->type == VDIR)
		return (-EISDIR);
	return (pagecache_read(vp, off, buf, len));
}

/* EN: Rejects non-directories (-ENOTDIR), delegates to iso_readdir() with vp->lba/vp->size passing cookie, name and isdir, maps negative returns to -EIO and passes 0/1 through. */
/* 中文：非目录返回 -ENOTDIR；用 vp->lba/vp->size 调用 iso_readdir() 并透传 cookie、name 与 isdir，负返回值映射为 -EIO，0（有项）和 1（结束）原样返回。 */
static int
iso_readdir_op(struct vnode *vp, unsigned *cookie, char *name, int *isdir)
{
	unsigned lba, size;
	int rv;

	if (vp->type != VDIR)
		return (-ENOTDIR);
	rv = iso_readdir(vp->lba, vp->size, cookie, name, &lba, &size,
	    isdir);
	return (rv < 0 ? -EIO : rv);
}

/* ---------------------------------------------------------------- ops */

struct vnodeops iso_vnodeops = {
	iso_lookup_op, iso_getattr_op, iso_read_op, 0, iso_readdir_op,
	0, 0, 0, 0, 0, 0, iso_readraw_op
};


/*
 * S10.4: compose an absolute path from cwd + in.  This is the ONLY place
 * that knows about the cwd; vfs_namei()/vfs_lookup_parent() keep taking
 * absolute paths exactly as before, so a backend's "dv == my root" identity
 * checks are unaffected.
 */
/* EN: Canonicalizes in into out (outlen bytes), prefixing cwd for relative paths, collapsing //, . and .., trimming a trailing slash; returns 0 or -ENOENT/-ENAMETOOLONG. */
/* 中文：把 in 规范化成绝对路径写入 out（长度 outlen），相对路径先拼接 cwd，折叠 // 、. 和 ..，去掉末尾斜杠；成功返回 0，否则 -ENOENT/-ENAMETOOLONG。 */
int
vfs_absolutize(const char *cwd, const char *in, char *out, unsigned outlen)
{
	unsigned o = 0, i = 0, n, k;

	if (in == 0 || in[0] == 0)
		return (-ENOENT);
	if (in[0] != '/') {
		if (cwd == 0 || cwd[0] != '/')
			return (-ENOENT);
		while (cwd[i] != 0 && o + 1 < outlen)
			out[o++] = cwd[i++];
		i = 0;
	} else {
		if (outlen < 2)
			return (-ENAMETOOLONG);
		out[o++] = '/';
		i = 1;
	}
	while (in[i] != 0) {
		while (in[i] == '/')
			i++;
		if (in[i] == 0)
			break;
		n = 0;
		while (in[i + n] != 0 && in[i + n] != '/')
			n++;
		if (n == 1 && in[i] == '.') {
			i += n;
			continue;
		}
		if (n == 2 && in[i] == '.' && in[i + 1] == '.') {
			while (o > 1 && out[o - 1] != '/')
				o--;
			if (o > 1)
				o--;
			i += n;
			continue;
		}
		if (o + n + 2 > outlen)
			return (-ENAMETOOLONG);
		if (o == 0 || out[o - 1] != '/')
			out[o++] = '/';
		for (k = 0; k < n; k++)
			out[o++] = in[i + k];
		i += n;
	}
	if (o == 0)
		out[o++] = '/';
	/* "/bin/" -> "/bin": vfs_namei() treats an empty component as ENOENT */
	if (o > 1 && out[o - 1] == '/')
		o--;
	out[o] = 0;
	return (0);
}

/* ---------------------------------------------------------------- namei */

static void mount_add(const char *path, struct vnode *root);

/* EN: One-shot VFS setup guarded by a static flag: initializes vnode, name, buffer and page caches, fills iso_rootv as the VDIR ISO9660 root and registers the / and /tmp mounts. */
/* 中文：由静态标志保证只执行一次的 VFS 初始化：建立 vnode/名字/缓冲/页缓存，把 iso_rootv 填成 VDIR 的 ISO9660 根并登记 / 与 /tmp 两个挂载点。 */
void
vfs_init(void)
{
	static int vfs_inited;

	if (vfs_inited)
		return;
	vfs_inited = 1;
	spin_init(&vfs_lock);
	vn_init();
	/*
	 * S17: bring up the three caches before the first namei().  The pages
	 * for the buffer cache and the page cache are allocated once, now, so
	 * they show up in the page accounting as a fixed baseline instead of
	 * growing during the S10 stress loops.
	 */
	namecache_init();
	bio_init();
	pagecache_init();
	tmpfs_init();
	tmpfs_root()->v_id = vn_newid();

	if (vfs_root_src == VFS_ROOT_JLFS && jlfs_mounted()) {
		struct vnode *jv = jlfs_root();
		bzero(jv, sizeof(*jv));
		jv->ref = 1; jv->type = VDIR; jv->ops = &jlfs_vnodeops;
		jv->lba = 2; jv->size = 0; jv->v_id = vn_newid();
		jv->v_key = 0;                 /* mutable */
		rootvnode = jv;
		mount_add("/", jv);
	} else 	if (vfs_root_src == VFS_ROOT_FFS && ffs_mounted()) {
		struct vnode *fr = ffs_root();

		bzero(fr, sizeof(*fr));
		fr->ref = 1;
		fr->type = VDIR;
		fr->ops = &ffs_vnodeops;
		fr->lba = 2;			/* FFS root inode */
		fr->size = 0;
		fr->v_id = vn_newid();
		fr->v_key = 0x40000000u | 2;	/* FFS inode key prefix */
		rootvnode = fr;
		mount_add("/", fr);
	} else if (vfs_root_src == VFS_ROOT_TMPFS) {
		rootvnode = tmpfs_root();
		mount_add("/", tmpfs_root());
	} else {
		bzero(&iso_rootv, sizeof(iso_rootv));
		iso_rootv.ref = 1;
		iso_rootv.type = VDIR;
		iso_rootv.ops = &iso_vnodeops;
		iso_rootv.lba = iso_root();
		iso_rootv.size = iso_root_size();
		iso_rootv.v_id = vn_newid();
		iso_rootv.v_key = 0x80000000u | (iso_rootv.lba & 0x7fffffff);
		rootvnode = &iso_rootv;
		mount_add("/", &iso_rootv);
	}
	mount_add("/tmp", tmpfs_root());
}

/*
 * Resolve an absolute path.  "/tmp" (and anything under it) is a mount of
 * the tmpfs root; everything else lives on the ISO9660 image.
 */
/* EN: Walks an absolute path from rootvnode, mapping /tmp to the tmpfs root and asking the name cache before ops->lookup; returns a referenced vnode in vpp, or a negative errno. */
/* 中文：从 rootvnode 逐段解析绝对路径，/tmp 直接映射到 tmpfs 根，其余先查名字缓存再调 ops->lookup；成功在 vpp 返回带引用的 vnode，失败返回负 errno。 */
int
vfs_namei(const char *path, struct vnode **vpp)
{
	struct vnode *vp = rootvnode;
	const char *p = path;
	int rv;

	while (*p == '/')
		p++;
	if (*p == 0) {
		*vpp = vp;
		return (0);
	}
	for (;;) {
		char name[48];
		const char *slash = p;
		int n;
		struct vnode *next;

		while (*slash && *slash != '/')
			slash++;
		n = slash - p;
		if (n == 0 || n >= (int)sizeof(name))
			return (-ENOENT);
		bcopy(p, name, n);
		name[n] = 0;

		if (vp == rootvnode && strcmp(name, "tmp") == 0)
			next = tmpfs_root();
		else {
			/*
			 * S17: ask the name cache first; on a miss do the
			 * backend lookup and remember it.  A cache hit comes
			 * back already referenced, exactly like a lookup.
			 */
			next = namecache_lookup(vp, name);
			if (next == 0) {
				rv = vp->ops->lookup(vp, name, &next);
				if (rv < 0)
					return (rv);
				namecache_enter(vp, name, next);
			}
		}
		if (vp != rootvnode && !tmpfs_is_root(vp))
			vn_put(vp);
		vp = next;
		if (*slash == 0) {
			*vpp = vp;
			return (0);
		}
		p = slash;
		while (*p == '/')
			p++;
	}
}

/*
 * Split path into the parent directory vnode and the last component, used
 * by creation/removal.
 */
/*
 * S29: kstr path entry points.  Backends still store ASCII names for now, so
 * the conversion happens once here; when the FAT/CNFS backends land they use
 * kstr names natively and this shim disappears.
 */
/* EN: kstr shim: converts the path into a 256-byte ASCII buffer with kstr_to_a, returning -ENAMETOOLONG if it will not fit, then forwards to vfs_namei(). */
/* 中文：kstr 适配层：先用 kstr_to_a 把路径转成 256 字节的 ASCII 缓冲，放不下则返回 -ENAMETOOLONG，然后转发给 vfs_namei()。 */
int
vfs_namei_k(const struct kstr *path, struct vnode **vpp)
{
	char buf[256];

	if (kstr_to_a(path, buf, sizeof(buf)) < 0)
		return (-ENAMETOOLONG);
	return (vfs_namei(buf, vpp));
}

/* EN: kstr shim around vfs_create(): converts path to a 256-byte ASCII buffer (else -ENAMETOOLONG) and forwards isdir and vpp unchanged. */
/* 中文：vfs_create() 的 kstr 适配层：把路径转成 256 字节 ASCII 缓冲（越界返回 -ENAMETOOLONG），再把 isdir 和 vpp 原样传给 vfs_create()。 */
int
vfs_create_k(const struct kstr *path, int isdir, struct vnode **vpp)
{
	char buf[256];

	if (kstr_to_a(path, buf, sizeof(buf)) < 0)
		return (-ENAMETOOLONG);
	return (vfs_create(buf, isdir, vpp));
}

/* EN: kstr shim around vfs_remove(): converts path to a 256-byte ASCII buffer, returning -ENAMETOOLONG on overflow, then removes the file or directory via vfs_remove(). */
/* 中文：vfs_remove() 的 kstr 适配层：把路径转成 256 字节 ASCII 缓冲，溢出时返回 -ENAMETOOLONG，然后调用 vfs_remove() 删除文件或目录。 */
int
vfs_remove_k(const struct kstr *path, int isdir)
{
	char buf[256];

	if (kstr_to_a(path, buf, sizeof(buf)) < 0)
		return (-ENAMETOOLONG);
	return (vfs_remove(buf, isdir));
}

/* EN: Splits path at its last slash, copying the directory part into a 128-byte buffer and the final component (max 48 bytes) into name, then resolves the parent with vfs_namei(). */
/* 中文：在路径最后一个斜杠处切分，把目录部分复制进 128 字节缓冲、末段名字（最多 48 字节）写入 name，再用 vfs_namei() 解析父目录。 */
int
vfs_lookup_parent(const char *path, struct vnode **dvp, char *name)
{
	char dir[128];
	const char *slash = 0, *p = path;
	int n;

	while (*p) {
		if (*p == '/')
			slash = p;
		p++;
	}
	if (slash == 0)
		return (-ENOENT);
	n = slash - path;
	if (n >= (int)sizeof(dir) - 1)
		return (-ENOENT);
	if (n == 0) {
		/* "/name": the parent is the root directory */
		dir[0] = '/';
		dir[1] = 0;
	} else {
		bcopy(path, dir, n);
		dir[n] = 0;
	}
	n = strlen(slash + 1);
	if (n == 0 || n >= 48)
		return (-ENOENT);
	bcopy(slash + 1, name, n + 1);
	return (vfs_namei(dir, dvp));
}

/* EN: Resolves the parent (-EROFS if it lacks create/mkdir), calls mkdir+lookup for dirs or create for files, purges the name-cache entry on success and vn_put()s the parent. */
/* 中文：先用 vfs_lookup_parent() 找父目录（无 create/mkdir 操作则返回 -EROFS），目录走 mkdir+lookup、文件走 create，成功后清除名字缓存项并 vn_put() 父目录。 */
int
vfs_create(const char *path, int isdir, struct vnode **vpp)
{
	struct vnode *dv;
	char name[48];
	int rv;

	rv = vfs_lookup_parent(path, &dv, name);
	if (rv < 0)
		return (rv);
	if (!dv->ops->create && !dv->ops->mkdir) {
		vn_put(dv);
		return (-EROFS);
	}
	if (isdir) {
		rv = dv->ops->mkdir ? dv->ops->mkdir(dv, name) : -EROFS;
		if (rv == 0)
			rv = dv->ops->lookup(dv, name, vpp);
	} else {
		rv = dv->ops->create ? dv->ops->create(dv, name, vpp) :
		    -EROFS;
	}
	if (rv == 0)
		namecache_purge(dv, name);
	vn_put(dv);
	return (rv);
}

/*
 * S17: unlink/rmdir with name-cache invalidation, so a removed file is not
 * found again through a stale positive entry.
 */
/* EN: Resolves the parent, calls rmdir or remove according to isdir (-EROFS when the op is absent), purges the name-cache entry on success, and always vn_put()s the parent vnode. */
/* 中文：解析父目录后按 isdir 调用 rmdir 或 remove（对应操作缺失则返回 -EROFS），成功后清除名字缓存项，最后总是 vn_put() 父目录 vnode。 */
int
vfs_remove(const char *path, int isdir)
{
	struct vnode *dv;
	char name[48];
	int rv;

	rv = vfs_lookup_parent(path, &dv, name);
	if (rv < 0)
		return (rv);
	if (isdir)
		rv = dv->ops->rmdir ? dv->ops->rmdir(dv, name) : -EROFS;
	else
		rv = dv->ops->remove ? dv->ops->remove(dv, name) : -EROFS;
	if (rv == 0)
		namecache_purge(dv, name);
	vn_put(dv);
	return (rv);
}

/*
 * Rename within one directory.  The ISO9660 root is read-only, so only
 * the tmpfs mount can do this; mv(1) falls back to cp+unlink otherwise.
 */
/* EN: Resolves both parents, returns -EXDEV unless they are the same vnode with a rename op; calls it, then on success purges both cached names and vn_put()s both parents. */
/* 中文：解析新旧两个父目录，若二者不是同一 vnode 或缺少 rename 操作则返回 -EXDEV；调用 rename 后在成功时清除新旧两个名字缓存并 vn_put() 两个父目录。 */
int
vfs_rename(const char *oldpath, const char *newpath)
{
	struct vnode *odv, *ndv;
	char oname[48], nname[48];
	int rv;

	rv = vfs_lookup_parent(oldpath, &odv, oname);
	if (rv < 0)
		return (rv);
	rv = vfs_lookup_parent(newpath, &ndv, nname);
	if (rv < 0) {
		vn_put(odv);
		return (rv);
	}
	if (odv != ndv || odv->ops->rename == 0)
		rv = -EXDEV;
	else
		rv = odv->ops->rename(odv, oname, nname);
	if (rv == 0) {
		/* old name is gone, new name may previously have been cached */
		namecache_purge(odv, oname);
		namecache_purge(ndv, nname);
	}
	vn_put(odv);
	vn_put(ndv);
	return (rv);
}
/* the mount table these two mounts are all we have for now */
struct vfs_mount {
	const char	*m_path;
	struct vnode	*m_root;
};
static struct vfs_mount mounts[2];
static int nmounts;

/* EN: Appends a path and root vnode pair to the fixed two-slot mounts[] table and bumps nmounts, silently doing nothing once both slots are taken. */
/* 中文：把路径与根 vnode 追加到只有 2 个槽位的 mounts[] 表并递增 nmounts，表满后静默忽略，不再登记。 */
static void
mount_add(const char *path, struct vnode *root)
{

	if (nmounts < 2) {
		mounts[nmounts].m_path = path;
		mounts[nmounts].m_root = root;
		nmounts++;
	}
}

/* ---- S12: render the mount table for the /dev/mounts pseudo file ---- */

/* EN: Copies string s into buf starting at offset o, stopping once o + 1 reaches len, and returns the advanced offset without writing a terminating NUL. */
/* 中文：从偏移 o 处把字符串 s 复制进 buf，当 o+1 达到 len 时停止，返回新的偏移；不写结尾 NUL。 */
static unsigned
app_str(char *buf, unsigned o, unsigned len, const char *s)
{

	while (*s && o + 1 < len)
		buf[o++] = *s++;
	return (o);
}

/* EN: Renders unsigned v as decimal by filling a small buffer in reverse, appends the digits to buf at offset o while space remains, and returns the advanced offset. */
/* 中文：把无符号数 v 逆序拆成十进制数字暂存到小缓冲，再在空间允许时正序追加到 buf 的 o 偏移处，返回新的偏移。 */
static unsigned
app_num(char *buf, unsigned o, unsigned len, unsigned v)
{
	char num[12];
	int n = 0;

	if (v == 0)
		num[n++] = '0';
	while (v > 0) {
		num[n++] = '0' + (v % 10);
		v /= 10;
	}
	while (n > 0 && o + 1 < len)
		buf[o++] = num[--n];
	return (o);
}

/* EN: Formats every mount for the /dev/mounts pseudo file, one line each with path, tmpfs/iso9660 type, lba, size and rw/ro; NUL-terminates within len and returns the bytes written. */
/* 中文：为 /dev/mounts 伪文件格式化全部挂载点，每行给出路径、tmpfs/iso9660 类型、lba、大小和 rw/ro，在 len 内补 NUL 并返回写入字节数。 */
int
vfs_mount_list(char *buf, unsigned len)
{
	unsigned o = 0;
	int i;

	for (i = 0; i < nmounts; i++) {
		int tmp = tmpfs_is_root(mounts[i].m_root);

		o = app_str(buf, o, len, mounts[i].m_path);
		o = app_str(buf, o, len, tmp ? " tmpfs lba=" : " iso9660 lba=");
		o = app_num(buf, o, len, tmp ? 0 : iso_root());
		o = app_str(buf, o, len, " size=");
		o = app_num(buf, o, len, tmp ? 0 : iso_root_size());
		o = app_str(buf, o, len, tmp ? " rw\n" : " ro\n");
	}
	if (o < len)
		buf[o] = 0;
	return ((int)o);
}

/* ---- S17: render the cache state for the /dev/vmstat pseudo file ---- */

/* EN: Snapshots buffer, name-cache, page-cache and vnode/page stats and renders the multi-line /dev/vmstat pseudo file, NUL-terminating within len and returning the byte count. */
/* 中文：先快照缓冲缓存、名字缓存、页缓存和 vnode/物理页的统计值，再渲染成 /dev/vmstat 伪文件的多行文本，在 len 内补 NUL 并返回字节数。 */
int
vfs_cache_report(char *buf, unsigned len)
{
	unsigned o = 0;
	unsigned bt, bu, bh, bmiss, br, bra, bd;
	unsigned nt, nu, nh, nmiss;
	unsigned pt, pu, ph, pmiss, pfills, pevicts;
	unsigned vt, vu, vf, pa, pfree, pinuse;

	bio_stats(&bt, &bu, &bh, &bmiss, &br, &bra, &bd);
	namecache_stats(&nt, &nu, &nh, &nmiss);
	pagecache_stats(&pt, &pu, &ph, &pmiss, &pfills, &pevicts);
	vfs_stats(&vt, &vu, &vf, &pa, &pfree, &pinuse);

	o = app_str(buf, o, len, "vnode: total ");
	o = app_num(buf, o, len, vt);
	o = app_str(buf, o, len, " used ");
	o = app_num(buf, o, len, vu);
	o = app_str(buf, o, len, " free ");
	o = app_num(buf, o, len, vf);
	o = app_str(buf, o, len, "\n");

	o = app_str(buf, o, len, "buf: total ");
	o = app_num(buf, o, len, bt);
	o = app_str(buf, o, len, " used ");
	o = app_num(buf, o, len, bu);
	o = app_str(buf, o, len, " hits ");
	o = app_num(buf, o, len, bh);
	o = app_str(buf, o, len, " misses ");
	o = app_num(buf, o, len, bmiss);
	o = app_str(buf, o, len, " reads ");
	o = app_num(buf, o, len, br);
	o = app_str(buf, o, len, " readahead ");
	o = app_num(buf, o, len, bra);
	o = app_str(buf, o, len, " dirty ");
	o = app_num(buf, o, len, bd);
	o = app_str(buf, o, len, "\n");

	o = app_str(buf, o, len, "name: total ");
	o = app_num(buf, o, len, nt);
	o = app_str(buf, o, len, " used ");
	o = app_num(buf, o, len, nu);
	o = app_str(buf, o, len, " hits ");
	o = app_num(buf, o, len, nh);
	o = app_str(buf, o, len, " misses ");
	o = app_num(buf, o, len, nmiss);
	o = app_str(buf, o, len, "\n");

	o = app_str(buf, o, len, "page: total ");
	o = app_num(buf, o, len, pt);
	o = app_str(buf, o, len, " used ");
	o = app_num(buf, o, len, pu);
	o = app_str(buf, o, len, " hits ");
	o = app_num(buf, o, len, ph);
	o = app_str(buf, o, len, " misses ");
	o = app_num(buf, o, len, pmiss);
	o = app_str(buf, o, len, " fills ");
	o = app_num(buf, o, len, pfills);
	o = app_str(buf, o, len, " evicts ");
	o = app_num(buf, o, len, pevicts);
	o = app_str(buf, o, len, "\n");

	o = app_str(buf, o, len, "pages: alloc ");
	o = app_num(buf, o, len, pa);
	o = app_str(buf, o, len, " freed ");
	o = app_num(buf, o, len, pfree);
	o = app_str(buf, o, len, " inuse ");
	o = app_num(buf, o, len, pinuse);
	o = app_str(buf, o, len, "\n");

	if (o < len)
		buf[o] = 0;
	return ((int)o);
}

/* ---------------------------------------------------------------- VFS I/O */
/*
 * Two explicit VFS interfaces.  The synchronous one runs the backend in the
 * caller and returns the byte count; the asynchronous one queues a kiocb and
 * returns -EIOCBQUEUED, with exactly one completion later.
 * 两套显式 VFS 接口：同步接口在调用者上下文执行后端并返回字节数；异步接口
 * 排队 kiocb 并返回 -EIOCBQUEUED，之后恰好完成一次。
 */
/* 中文：同步读接口，调用 vnode 的 read 操作把数据读入 buf 并返回字节数，参数或操作缺失返回 -EINVAL */
/* EN: Synchronous read interface, invoking the vnode's read op into buf and returning the byte count, or -EINVAL on a bad argument or missing op */
int
vfs_read_sync(struct vnode *vp, unsigned off, void *buf, unsigned len)
{

	if (vp == 0 || vp->ops->read == 0)
		return (-EINVAL);
	return (vp->ops->read(vp, off, buf, len));
}

/* 中文：同步写接口，调用 vnode 的 write 操作把 buf 数据写出并返回字节数，参数或操作缺失返回 -EINVAL */
/* EN: Synchronous write interface, invoking the vnode's write op on buf and returning the byte count, or -EINVAL on a bad argument or missing op */
int
vfs_write_sync(struct vnode *vp, unsigned off, const void *buf, unsigned len)
{

	if (vp == 0 || vp->ops->write == 0)
		return (-EINVAL);
	return (vp->ops->write(vp, off, buf, len));
}

/* 中文：异步读接口，校验 kiocb 与 vnode 后把操作标记为 KI_READ 并提交异步队列 */
/* EN: Asynchronous read interface, validating the kiocb and vnode, marking the op as KI_READ and queuing it for completion */
int
vfs_read_async(struct kiocb *ki)
{

	if (ki == 0 || ki->ki_vp == 0 || ki->ki_vp->ops->read == 0)
		return (-EINVAL);
	ki->ki_op = KI_READ;
	return (vfs_submit_async(ki));
}

/* 中文：异步写接口，校验 kiocb 与 vnode 后把操作标记为 KI_WRITE 并提交异步队列 */
/* EN: Asynchronous write interface, validating the kiocb and vnode, marking the op as KI_WRITE and queuing it for completion */
int
vfs_write_async(struct kiocb *ki)
{

	if (ki == 0 || ki->ki_vp == 0 || ki->ki_vp->ops->write == 0)
		return (-EINVAL);
	ki->ki_op = KI_WRITE;
	return (vfs_submit_async(ki));
}
