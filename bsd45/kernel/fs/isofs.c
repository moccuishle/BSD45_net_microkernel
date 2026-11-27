/*
 * isofs.c - read-only ISO9660 root filesystem on the ATAPI CD-ROM.
 *
 * The kernel only needs what execve() does: mount the primary volume,
 * resolve a path to (LBA, size) and read the file.  Directory records are
 * walked sector by sector (ISO9660 never lets a record cross a sector).
 */

#include <sys/param.h>
#include <sys/systm.h>

#include "atapi.h"
#include "isofs.h"
#include "vfs.h"

extern void bcopy(const void *, void *, unsigned);
extern int bcmp(const void *, const void *, unsigned);

#define	ISO_SECTOR	2048
#define	ISO_ROOT_REC	156	/* offset of the root directory record in the PVD */

/*
 * No file-scope sector buffer.  Directory scans used to read through one, so
 * two concurrent lookups (namei() without the BKL, §7.2.1 item 7) would have
 * overwritten each other's records and could have returned a vnode for the
 * wrong extent.  The scans now parse the block in place in the buffer cache:
 * bread() pins it for the duration of one sector, which is exactly the
 * lifetime the record walk needs, and costs no copy and no stack buffer.
 * 中文：不再用文件级扇区缓冲。目录扫描以前经它中转，两个并发 lookup（摘掉 BKL 的
 * namei，§7.2.1 第 7 条）会互相覆盖记录，甚至可能返回错误 extent 的 vnode。现在直接
 * 就地在缓冲缓存里解析：bread() 把该块固定到本轮扫描结束，正是记录遍历需要的生命
 * 周期，且既不用拷贝也不占栈。
 */
static unsigned		root_lba, root_size;
static int		mounted;

/*
 * S17: all ISO9660 metadata goes through the buffer cache (vfs_bio.c).
 * These helpers hide the struct buf plumbing from the on-disk format code;
 * the block size equals the CD sector size, so a record always sits inside
 * one cached block.
 */
/* EN: bread()s sector lba from BDEV_CD and returns -1 on failure; otherwise copies ISO_SECTOR bytes into out, brelse()s the buffer and returns 0. */
/* 中文：用 bread 从 BDEV_CD 读取第 lba 扇区，失败返回 -1；否则把 ISO_SECTOR 字节复制到 out，brelse 释放缓冲区并返回 0。 */
static int
iso_readsec(unsigned lba, unsigned char *out)
{
	struct buf *bp;

	bp = bread(BDEV_CD, lba);
	if (bp == 0)
		return (-1);
	bcopy(buf_data(bp), out, ISO_SECTOR);
	brelse(bp);
	return (0);
}

