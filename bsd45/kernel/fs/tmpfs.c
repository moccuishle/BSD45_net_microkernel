/*
 * tmpfs.c - the writable /tmp backend, split out of vfs.c (S100.2a).
 *
 * This backend talks to the rest of the kernel only through the primitives
 * listed in docs/kernel-boundary.md:
 *	KP-2  pmap_map()            - map a data page into the kernel window
 *	KP-3  vfs_page_alloc/free/unmap - data page ownership
 * plus bcopy/bzero and the VFS vnode helpers vn_alloc()/vn_put().
 * It never touches the process table, the scheduler or the page allocator.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/smp.h>

#ifndef PAGE_SIZE
#define	PAGE_SIZE	4096
#endif
#ifndef VM_PROT_READ
#define	VM_PROT_READ	0x1
#define	VM_PROT_WRITE	0x2
#endif

#include "vfs.h"

extern unsigned vfs_page_alloc(void);
extern void vfs_page_free(unsigned pa);
extern void vfs_page_unmap(unsigned va);
extern void pmap_map(vm_offset_t, vm_offset_t, vm_offset_t, int);
extern void bcopy(const void *, void *, unsigned);
extern void bzero(void *, unsigned);
extern int strcmp(const char *, const char *);
extern unsigned strlen(const char *);

#define	TMPFS_MAXNODES	32
#define	TMPFS_NAMELEN	32
#define	TMPFS_DATASZ	4096
#define	TMPFS_VA	0xFE600000

struct tmpfs_node {
	int		used;
	int		isdir;
	char		name[TMPFS_NAMELEN];
	unsigned char	*data;		/* kernel window VA, 0 = no page */
	unsigned	pa;		/* backing physical page */
	unsigned	size;
};

static struct tmpfs_node tmpfs_nodes[TMPFS_MAXNODES];
static struct vnode tmpfs_rootv;

/*
 * §7.2.1 item 4: the tmpfs node lock.
 *
 * Until now every tmpfs field was serialised by the BKL alone.  read(2) on a
 * tmpfs vnode has *already* been running outside the BKL since §6.2.9 (it
 * goes through the generic vnode branch of read(2) -> vfs_read_sync()), so
 * the missing lock was a live race: a readraw() filling a page could run
 * concurrently with a write() growing n->size, with truncate() handing the
 * data page back to the pool, or with remove() reusing the slot.  This lock
 * closes that window and is the precondition for dropping the BKL on the
 * write side too.
 *
 * Scope: every node field (used/isdir/name/data/pa/size) and the lazily
 * allocated data page.  The page allocation and the size update in
 * tmpfs_write_op() must stay inside one critical section -- they are the
 * "two separate steps" the design doc warns about.
 *
 * Order: tmpfs_lock -> page_lock (tmpfs_getdata/putdata -> vfs_page_alloc/
 * free) and tmpfs_lock -> pc_lock is avoided by calling pagecache_purge_vp()
 * after the unlock.  Nothing takes tmpfs_lock while holding pc_lock:
 * pagecache_get() drops pc_lock before invoking ops->readraw().
 * 中文：tmpfs 节点锁。此前 tmpfs 全靠 BKL；但 §6.2.9 之后 tmpfs 的 read(2) 已走通用
 * vnode 分支在 BKL 外执行，与 write/truncate/remove 存在真实竞争。本锁覆盖全部节点
 * 字段（含惰性分配的数据页），且 write 里"分配页 + 改 size"必须同一临界区。锁序：
 * tmpfs_lock -> page_lock；purge 放到解锁之后，避免 tmpfs_lock -> pc_lock。
 */
static struct spinlock tmpfs_lock;

/*
 * tmpfs data pages are allocated lazily on the first write and handed back
 * by truncate/remove/rmdir, so "echo x > /tmp/f" in a loop reuses one page
 * instead of eating the whole pool.
 */
