# BSD45 文件系统实现清单

> 代码位置：`bsd45/kernel/fs/`
> 说明：只有被 `build_kernel.ps1` / `build_kernel.sh` 显式列出的 `.c` 才会编进内核；`config.mk` 里的清单是 4.5BSD 原始树的参考元数据，不代表参与构建。

## 一、总览

| 名称 | 实现文件 | 行数 | 读写 | 编进内核 | 角色 |
|---|---|---|---|---|---|
| **ISO9660**（isofs） | `isofs.c`（vnodeops 在 `vfs.c`） | 350 | 只读 | 是 | live ISO 的根文件系统 |
| **FFS / UFS1**（ffs） | `ffs/ffs.c` | 406 | 只读 | 是 | 装完后从 ATA 盘启动的根 |
| **JLFS** | `jlfs/jlfs.c` | 968 | **读写** | 是 | FFS 的可写衍生，从盘启动时优先于 FFS |
| **tmpfs** | `tmpfs.c` | 410 | 读写 | 是 | 固定挂载在 `/tmp`，一页一文件 |
| **VFS 层** | `vfs.c` / `vfs_bio.c` / `vfs_cache.c` | 955 / 404 / 300 | — | 是 | vnode 池、namei、缓冲与页缓存 |
| **install** | `ffs/install.c` | 227 | — | 是 | 不是 FS，是 `install` 命令（ISO → 磁盘） |
| NFS | `nfs/*.c`（10 个文件，462 KB） | — | — | **否** | 4.4BSD 参考源码，未参与构建 |
| miscfs 系列 | `miscfs/*/` | — | — | 否 | 只有 `README.md` + `config.mk` 占位 |

## 二、四个真实后端

### 1. ISO9660（只读根，`isofs.c` + `vfs.c`）

- **用途**：内核只需要 `execve()` 需要的那点能力——挂主卷、把路径解析成 `(LBA, size)`、读文件。
- **实现要点**：目录记录按扇区逐个走（ISO9660 规定一条记录不跨 2048 字节扇区）；PVD 里的根目录记录固定在偏移 156；vnode 里直接存 `lba` / `size`，不建任何内存索引。
- **限制**：完全只读，`write/create/remove/mkdir/rmdir/truncate/rename` 七个操作位全是 `0`（NULL）。

### 2. FFS / UFS1（只读，`ffs/ffs.c`）

- **用途**：`install` 把 `mkffs.py` 生成的 FFS 镜像写到 ATA 盘、机器从 MBR 重启后，用它挂出根，从盘上加载 `/sbin/init`、`/bin/sh`。
- **支持范围**（只覆盖镜像实际用到的布局）：
  - 512 字节设备扇区、4096 字节文件系统块、每块 8 个 frag；
  - **只有 1 个柱面组**；
  - 文件数据**只走直接块，没有间接块**；
  - 目录用 `struct direct` 格式；
  - 超级块是**解析出来的**（不是写死常量），所以布局跟着磁盘走；但常量必须和 `hdd/mkffs.py` 一致。
- **能力**：`lookup / getattr / read / readdir / readraw`，写类操作全为 NULL。

### 3. JLFS（读写，`jlfs/jlfs.c`）

- **定位**：4.4BSD-Lite2 FFS(UFS1) 的**可写衍生**。读侧从 `ffs.c` 派生，磁盘格式与 `hdd/mkffs.py`、`ffs.c` 完全一致。
- **七个写操作全部实现**：
  `write` / `create` / `remove` / `mkdir` / `rmdir` / `truncate` / `rename`
- **空间分配**：柱面组位图。老镜像没有位图时，**首次挂载扫描 inode 表重建**（`cg_rebuild`）。
- **挂载优先级**：从盘启动时先试 JLFS，失败才退回只读 FFS（见 `sys/glue.c`）。
- **踩过的坑**：`jlfs_inode`/`putinode` 要用独立的 512 字节扇区缓冲；`cg_rebuild` 对 frag/间接块必须做 `>= f_dblkno && < f_size` 范围检查；`readfile`/`writefile` 内层循环必须带 `done < len`，否则会空转把 ATA 状态拖垮。

### 4. tmpfs（读写，`tmpfs.c`）

- **定位**：可写 `/tmp` 后端，从 `vfs.c` 里拆出来的（S100.2a）。
- **模型**：**一个文件一页**（4 KB），页通过 `pmap_map()` 映射进内核窗口，页的所有权交给 `vfs_page_alloc/free/unmap`。
- **边界**：只通过 KP-2 / KP-3 原语（`pmap_map` / `vfs_page_*`）和 VFS 助手 `vn_alloc()/vn_put()` 与内核交互，**不碰进程表、调度器和页分配器**。
- **能力**：12 个 vnodeops 全实现，可写、可 rename。