/* EN: Decodes a 4-byte little-endian integer by OR-ing p[0] with p[1..3] shifted left by 8/16/24, used for ISO9660 on-disk fields. */
/* 中文：把 p[0] 与依次左移 8/16/24 位的 p[1..3] 相或，解出 4 字节小端整数，用于读取 ISO9660 的磁盘字段。 */
static unsigned
le32(const unsigned char *p)
{

	return (p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/*
 * ISO9660 names are upper case; the kernel's exec paths are not, and files
 * are shipped as NAME.ELF so that mkisofs -iso-level 1 (8.3) accepts them.
 */
/* EN: Copies at most n (capped at 40) name bytes of src into dst, folding a-z to A-Z, and appends .ELF when isfile is set and no dot was seen, then NUL-terminates. */
/* 中文：把 src 中最多 n 个（上限 40）名字字节复制到 dst 并将 a-z 转为大写，若 isfile 置位且名字中未出现点号则追加 .ELF，最后补 NUL 结尾。 */
static void
iso_name(char *dst, const char *src, int n, int isfile)
{
	int i, dot = 0;

	for (i = 0; i < n && i < 40; i++) {
		char c = src[i];

		if (c == '.')
			dot = 1;
		if (c >= 'a' && c <= 'z')
			c = c - 'a' + 'A';
		dst[i] = c;
	}
	if (isfile && !dot) {
		dst[i++] = '.';
		dst[i++] = 'E';
		dst[i++] = 'L';
		dst[i++] = 'F';
	}
	dst[i] = 0;
}

/* EN: Compares an nlen-byte ISO directory name against the NUL-terminated want byte by byte, stopping at ';', and returns nonzero only when want is fully consumed. */
/* 中文：把 nlen 字节的 ISO 目录名与以 NUL 结尾的 want 逐字节比较，遇 ';' 即停止，仅当 want 被完全匹配时才返回非 0。 */
static int
name_eq(const unsigned char *nm, int nlen, const char *want)
{
	int i = 0;

	while (i < nlen && nm[i] != ';') {
		if (want[i] == 0 || nm[i] != (unsigned char)want[i])
			return (0);
		i++;
	}
	return (want[i] == 0);
}

/*
 * Find `want' in the directory at (lba, size); fill in the record fields.
 */
/* EN: Scans the directory extent at lba for size bytes sector by sector, parsing each bread()ed block in place and brelse()ing it after the record walk; on a name_eq match fills *rlba, *rsize and *rflags; returns 0, else -1. */
/* 中文：逐扇区扫描 lba 处 size 字节的目录区，就地在 bread() 得到的块里解析并在一轮记录遍历后 brelse；匹配 name_eq 后取出块号、大小与标志填入 *rlba/*rsize/*rflags，命中返回 0，否则 -1。 */
static int
find_in_dir(unsigned lba, unsigned size, const char *want, unsigned *rlba,
    unsigned *rsize, int *rflags)
{
	unsigned off, pos;

	for (off = 0; off < size; off += ISO_SECTOR) {
		struct buf *bp;
		unsigned char *sec;

		bp = bread(BDEV_CD, lba + off / ISO_SECTOR);
		if (bp == 0)
			return (-1);
		sec = buf_data(bp);
		pos = 0;
		while (pos + 33 < ISO_SECTOR) {
			int len = sec[pos];
			int nlen;

			if (len == 0)
				break;		/* rest of the sector is padding */
			if (pos + len > ISO_SECTOR)
				break;
			nlen = sec[pos + 32];
			if (nlen > 0 && pos + 33 + nlen <= ISO_SECTOR &&
			    name_eq(sec + pos + 33, nlen, want)) {
				*rlba = le32(sec + pos + 2);
				*rsize = le32(sec + pos + 10);
				*rflags = sec[pos + 25];
				brelse(bp);
				return (0);
			}
			pos += len;
		}
		brelse(bp);
	}
	return (-1);
}

/* EN: Fails unless atapi_present(); reads sector 16, requires it to be a CD001 primary volume descriptor, then caches root_lba/root_size and sets mounted = 1. */
/* 中文：atapi_present() 不成立即失败；读取第 16 扇区并要求其为 CD001 主卷描述符，然后缓存 root_lba/root_size 并置 mounted = 1。 */
int
iso_mount(void)
{
	struct buf *bp;
	unsigned char *sec;

	if (!atapi_present())
		return (-1);
	bp = bread(BDEV_CD, 16);
	if (bp == 0) {
		printf("isofs: sector 16 read failed\n");
		return (-1);
	}
	sec = buf_data(bp);
	if (sec[0] != 1 || bcmp(sec + 1, "CD001", 5) != 0) {
		printf("isofs: not a primary volume descriptor\n");
		brelse(bp);
		return (-1);
	}
	root_lba = le32(sec + ISO_ROOT_REC + 2);
	root_size = le32(sec + ISO_ROOT_REC + 10);
	brelse(bp);
	if (root_lba == 0 || root_size == 0)
		return (-1);
	mounted = 1;
	return (0);
}

/* EN: Returns the mounted flag that iso_mount() sets on success, so callers can test whether the ISO9660 filesystem is available. */
/* 中文：返回 iso_mount() 成功时置位的 mounted 标志，供调用者判断 ISO9660 文件系统是否可用。 */
int
iso_mounted(void)
{

	return (mounted);
}

/* EN: Returns root_lba, the extent LBA of the root directory that iso_mount() decoded from the primary volume descriptor. */
/* 中文：返回 iso_mount() 从主卷描述符解析出的根目录起始块号 root_lba。 */
unsigned
iso_root(void)
{

	return (root_lba);
}

/* EN: Returns root_size, the byte length of the root directory extent cached by iso_mount(). */
/* 中文：返回 iso_mount() 缓存的根目录数据区字节长度 root_size。 */
unsigned
iso_root_size(void)
{

	return (root_size);
}

/* EN: Walks the path component by component with find_in_dir(), retries a final dotless name without .ELF, fills *lba/*size/*isdir; -1 on a missing, over-long or wrong-type component. */
/* 中文：用 find_in_dir() 逐级解析路径，最后一级可去掉 .ELF 后缀重试，成功时填入 *lba/*size/*isdir；组件缺失、超过 40 字符或类型不符则返回 -1。 */
int
iso_stat(const char *path, unsigned *lba, unsigned *size, int *isdir)
{
	char name[64];
	const char *p = path;
	unsigned cur_lba = root_lba, cur_size = root_size;
	int flags, isfile;

	if (!mounted)
		return (-1);
	while (*p == '/')
		p++;
	if (*p == 0) {			/* the root directory itself */
		*lba = root_lba;
		*size = root_size;
		*isdir = 1;
		return (0);
	}

	for (;;) {
		const char *slash = p;
		int n;

		while (*slash && *slash != '/')
			slash++;
		if (slash == p)
			return (-1);
		n = slash - p;
		if (n > 40)
			return (-1);
		isfile = (*slash == 0);
		iso_name(name, p, n, isfile);
		if (find_in_dir(cur_lba, cur_size, name, lba, size, &flags) < 0) {
			/*
			 * The last component may be a directory: iso_name()
			 * appends ".ELF" to dotless names for exec paths, so
			 * retry with the bare name before giving up.
			 */
			if (!isfile)
				return (-1);
			iso_name(name, p, n, 0);
			if (find_in_dir(cur_lba, cur_size, name, lba,
			    size, &flags) < 0)
				return (-1);
			isfile = 0;
		}
		if (isfile) {
			*isdir = 0;
			return ((flags & 2) ? -1 : 0);
		}
		if ((flags & 2) == 0)		/* not a directory */
			return (-1);
		if (*slash == 0) {		/* the path ends at a directory */
			*isdir = 1;
			return (0);
		}
		cur_lba = *lba;
		cur_size = *size;
		p = slash;
		while (*p == '/')
			p++;
	}
}

/* EN: Thin wrapper over iso_stat() that rejects a missing path or a directory, returning the extent LBA and size of a regular file. */
/* 中文：对 iso_stat() 的简单封装：路径不存在或指向目录时失败，否则返回普通文件的数据区块号与大小。 */
int
iso_lookup(const char *path, unsigned *lba, unsigned *size)
{
	int isdir;

	if (iso_stat(path, lba, size, &isdir) < 0 || isdir)
		return (-1);
	return (0);
}

/*
 * Return the next directory entry at (lba, size), starting at *posp.
 * Names come back lower case with the ";1" version suffix and any trailing
 * dot removed.  Returns 0 on success, 1 at end of directory, -1 on error.
 */
/* EN: Walks records from *posp, skipping padding, cross-sector entries and . and .., and returns a lowercased ;1-stripped name plus elba/esize/eflags; 0 entry, 1 end, -1 error. */
/* 中文：从 *posp 起遍历 (lba,size) 目录中的记录，跳过填充、跨扇区记录以及 . 和 ..，返回去掉 ;1 的小写名字及 elba/esize/eflags 并推进 *posp；0 表示条目，1 表示结束，-1 表示读失败。 */
int
iso_readdir(unsigned lba, unsigned size, unsigned *posp, char *name,
    unsigned *elba, unsigned *esize, int *eflags)
{
	unsigned pos = *posp;
	unsigned char *rec;
	int len, nlen, i, j;

	while (pos < size) {
		struct buf *bp;
		unsigned char *sec;

		/* records never cross a sector, so one bread() covers the walk */
		bp = bread(BDEV_CD, lba + pos / ISO_SECTOR);
		if (bp == 0)
			return (-1);
		sec = buf_data(bp);
		rec = sec + (pos % ISO_SECTOR);
		len = rec[0];
		if (len == 0) {			/* padding to the next sector */
			brelse(bp);
			pos = (pos / ISO_SECTOR + 1) * ISO_SECTOR;
			continue;
		}
		if ((pos % ISO_SECTOR) + len > ISO_SECTOR) {
			brelse(bp);
			pos = (pos / ISO_SECTOR + 1) * ISO_SECTOR;
			continue;
		}
		nlen = rec[32];
		if (nlen == 1 && (rec[33] == 0 || rec[33] == 1)) {
			brelse(bp);
			pos += len;		/* "." and ".." */
			continue;
		}
		if (nlen <= 0 || nlen > 48) {
			brelse(bp);
			pos += len;
			continue;
		}
		/* copy, lower case, stop at ';' */
		for (i = 0, j = 0; i < nlen && j < 40; i++) {
			char c = rec[33 + i];

			if (c == ';')
				break;
			if (c >= 'A' && c <= 'Z')
				c = c - 'A' + 'a';
			name[j++] = c;
		}
		while (j > 0 && name[j - 1] == '.')
			j--;
		name[j] = 0;
		*elba = le32(rec + 2);
		*esize = le32(rec + 10);
		*eflags = rec[25];
		brelse(bp);
		*posp = pos + len;
		return (0);
	}
	return (1);
}

/* EN: Uses iso_lookup() to find the extent, rejects files larger than max, reads ceil(size/ISO_SECTOR) sectors straight into buf, stores the file size in *lenp and returns 0. */
/* 中文：先用 iso_lookup() 定位文件区，拒绝大于 max 的文件，把 ceil(size/ISO_SECTOR) 个扇区直接读入 buf，将真实文件长度写入 *lenp 并返回 0。 */
int
iso_read_file(const char *path, void *buf, unsigned max, unsigned *lenp)
{
	unsigned lba, size, nsec;

	if (iso_lookup(path, &lba, &size) < 0)
		return (-1);
	if (size > max)
		return (-1);
	nsec = (size + ISO_SECTOR - 1) / ISO_SECTOR;
	if (nsec == 0)
		return (-1);
	{
		unsigned i;

		for (i = 0; i < nsec; i++)
			if (iso_readsec(lba + i,
			    (unsigned char *)buf + i * ISO_SECTOR) < 0)
				return (-1);
	}
	*lenp = size;
	return (0);
}