/* EN: Returns the node data pointer, allocating a page via vfs_page_alloc() mapped at TMPFS_VA + index * PAGE_SIZE and zeroed when create is set; 0 when create is clear.  Caller holds tmpfs_lock. */
/* 中文：返回节点的数据指针；create 置位且尚未分配时用 vfs_page_alloc() 取一页，映射到 TMPFS_VA + (n - tmpfs_nodes) * PAGE_SIZE 并清零 TMPFS_DATASZ 字节；create 为 0 时返回 0。调用者须持有 tmpfs_lock。 */
static unsigned char *
tmpfs_getdata_locked(struct tmpfs_node *n, int create)
{
	int i = n - tmpfs_nodes;

	if (n->data != 0)
		return (n->data);
	if (!create)
		return (0);
	n->pa = vfs_page_alloc();
	n->data = (unsigned char *)(TMPFS_VA + i * PAGE_SIZE);
	pmap_map((vm_offset_t)n->data, (vm_offset_t)n->pa,
	    (vm_offset_t)n->pa + PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE);
	bzero(n->data, TMPFS_DATASZ);
	return (n->data);
}

/* EN: Frees the node's data page by unmapping the kernel window with vfs_page_unmap() before vfs_page_free(), then clears data, pa and size; no-op when nothing is allocated.  Caller holds tmpfs_lock. */
/* 中文：先用 vfs_page_unmap() 解除内核窗口映射，再经 vfs_page_free() 归还物理页，随后清空 data、pa 与 size；未分配时直接返回。调用者须持有 tmpfs_lock。 */
static void
tmpfs_putdata_locked(struct tmpfs_node *n)
{

	if (n->data == 0)
		return;
	/* drop the kernel window mapping first, then recycle the page */
	vfs_page_unmap((unsigned)n->data);
	vfs_page_free(n->pa);
	n->data = 0;
	n->pa = 0;
	n->size = 0;
}

struct vnodeops tmpfs_vnodeops;

/* ---------------------------------------------------------------- root */

/* EN: Zeroes the static tmpfs_rootv and sets ref=1, type=VDIR, ops=tmpfs_vnodeops and node=-1 (the negative index that marks the root); no page or vnode is allocated. */
/* 中文：清零静态的 tmpfs_rootv，并设置 ref=1、type=VDIR、ops=tmpfs_vnodeops、node=-1（负下标即根节点标记）；不分配任何页或 vnode。 */
void
tmpfs_init(void)
{

	bzero(&tmpfs_rootv, sizeof(tmpfs_rootv));
	tmpfs_rootv.ref = 1;
	tmpfs_rootv.type = VDIR;
	tmpfs_rootv.ops = &tmpfs_vnodeops;
	tmpfs_rootv.node = -1;
	spin_init(&tmpfs_lock);
}

/* EN: Returns the address of the single static tmpfs root vnode, tmpfs_rootv; no lock is taken and the caller gets no extra reference. */
/* 中文：返回唯一的静态 tmpfs 根 vnode（tmpfs_rootv）的地址；不加锁，也不为调用者增加引用计数。 */
struct vnode *
tmpfs_root(void)
{

	return (&tmpfs_rootv);
}

/* EN: Returns nonzero only when vp is exactly the static tmpfs root vnode; this identity test lets vn_put()/vn_free() skip releasing the root. */
/* 中文：仅当 vp 正是静态 tmpfs 根 vnode 时返回非零；该同一性判断供 vn_put()/vn_free() 跳过对根节点的释放。 */
int
tmpfs_is_root(struct vnode *vp)
{

	return (vp == &tmpfs_rootv);
}

/* ---------------------------------------------------------------- nodes */

