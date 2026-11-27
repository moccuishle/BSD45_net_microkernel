/*
 * vfs.h - a small vnode/VFS layer.
 *
 * Everything that used to be special-cased in the syscall switch (ramfs,
 * ISO9660, tmpfs) now goes through a vnode with an ops vector, so the
 * syscall code no longer knows which file system it is talking to.
 */
#ifndef _VFS_H_
#define _VFS_H_

#include "lib/kstr.h"

#define	VREG	1
#define	VDIR	2

struct vnode;

struct vnodeops {
	int	(*lookup)(struct vnode *dv, const char *name,
		    struct vnode **vpp);
	int	(*getattr)(struct vnode *vp, unsigned *mode, unsigned *size);
	int	(*read)(struct vnode *vp, unsigned off, void *buf,
		    unsigned len);
	int	(*write)(struct vnode *vp, unsigned off, const void *buf,
		    unsigned len);
	int	(*readdir)(struct vnode *vp, unsigned *cookie, char *name,
		    int *isdir);
	int	(*create)(struct vnode *dv, const char *name,
		    struct vnode **vpp);
	int	(*remove)(struct vnode *dv, const char *name);
	int	(*mkdir)(struct vnode *dv, const char *name);
	int	(*rmdir)(struct vnode *dv, const char *name);
	int	(*truncate)(struct vnode *vp);
	int	(*rename)(struct vnode *dv, const char *old,
		    const char *new);
	/*
	 * S17: uncached read, used by the page cache to fill a page.  The
	 * backend must go straight to its storage; read() itself is layered
	 * on top of the page cache and must not call this directly.
	 */
	int	(*readraw)(struct vnode *vp, unsigned off, void *buf,
		    unsigned len);
};

struct vnode {
	int			ref;
	int			type;		/* VREG or VDIR */
	struct vnodeops		*ops;
	unsigned		lba;		/* ISO: start sector */
	unsigned		size;		/* ISO: byte size */
	int			node;		/* tmpfs: table slot */
	/*
	 * S17: unique id for the name cache and the page cache.  A vnode slot
	 * is recycled from a LIFO pool, so pointer identity is not stable;
	 * caches key on v_id and vn_put() clears it on free.
	 */
	unsigned		v_id;
	/*
	 * S17: stable page-cache key for read-only backends.  ISO9660 files
	 * are immutable and are identified by LBA, so their pages survive a
	 * close/reopen; 0 means "key on v_id" (tmpfs, which is mutable).
	 */
	unsigned		v_key;
};

extern struct vnode *rootvnode;

/* ---------------------------------------------------------------- VFS I/O
 *
 * os.md mechanism 5: sync and async I/O share one VFS interface.  A kiocb
 * carries the vnode, offset, buffer and a completion callback.  Sync calls
 * execute in the caller; async calls return -EIOCBQUEUED and must call
 * ki_complete exactly once.
 * os.md 机制 5：同步/异步 I/O 共用一套 VFS 接口。kiocb 携带 vnode、偏移、
 * 缓冲区和完成回调；同步调用在调用者上下文执行，异步调用返回
 * -EIOCBQUEUED，并且 ki_complete 必须恰好调用一次。
 */
#define	KI_READ		1
#define	KI_WRITE	2
#define	KI_SYNC		0
#define	KI_ASYNC	1
#define	KI_FREE		0
#define	KI_QUEUED	1
#define	KI_ACTIVE	2
#define	KI_DONE		3
#define	EIOCBQUEUED	(-85)

struct kiocb {
	struct vnode	*ki_vp;
	int		 ki_op;		/* KI_READ / KI_WRITE */
	int		 ki_flags;	/* KI_SYNC / KI_ASYNC */
	unsigned	 ki_off;
	void		*ki_buf;	/* kernel buffer */
	unsigned	 ki_len;
	int		 ki_result;
	int		 ki_state;
	void		(*ki_complete)(struct kiocb *, int);
	void		*ki_private;
	struct kiocb	*ki_next;
};

/* Synchronous VFS interface: executes in the caller, returns bytes/-errno. */
int	vfs_read_sync(struct vnode *vp, unsigned off, void *buf, unsigned len);
int	vfs_write_sync(struct vnode *vp, unsigned off, const void *buf,
	    unsigned len);
/* Asynchronous VFS interface: queues the kiocb, returns -EIOCBQUEUED. */
int	vfs_read_async(struct kiocb *ki);
int	vfs_write_async(struct kiocb *ki);
/* Queue an async request; returns -EIOCBQUEUED. */
int	vfs_submit_async(struct kiocb *ki);
void	vfs_aio_init(void);
void	vfs_aio_stats(unsigned *queued, unsigned *done, unsigned *pending);
int	vfs_aio_selftest(void);

/*
 * VFS core services available to backends (S100.2a): a backend may allocate
 * a vnode for a lookup result, but must not reach into the pool itself.
 */
struct vnode *vn_alloc(void);
void	vn_ref(struct vnode *vp);

/*
 * P2: the name cache has to bump a reference while holding its own lock,
 * because it validates vp->v_id against the cache entry and the slot must
 * not be recycled in between.  vn_free() purges the name cache from inside
 * the pool lock, so the fixed order is vfs_lock -> nc_lock: the cache takes
 * the pool lock first and then calls the _locked form.
 */
/* 中文：名字缓存需在自己的锁内递增引用，因为要用 vp->v_id 与表项比对，中间不能让槽被回收；vn_free() 在池锁内清理名字缓存，故锁序固定为 vfs_lock -> nc_lock。 */
void	vfs_pool_lock(void);
void	vfs_pool_unlock(void);
void	vn_ref_locked(struct vnode *vp);