## 三、根文件系统的选择顺序（`sys/glue.c: main()`）

```
ata_probe() 成功
   ├─ jlfs_mount() == 0  → 根 = JLFS（读写），from_disk = 2
   ├─ ffs_mount()  == 0  → 根 = FFS（只读），from_disk = 1
   └─ 都失败             → "ata: no FFS/JLFS on ATA disk"
from_disk == 0 且 atapi_probe() == 0
   ├─ iso_mount() == 0   → 根 = ISO9660（live ISO）
   └─ 失败               → 退回磁盘或 tmpfs
其余情况                 → 根 = tmpfs（空）
vfs_init() 之后始终额外挂载 /tmp = tmpfs（读写）
```

要点：
- **插着 ISO 时 ISO 优先**，这样 `install` 命令一定可用。
- **挂载表是静态的 2 个槽位**（`mounts[2]`，`/` 和 `/tmp`），不是通用的多文件系统挂载框架；`mount(2)` 虽有 4.4BSD 的通用实现，但实际只登记这两个。
- 根来源由 `vfs_set_root_source()` 在 `vfs_init()` 前设定，取值：`VFS_ROOT_ISO / VFS_ROOT_FFS / VFS_ROOT_TMPFS / VFS_ROOT_JLFS`。

## 四、VFS 层（不是文件系统，但决定它们怎么被调用）

| 文件 | 行数 | 职责 |
|---|---|---|
| `vfs.c` | 955 | vnode 池、`vn_alloc()/vn_put()`、`namei()` 路径解析、cwd 规范化、静态挂载表、ISO9660 vnodeops |
| `vfs_bio.c` | 404 | 缓冲层（块 I/O 的组织与复用的缓冲头） |
| `vfs_cache.c` | 300 | 名字缓存与目录缓存（`dir_cache_keep()` 等） |
| `aio.c` | 5.1 KB | 异步 I/O 提交队列（启动日志 `aio: 8 submission slots`） |

统一接口是 `struct vnodeops`，共 12 个槽位：
`lookup / getattr / read / write / readdir / create / remove / mkdir / rmdir / truncate / rename / readraw`

其中 `readraw` 是 S17 加的「绕过页缓存的直读」，专门给页缓存填页用；`read()` 本身叠在页缓存之上，**不允许**自己转调 `readraw`。

## 五、没有编进内核的部分

- **NFS**：`nfs/` 下有完整 10 个 `.c`（`nfs_vfsops.c`、`nfs_serv.c`、`nfs_socket.c`、`nfs_bio.c`、`nfs_node.c`、`nfs_subs.c`、`nfs_syscalls.c`、`nfs_srvcache.c`、`nfs_nqlease.c` 等，共 462 KB），是 4.4BSD 原树搬来的参考实现，构建脚本完全没引用。要用它得先补 socket 层、RPC 和 `vfsconf` 注册。
- **miscfs 伪文件系统**：`deadfs`、`fdesc`、`fifofs`、`kernfs`、`nullfs`、`portal`、`procfs`、`specfs`、`umapfs`、`union`——每个目录只有 `README.md` 和 `config.mk`，**没有一行 `.c`**，纯占位。所以内核里**没有 `/proc`、没有 fifo、没有 nullfs/union 叠加**。
- **ufs 子族**：`ufs/ffs`、`ufs/lfs`、`ufs/mfs`、`ufs/ufs` 都是空目录（仅元数据）。实际在用的 FFS 实现是 `fs/ffs/ffs.c`，不在 `ufs/` 下；**LFS（日志结构文件系统）和 MFS（内存文件系统）都没有实现**。
- **`isofs/cd9660/`**：空目录，ISO9660 的实现就在 `isofs.c` 一个文件里。
- **`pagecache.c`**：`config.mk` 里记了这个名字，但文件不存在——页缓存实际落在 `vfs_cache.c` + `vfs_bio.c`。

## 六、一句话总结

真正可用的文件系统只有 **4 个**：ISO9660（只读，启动介质）、FFS/UFS1（只读，装好的系统）、**JLFS（读写，本项目核心增量）**、tmpfs（读写，`/tmp`）。其余（NFS、procfs 等 miscfs、LFS/MFS）都是**参考源码或空壳占位**，既不参与构建，运行时也不会出现。