/* EN: Linear-searches all TMPFS_MAXNODES slots for a used node whose name strcmp-equals name, returning that node pointer or 0 when absent.  Caller holds tmpfs_lock. */
/* 中文：在全部 TMPFS_MAXNODES 个槽中线性查找 used 且 name 经 strcmp 相等的节点，返回该节点指针，不存在时返回 0。调用者须持有 tmpfs_lock。 */
static struct tmpfs_node *
tmpfs_find_locked(const char *name)
{
	int i;

	for (i = 0; i < TMPFS_MAXNODES; i++)
		if (tmpfs_nodes[i].used &&
		    strcmp(tmpfs_nodes[i].name, name) == 0)
			return (&tmpfs_nodes[i]);
	return (0);
}

/* EN: Rejects empty/too-long names; reuses an existing name only if isdir matches, else zeroes the first free slot, marks used/isdir, copies name (page on first write); 0 if full.  Caller holds tmpfs_lock. */
/* 中文：名字为空或 >=TMPFS_NAMELEN 时失败；已有同名节点仅在 isdir 相符时复用，否则占首个空闲槽、清零并置 used/isdir、拷入名字（数据页留到首次写才分配），无空槽返回 0。调用者须持有 tmpfs_lock。 */
static struct tmpfs_node *
tmpfs_create_node_locked(const char *name, int isdir)
{
	struct tmpfs_node *n;
	int i, len;

	len = strlen(name);
	if (len == 0 || len >= TMPFS_NAMELEN)
		return (0);
	if ((n = tmpfs_find_locked(name)) != 0)
		return (n->isdir == isdir ? n : 0);
	for (i = 0; i < TMPFS_MAXNODES; i++) {
		if (tmpfs_nodes[i].used)
			continue;
		n = &tmpfs_nodes[i];
		bzero(n, sizeof(*n));
		n->used = 1;
		n->isdir = isdir;
		bcopy(name, n->name, len + 1);
		/* the data page is created on the first write */
		return (n);
	}
	return (0);
}

/* ---------------------------------------------------------------- vnodeops */

/* EN: Only the tmpfs root is searchable (else -ENOENT); a hit gets a vn_alloc()ed vnode typed VDIR/VREG from n->isdir, with tmpfs_vnodeops and the node index; -ENOMEM if alloc fails. The node is revalidated after the (lock-free) allocation, so a lookup that raced with remove/rmdir cannot hand out a vnode for a slot that has since been reused. */
/* 中文：只允许查 tmpfs 根目录（否则 -ENOENT）；命中后用 vn_alloc() 分配 vnode，按 n->isdir 置 VDIR/VREG、ops 与节点下标并返回 0，分配失败返回 -ENOMEM。分配在锁外进行，之后要重新校验该节点，避免与 remove/rmdir 竞争的 lookup 把已被复用的槽交出去。 */
static int
tmpfs_lookup_op(struct vnode *dv, const char *name, struct vnode **vpp)
{
	struct tmpfs_node *n;
	struct vnode *vp;
	int idx, isdir;

	if (dv != &tmpfs_rootv)
		return (-ENOENT);
	/*
	 * Table scan under tmpfs_lock, vnode allocation outside it:
	 * vn_alloc() takes vfs_lock, and there is no reason to nest the
	 * two orders when a plain index copy is all we need.
	 */
	/* 中文：表扫描在 tmpfs_lock 内，vnode 分配放在锁外：vn_alloc() 会取 vfs_lock，没必要把两种锁序嵌套起来。 */
	spin_lock(&tmpfs_lock);
	n = tmpfs_find_locked(name);
	if (n == 0) {
		spin_unlock(&tmpfs_lock);
		return (-ENOENT);
	}
	idx = n - tmpfs_nodes;
	isdir = n->isdir;
	spin_unlock(&tmpfs_lock);
	vp = vn_alloc();
	if (vp == 0)
		return (-ENOMEM);
	/*
	 * §7.2.1 item 7: namei() runs without the BKL now, so a remove(2) or
	 * rmdir(2) can delete this node (and another create reuse the slot)
	 * between the scan above and this point.  Re-find the name and
	 * require the same slot; otherwise the vnode would describe a file
	 * the caller never asked for.
	 * 中文：namei 现在不持 BKL，上面的扫描与这里之间可能被 remove/rmdir 删掉该节点、
	 * 再被 create 复用同一个槽。重新按名字查找并要求落在同一个槽，否则这个 vnode
	 * 描述的就是调用者没要的文件。
	 */
	spin_lock(&tmpfs_lock);
	if (tmpfs_find_locked(name) != &tmpfs_nodes[idx]) {
		spin_unlock(&tmpfs_lock);
		vn_put(vp);
		return (-ENOENT);
	}
	spin_unlock(&tmpfs_lock);
	vp->type = isdir ? VDIR : VREG;
	vp->ops = &tmpfs_vnodeops;
	vp->node = idx;
	*vpp = vp;
	return (0);
}