/*
 * Backends (S100.2a: tmpfs lives in tmpfs.c behind these, so vfs.c stays a
 * forwarding layer and the microkernel variant can replace the backend with
 * an IPC client without touching namei()/the mount table).
 */
void	tmpfs_init(void);
struct vnode *tmpfs_root(void);
int	tmpfs_is_root(struct vnode *vp);
int	tmpfs_is_vnode(struct vnode *vp);

/*
 * Root filesystem source (S-install): the ISO live CD mounts the ISO9660
 * root; a machine booted from the installed virtual disk mounts the FFS
 * root.  glue.c sets this before vfs_init().
 */
#define	VFS_ROOT_ISO	0
#define	VFS_ROOT_FFS	1
#define	VFS_ROOT_TMPFS	2
#define	VFS_ROOT_JLFS	3
void	vfs_set_root_source(int);
int	vfs_root_source(void);

void	vfs_init(void);
int	vfs_namei(const char *path, struct vnode **vpp);
/*
 * §7.2.1 item 7: non-zero when this boot's root backend can be walked with
 * the BKL released (ISO9660, tmpfs).  open(2) uses it as the gate; JLFS/FFS
 * have no node lock yet and stay inside the BKL.
 */
/* 中文：§7.2.1 第 7 条：本次启动的根后端能否在不持 BKL 的情况下遍历（ISO9660、tmpfs）。open(2) 用它作门控；JLFS/FFS 还没有节点锁，仍留在 BKL 内。 */
int	vfs_namei_bklfree(void);
/*
 * S29: kstr path entry points.  The A callers above stay for the historical
 * syscall ABI; the W syscall path and future file-system code use these so
 * path traversal is expressed in the kernel's native string type.
 */
int	vfs_namei_k(const struct kstr *path, struct vnode **vpp);
int	vfs_create_k(const struct kstr *path, int isdir, struct vnode **vpp);
int	vfs_remove_k(const struct kstr *path, int isdir);
int	vfs_lookup_parent(const char *path, struct vnode **dvp, char *name);
int	vfs_create(const char *path, int isdir, struct vnode **vpp);
int	vfs_remove(const char *path, int isdir);
int	vfs_mount_list(char *buf, unsigned len);
int	vfs_absolutize(const char *cwd, const char *in, char *out,
	    unsigned outlen);
int	vfs_rename(const char *oldpath, const char *newpath);
void	vn_put(struct vnode *vp);
unsigned vn_newid(void);
void	vfs_stats(unsigned *vtotal, unsigned *vused, unsigned *vfree,
	    unsigned *pnalloc, unsigned *pnfree, unsigned *pninuse);

/* ---------------------------------------------------------------- S17 caches */

/*
 * Buffer cache (vfs_bio.c): 2048-byte blocks, hash + LRU, hit/miss
 * accounting.  The only block device today is the ATAPI CD, write returns
 * EROFS; bdwrite/bwrite stay here so an ATA disk can plug in unchanged.
 */
struct buf;

#define	BDEV_CD		0
#define	BDEV_WD		1		/* ATA hard disk (the VMware virtual disk) */

void	bio_init(void);
struct buf *bread(unsigned dev, unsigned blkno);
void	breada(unsigned dev, unsigned blkno, unsigned n);
void	brelse(struct buf *bp);
int	bwrite(struct buf *bp);
void	bdwrite(struct buf *bp);
unsigned char *buf_data(struct buf *bp);
void	bio_stats(unsigned *total, unsigned *used, unsigned *hits,
	    unsigned *misses, unsigned *reads, unsigned *readahead,
	    unsigned *dirty);

/* Name cache (vfs_cache.c). */
void	namecache_init(void);
struct vnode *namecache_lookup(struct vnode *dv, const char *name);
/*
 * Find-or-insert in one critical section, so only one vnode can own a
 * (directory, name) pair.  Returns a referenced vnode; when that is not vp
 * the caller must vn_put(vp).  This is what lets namei() run without the
 * BKL: publishing the lookup result and taking the reference are one step.
 */
/* 中文：同一临界区内"查到就复用、查不到就插入"，使一个 (目录, 名字) 只可能对应一个 vnode。返回已加引用的 vnode；不等于 vp 时调用者须 vn_put(vp)。namei 能摘掉 BKL 靠的就是"发布结果 + 取引用"是同一个动作。 */
struct vnode *namecache_enter_unique(struct vnode *dv, const char *name,
	    struct vnode *vp);
void	namecache_purge(struct vnode *dv, const char *name);
void	namecache_purge_vp(struct vnode *vp);
void	namecache_stats(unsigned *total, unsigned *used, unsigned *hits,
	    unsigned *misses);

/* File page cache (pagecache.c), the vnode_pager of this port. */
void	pagecache_init(void);
unsigned pagecache_key(struct vnode *vp);
int	pagecache_read(struct vnode *vp, unsigned off, void *buf,
	    unsigned len);
unsigned pagecache_get(struct vnode *vp, unsigned pindex,
	    unsigned char **vap);
unsigned pagecache_unref(struct vnode *vp, unsigned pindex);
unsigned pagecache_unref_key(unsigned key, unsigned pindex);
void	pagecache_purge_vp(struct vnode *vp);
void	pagecache_stats(unsigned *total, unsigned *used, unsigned *hits,
	    unsigned *misses, unsigned *fills, unsigned *evicts);

/* Render the whole cache state for /dev/vmstat. */
int	vfs_cache_report(char *buf, unsigned len);

/* Kernel primitives the backends use (KP-3 in docs/kernel-boundary.md). */
unsigned vfs_page_alloc(void);
void	vfs_page_free(unsigned pa);
void	vfs_page_unmap(unsigned va);
void	vfs_page_stats(unsigned *nalloc, unsigned *nfreed, unsigned *ninuse);

#endif