/* EN: Fills *mode/*size: the root (vp->node < 0) gets 0040755/size 0, else 0040755 for a dir or 0100644 for a file, with n->size (0 for dirs); returns 0. */
/* 中文：填充 *mode/*size：根节点（vp->node < 0）为 0040755 且 size 0；否则目录为 0040755、普通文件为 0100644，并给出 n->size（目录为 0）；恒返回 0。 */
static int
tmpfs_getattr_op(struct vnode *vp, unsigned *mode, unsigned *size)
{
	struct tmpfs_node *n;

	if (vp->node < 0) {		/* the tmpfs root */
		*mode = 0040755;
		*size = 0;
		return (0);
	}
	n = &tmpfs_nodes[vp->node];
	/*
	 * §7.2.1 item 4: size is exactly what a concurrent write() grows,
	 * so the snapshot has to be taken under the node lock.
	 * 中文：size 正是并发 write() 会增长的量，必须在节点锁内取值。
	 */
	spin_lock(&tmpfs_lock);
	*mode = n->isdir ? 0040755 : 0100644;
	*size = n->isdir ? 0 : n->size;
	spin_unlock(&tmpfs_lock);
	return (0);
}

/*
 * S17: read() goes through the file page cache; this raw variant is what
 * the page cache calls to fill a page.  Writes and truncates purge the
 * cached pages of the vnode so a cat after a write sees the new data.
 */
/* EN: Uncached page-cache fill read: -EISDIR for the root or a dir, 0 if no data page or off >= size, else bcopy()s min(len, size-off) bytes and returns that count. */
/* 中文：供页缓存填页的无缓存读取：根节点或目录返回 -EISDIR，无数据页或 off >= size 返回 0，否则 bcopy 复制 min(len, size-off) 字节并返回该字节数。 */
static int
tmpfs_readraw_op(struct vnode *vp, unsigned off, void *buf, unsigned len)
{
	struct tmpfs_node *n;

	if (vp->node < 0)
		return (-EISDIR);
	n = &tmpfs_nodes[vp->node];
	/*
	 * Called by pagecache_get() with pc_lock *not* held (it drops the
	 * lock before the callback), so this is a leaf: tmpfs_lock may be
	 * taken here without inverting the documented vfs_lock -> pc_lock
	 * order.  A truncate()/remove() racing with this fill would
	 * otherwise hand the page back to the pool mid-bcopy.
	 * 中文：pagecache_get() 调用本回调时并未持有 pc_lock（先放锁再回调），
	 * 所以这里是最内层，取 tmpfs_lock 不会反转 vfs_lock -> pc_lock 的既有锁序；
	 * 否则 truncate()/remove() 可能在 bcopy 中途把页还给页池。
	 */
	spin_lock(&tmpfs_lock);
	if (!n->used || n->isdir) {
		spin_unlock(&tmpfs_lock);
		return (n->isdir ? -EISDIR : 0);
	}
	if (n->data == 0 || off >= n->size) {
		spin_unlock(&tmpfs_lock);
		return (0);
	}
	if (len > n->size - off)
		len = n->size - off;
	bcopy(n->data + off, buf, len);
	spin_unlock(&tmpfs_lock);
	return ((int)len);
}

/* EN: Thin wrapper over pagecache_read() for a tmpfs file, after rejecting the root (vp->node < 0) and directory nodes with -EISDIR, so data is served from the vnode's page cache. */
/* 中文：对 tmpfs 文件薄封装 pagecache_read()：先对根节点（vp->node < 0）和目录节点返回 -EISDIR，数据因此由该 vnode 的页缓存提供。 */
static int
tmpfs_read_op(struct vnode *vp, unsigned off, void *buf, unsigned len)
{
	struct tmpfs_node *n;
	int isdir;

	if (vp->node < 0)
		return (-EISDIR);
	n = &tmpfs_nodes[vp->node];
	spin_lock(&tmpfs_lock);
	isdir = (n->isdir || !n->used);
	spin_unlock(&tmpfs_lock);
	if (isdir)
		return (-EISDIR);
	return (pagecache_read(vp, off, buf, len));
}

/* EN: Dir -EISDIR, gone -ENOENT, off >= TMPFS_DATASZ -ENOSPC; clips len, lazily allocs the 4096-byte page (fail -ENOSPC), copies, grows n->size and purges the vnode page cache, all under tmpfs_lock except the purge; returns count. */
/* 中文：目录返回 -EISDIR、节点已删除返回 -ENOENT、off >= TMPFS_DATASZ 返回 -ENOSPC；把 len 截到页尾，经 tmpfs_getdata_locked(n,1) 惰性分配 4096 字节页（失败返回 -ENOSPC），拷贝后增长 n->size，最后 purge 该 vnode 页缓存（purge 在锁外）并返回字节数。 */
static int
tmpfs_write_op(struct vnode *vp, unsigned off, const void *buf, unsigned len)
{
	struct tmpfs_node *n;

	if (vp->node < 0)
		return (-EISDIR);
	n = &tmpfs_nodes[vp->node];
	/*
	 * Allocation of the data page and the n->size bump are two steps;
	 * they must not be observable half done, so both stay inside the
	 * node lock.  pagecache_purge_vp() runs after the unlock: holding
	 * tmpfs_lock across it would add tmpfs_lock -> pc_lock to the order.
	 * 中文：数据页分配与 n->size 增长是两个步骤，不能被观察到"完成一半"，
	 * 因此整段都在节点锁内；pagecache_purge_vp() 放到解锁之后调用，
	 * 否则会引入 tmpfs_lock -> pc_lock 的锁序。
	 */
	spin_lock(&tmpfs_lock);
	if (!n->used || n->isdir) {
		int err = n->used ? -EISDIR : -ENOENT;

		spin_unlock(&tmpfs_lock);
		return (err);
	}
	if (off >= TMPFS_DATASZ) {
		spin_unlock(&tmpfs_lock);
		return (-ENOSPC);
	}
	if (len > TMPFS_DATASZ - off)
		len = TMPFS_DATASZ - off;
	if (tmpfs_getdata_locked(n, 1) == 0) {
		spin_unlock(&tmpfs_lock);
		return (-ENOSPC);
	}
	bcopy(buf, n->data + off, len);
	if (off + len > n->size)
		n->size = off + len;
	spin_unlock(&tmpfs_lock);
	pagecache_purge_vp(vp);
	return ((int)len);
}

/* EN: vp unused: from *cookie scans the global table for the next used entry, copies name/isdir, sets *cookie = i+1, returns 0; at end sets *cookie = TMPFS_MAXNODES, returns 1. */
/* 中文：忽略 vp，从 *cookie 起扫描全局节点表找下一个 used 表项，拷出名字与 isdir 并把 *cookie 置为 i+1 后返回 0；扫描完毕把 *cookie 置为 TMPFS_MAXNODES 并返回 1。 */
static int
tmpfs_readdir_op(struct vnode *vp, unsigned *cookie, char *name, int *isdir)
{
	unsigned i;

	(void)vp;
	spin_lock(&tmpfs_lock);
	for (i = *cookie; i < TMPFS_MAXNODES; i++) {
		if (!tmpfs_nodes[i].used)
			continue;
		*cookie = i + 1;
		bcopy(tmpfs_nodes[i].name, name,
		    strlen(tmpfs_nodes[i].name) + 1);
		*isdir = tmpfs_nodes[i].isdir;
		spin_unlock(&tmpfs_lock);
		return (0);
	}
	*cookie = TMPFS_MAXNODES;
	spin_unlock(&tmpfs_lock);
	return (1);
}

/* EN: Regular file, tmpfs root only (else -ENOENT): node creation failure gives -ENOSPC, vn_alloc failure -ENOMEM, else type VREG + tmpfs_vnodeops + node index are set. */
/* 中文：只在 tmpfs 根目录创建普通文件（否则 -ENOENT）：tmpfs_create_node_locked(name,0) 返回 0 则得 -ENOSPC，vn_alloc 失败则 -ENOMEM；否则 vnode 置为 VREG、tmpfs_vnodeops 与节点下标。 */
static int
tmpfs_create_op(struct vnode *dv, const char *name, struct vnode **vpp)
{
	struct tmpfs_node *n;
	struct vnode *vp;
	int idx;

	if (dv != &tmpfs_rootv)
		return (-ENOENT);
	spin_lock(&tmpfs_lock);
	n = tmpfs_create_node_locked(name, 0);
	idx = (n != 0) ? (n - tmpfs_nodes) : -1;
	spin_unlock(&tmpfs_lock);
	if (n == 0)
		return (-ENOSPC);
	vp = vn_alloc();
	if (vp == 0)
		return (-ENOMEM);
	vp->type = VREG;
	vp->ops = &tmpfs_vnodeops;
	vp->node = idx;
	*vpp = vp;
	return (0);
}

/* EN: Unlinks a regular file from the tmpfs root: -ENOENT for a non-root dir or a missing/dir entry, else frees the node data page via tmpfs_putdata_locked() and clears n->used. */
/* 中文：从 tmpfs 根目录删除普通文件：非根目录或找不到/本身是目录的表项返回 -ENOENT，否则用 tmpfs_putdata_locked() 释放节点数据页并清除 n->used。 */
static int
tmpfs_remove_op(struct vnode *dv, const char *name)
{
	struct tmpfs_node *n;

	if (dv != &tmpfs_rootv)
		return (-ENOENT);
	spin_lock(&tmpfs_lock);
	n = tmpfs_find_locked(name);
	if (n == 0 || n->isdir) {
		spin_unlock(&tmpfs_lock);
		return (-ENOENT);
	}
	tmpfs_putdata_locked(n);
	n->used = 0;
	spin_unlock(&tmpfs_lock);
	return (0);
}

/* EN: Creates a subdirectory only under the tmpfs root: -ENOENT for another directory, -EEXIST if the name is already used, otherwise tmpfs_create_node_locked(name,1) yielding 0 or -ENOSPC. */
/* 中文：仅在 tmpfs 根目录下建立子目录：非根目录返回 -ENOENT，名字已被占用返回 -EEXIST，否则调用 tmpfs_create_node_locked(name,1)，成功返回 0、失败返回 -ENOSPC。 */
static int
tmpfs_mkdir_op(struct vnode *dv, const char *name)
{
	int rv;

	if (dv != &tmpfs_rootv)
		return (-ENOENT);
	spin_lock(&tmpfs_lock);
	if (tmpfs_find_locked(name) != 0)
		rv = -EEXIST;
	else
		rv = tmpfs_create_node_locked(name, 1) ? 0 : -ENOSPC;
	spin_unlock(&tmpfs_lock);
	return (rv);
}

/* EN: Truncates to zero length: frees the data page via tmpfs_putdata_locked() (also zeroing n->pa/n->size) and purges the vnode page cache; a directory gives -EISDIR. */
/* 中文：用 tmpfs_putdata_locked() 释放数据页（同时清零 n->pa 与 n->size）并 purge 该 vnode 页缓存，把 tmpfs 文件截断到 0 长度；目录返回 -EISDIR。 */
static int
tmpfs_truncate_op(struct vnode *vp)
{
	struct tmpfs_node *n = &tmpfs_nodes[vp->node];

	spin_lock(&tmpfs_lock);
	if (!n->used || n->isdir) {
		int err = n->used ? -EISDIR : -ENOENT;

		spin_unlock(&tmpfs_lock);
		return (err);
	}
	tmpfs_putdata_locked(n);
	spin_unlock(&tmpfs_lock);
	pagecache_purge_vp(vp);
	return (0);
}

/* EN: Removes a subdirectory from the tmpfs root: -ENOENT for a non-root dir or a missing/non-dir entry, else drops the data page and clears n->used; emptiness is not checked. */
/* 中文：从 tmpfs 根目录删除子目录：非根目录或找不到/不是目录的表项返回 -ENOENT，否则释放数据页并清除 n->used；不检查目录是否为空。 */
static int
tmpfs_rmdir_op(struct vnode *dv, const char *name)
{
	struct tmpfs_node *n;

	if (dv != &tmpfs_rootv)
		return (-ENOENT);
	spin_lock(&tmpfs_lock);
	n = tmpfs_find_locked(name);
	if (n == 0 || !n->isdir) {
		spin_unlock(&tmpfs_lock);
		return (-ENOENT);
	}
	tmpfs_putdata_locked(n);
	n->used = 0;
	spin_unlock(&tmpfs_lock);
	return (0);
}

/* EN: Renames in place: non-root -EROFS (not -ENOENT), missing old -ENOENT, existing new -EEXIST, empty/too-long new -ENAMETOOLONG; else zeroes the name field and copies the new name. */
/* 中文：原地重命名节点：非根目录返回 -EROFS（而非 -ENOENT），旧名不存在返回 -ENOENT，新名已存在返回 -EEXIST，新名为空或过长返回 -ENAMETOOLONG；否则清零 name 字段并拷入新名字。 */
static int
tmpfs_rename_op(struct vnode *dv, const char *old, const char *new)
{
	struct tmpfs_node *n;
	int len, rv;

	if (dv != &tmpfs_rootv)
		return (-EROFS);
	rv = 0;
	spin_lock(&tmpfs_lock);
	n = tmpfs_find_locked(old);
	if (n == 0)
		rv = -ENOENT;
	else if (tmpfs_find_locked(new) != 0)
		rv = -EEXIST;
	else {
		len = strlen(new);
		if (len == 0 || len >= TMPFS_NAMELEN)
			rv = -ENAMETOOLONG;
		else {
			bzero(n->name, TMPFS_NAMELEN);
			bcopy(new, n->name, len + 1);
		}
	}
	spin_unlock(&tmpfs_lock);
	return (rv);
}

/*
 * §7.2.1 item 4: lets write(2) tell a tmpfs vnode from a disk-backed one.
 * Only tmpfs is safe to drive with the BKL released today; JLFS still needs
 * the BKL for its ATA/journal path, so the syscall keeps that one serialised.
 * 中文：供 write(2) 区分 tmpfs 与磁盘后端。当前只有 tmpfs 可以在放掉 BKL 的情况下安全驱动，
 * JLFS 的 ATA/日志路径仍需 BKL，因此那条路径保持串行。
 */
int
tmpfs_is_vnode(struct vnode *vp)
{

	return (vp != 0 && vp->ops == &tmpfs_vnodeops);
}

struct vnodeops tmpfs_vnodeops = {
	tmpfs_lookup_op, tmpfs_getattr_op, tmpfs_read_op, tmpfs_write_op,
	tmpfs_readdir_op, tmpfs_create_op, tmpfs_remove_op,
	tmpfs_mkdir_op, tmpfs_rmdir_op, tmpfs_truncate_op,
	tmpfs_rename_op, tmpfs_readraw_op
};
