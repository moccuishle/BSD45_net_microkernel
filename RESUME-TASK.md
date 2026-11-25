# BSD45-ELF-x86 任务恢复文本（入口 + 缓存系统 S17）

更新时间：2026-11-17（Asia/Shanghai）
状态：**os.md 机制 1/2/3/4/5 已落地**（IPL/抢占、COW、demand paging、
内存压缩、VFS 同步/异步 + SQ/CQ 环）；`tools/all_tests.sh` **全部套件 PASSED**。
仓库：https://github.com/moccuishle/BSD_object_orient_kernel

> 本文件是恢复入口，并**合并了原 `恢复.md`**（4.5BSD VFS/VM 缓存层盘点与路线）。
> 完整的 S0–S17 功能记录、43+ 条踩坑、构建脚本细节在
> [`bsd45/RESUME-TASK.md`](bsd45/RESUME-TASK.md)；缓存实现的设计细节在
> [`docs/cache-mmap.md`](docs/cache-mmap.md)。

## 0. 最新状态（2026-11-17）

### 0.1 内核目录合并（commit `d54ecaf`）

- `kernel/obj/` → `kernel/lib/`（`kstr.c` / `kstr.h`）
- `kernel/other/strings.c` → `kernel/lib/`；`glue.c`、`mon.c`、`usermode.c`、
  `usermode_asm.s`、`stubs.c` → `kernel/sys/`
- `kernel/other/` 的遗留 `tests/stand/stand.att/conf/compile/scripts`
  → `kernel/sys/legacy/`
- `kernel/fs/pagecache.c` → `kernel/vm/`
- `kernel/obj/` 与 `kernel/other/` 已删除；当前 kernel 顶层为
  `arch driver fs hal include lib net subsys sys thread vm`

### 0.2 文档同步

- `bsd45/README.md` 目录树已更新为合并后的结构
- `docs/cache-mmap.md`、`docs/kernel-boundary.md` 中的 `usermode.c`、
  `pagecache.c`、`glue.c` 等路径已更新

### 0.3 下一步

如果要继续整理，可做 `arch/legacy`（hp300/vax 等空占位）或
`fs/legacy`、`net/proto` 的归并。

---

## 1. 快速开始

环境要求（用户负责安装；执行者只做检查）：MSYS2 + `ucrt64` GCC 16.2（或任意能
`-m32` 的 gcc）、`ld`/`objcopy`、`qemu-system-i386`、`bison`、`flex`、`python`、
`git`、`xorriso`/`genisoimage`。

```powershell
cd bsd45
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test_cache.ps1
```

```bash
cd bsd45
bash build_all.sh
bash tools/all_tests.sh        # 17 套，预期最后两行：suites failed: 0 / ALL SUITES PASSED
```

产物：`bsd45/out/BSD45-ELF-x86.iso`（约 4.6 MB，El Torito 引导）与
`bsd45/build/bsd45.elf`。

> 工具链查找：`tools-find.ps1` 会按 `mingw32 → ucrt64 → mingw64` 找 gcc，并把
> `gcc -B <dir>/` 传给所有编译命令；`as.exe` 找不到时 gcc 会**静默返回 1**。
> 本机没有 32 位 libgcc，`libc/src/quad32.c` 与 `chkstk_ms.s` 自己提供。

## 2. 源码位置

| 内容 | 位置 |
|---|---|
| 4.5BSD sys 树（内核源码） | `bsd45/kernel/` |
| `/bin/sh` 用的 4.5BSD 用户态源码 | `bsd45/libc/arch/bsd/` |
| 用户态工具（`main()` 程序） | `bsd45/libexec/` |
| libc 子集与 syscall 粘合 | `bsd45/libc/src/` |
| 内核 VFS/驱动/进程层 | `bsd45/kernel/fs/`、`driver/`、`other/` |
| 测试驱动 | `bsd45/tools/run_qemu_test.{ps1,sh}`、`all_tests.sh` |
| 缓存/mmap 设计（S17） | `docs/cache-mmap.md` |
| 内核边界契约（S100.1） | `docs/kernel-boundary.md` |

`bsd45/src/` 已删除，构建**不需要任何 tarball**。

---

## 3. 缓存系统移植（S17，已完成）

以下 3.1–3.4 是原 `恢复.md` 的盘点与决策存档（保留原文要点）；
3.5 起是 2026-11-16 的实施结果。

### 3.1 结论摘要

- 4.5BSD 的 VFS/VM 文件**大多存在，但不能直接“加进构建”**：
  `vfs_bio.c` 的函数体被删空；`vfs_cache.c` / `vfs_subr.c` / `vfs_lookup.c` /
  `vfs_vnops.c` / `vfs_syscalls.c` / `vfs_cluster.c` 以及 `vm_page.c` /
  `vm_object.c` / `vm_fault.c` / `vm_pageout.c` / `vnode_pager.c` 是完整源码，
  但依赖 **4.5BSD 内核核心**（`struct buf`、带 `v_id` 的 `struct vnode`、
  `struct mount`、`struct ucred`、`struct uio`、`VOP_*`、`sleep/wakeup`、
  `M_*` malloc、`bdevsw->d_strategy`、`vnode_if.h`）。
- 我们的内核是为这个端口重写的**最小核心**（自定义 `struct uproc`、
  协作式调度、`vfs.c` + `vnodeops`、同步 `atapi_read`）。直接编译这些文件会
  得到几百个 unknown struct / undefined reference。
- `kernel/fs/ufs/ufs/`、`kernel/fs/ufs/ffs/` **只有 `config.mk`，没有实现**；
  当前只有 ATAPI CD（只读）驱动，没有 ATA 硬盘驱动，FFS 即使有源码也无设备可挂。
- `kernel/sys/config.mk` 里列的 `vfs_bio.c`、`vfs_cache.c` 等只是注释里的
  “sources in this directory”，`obj-y` 全空，构建从未编译它们。

### 3.2 各文件真实状态

| 文件 | 真实状态 |
|---|---|
| `kernel/sys/vfs_bio.c` | **函数体被删空**：`bread()` 只剩 `/* Body deleted. */ return (EIO);`（vfs_bio.c:142-154），`bwrite/getblk/brelse/biodone` 同样；只有 `bufinit` 和 hash/队列管理是真的 |
| `kernel/sys/vfs_cache.c` | 完整 4.5BSD 实现，但用 `(dvp)->v_id`（vfs_cache.c:75）——我们的 `struct vnode` 原本没有 `v_id` |
| `vfs_subr.c`(1782) / `vfs_lookup.c`(645) / `vfs_vnops.c`(449) / `vfs_syscalls.c`(2417) / `vfs_cluster.c`(756) | 4.5BSD 完整源码，但编译不了：依赖 `struct ucred`、`struct mount`、`struct uio`、`VOP_*`、`vnode_if.h`、`sleep/wakeup`、`M_*` malloc、`bdevsw/strategy` |
| `kernel/vm/vm_page.c`(710) / `vm_object.c`(1449) / `vm_fault.c`(1037) / `vm_pageout.c`(571) / `vnode_pager.c`(582) | 完整 4.5BSD 源码，依赖 `vm_map`/`proc`/pager 注册/`vm_page` 队列——我们的内核只用了 `pmap`，没有 VM 对象层 |
| `kernel/fs/ufs/ufs/`、`kernel/fs/ufs/ffs/` | **只有 `config.mk`，没有任何 .c/.h 实现** |
| `kernel/sys/config.mk` | 这些文件名只是注释里列的 “sources in this directory”，`obj-y` 全空，构建从未编译它 |

S17 之前构建只编 19 个目标：`locore pmap machdep cons bcmp strcmp strlen strings
usermode usermode_asm embed pccon stubs glue mon atapi isofs tmpfs vfs`。

### 3.3 为什么不能“加进构建就行”

这些 4.5BSD 文件是冲着 **4.5BSD 内核核心**写的：缓冲区（`struct buf`、
`bufqueues`、`bdevsw->d_strategy`）、vnode（`v_id`、`VOP_*`、`vnode_if.h`）、
进程/凭证（`struct process`、`ucred`、`uio`）、同步/内存（`sleep/wakeup`、
`M_CACHE`、`copyin/copyout`）。本端口有意简化了这些，所以缺的不是一行
`obj-y`，而是一整套内核核心。

### 3.4 两条路线

- **路线 A：真·移植 4.5BSD 内核核心**——恢复 `vfs_bio.c` 被删的函数体、
  生成 `vnode_if.h`、带起 `init_main/kern_synch/kern_malloc/kern_descrip/
  kern_exec/fork/sig`、`vfs_init/vfs_conf`、`bdevsw/strategy`、`ucred/mount`，
  替换 `usermode.c` 的自定义 syscall 层；还要补 ATA 硬盘驱动。
  代价：长期不可启动，验收暂时用不上。
- **路线 B：按 4.5BSD 职责划分，把真正的缓存算法写进当前端口**（本次采用）：
  B1 buffer cache + 预读；B2 name cache（给 vnode 加 `v_id`）；
  B3 `vm_object/vm_page/vnode_pager` 文件页缓存；B4 UFS/FFS 暂缓。
  算法以后可以原样搬进路线 A 的 4.5BSD 结构，不会白做。

### 3.5 已实施：路线 B 的 B1+B2+B3

**新增文件**

| 文件 | 作用 |
|---|---|
| `kernel/fs/vfs_bio.c` | 2K 块 buffer cache：hash(32)+LRU、`bread/breada/brelse/bwrite/bdwrite`、hit/miss/read/readahead 统计 |
| `kernel/fs/vfs_cache.c` | name cache：`(dv->v_id,name)->vnode`，64 项/32 桶，`v_id` 校验、create/remove/rename purge |
| `kernel/fs/pagecache.c` | 4K 文件页缓存（= vm_object+vm_page+vnode_pager）：128 页、hash+LRU+引用计数、`readraw` 填充 |
| `libexec/vmstat.c` | `/bin/vmstat`：cat `/dev/vmstat` |
| `libexec/mmaptest.c` | mmap 三用例验收程序 |
| `tools/test_cache.{ps1,sh}` | 两遍 `cat` 的缓存断言 |
| `tools/test_mmap.{ps1,sh}` | file/anon/shared+msync 断言 |

**改动要点**

1. `struct vnode` 增加 `v_id`（唯一、释放清零）与 `v_key`（只读后端的稳定页缓存键）；
   `vnodeops` 末尾增加 `readraw`（页缓存填充用的无缓存读）。
2. `vfs_namei()` 先查 name cache；`vfs_create/rename` 与新增的 `vfs_remove()`
   （unlink/rmdir 统一走它）负责 purge。
3. **目录 vnode 保留**（8 槽，只保留只读 ISO 目录）：`root->bin->file` 的目录项
   缓存可跨 exec 命中；普通文件与 tmpfs 目录仍随 close/rmdir 释放，S10.1 的
   vnode 基线不变。
4. ISO 的 `read()` = `pagecache_read()`，miss 调 `iso_readraw_op()` → buffer
   cache（sequential readahead 每块最多预读 4 块）；目录扫描/PVD/readdir 全走 buffer cache。
5. tmpfs 的 `read()` 也走页缓存（键用 `v_id`），`write/truncate` 时 purge。
6. `usermode.c` 新增 `mmap(197)/munmap(73)/mprotect(74)/msync(65)`：
   立即填充、`MAP_PRIVATE` 复制、`MAP_SHARED` 直接映射页缓存页、
   `mmap_teardown()` 在 exit/exec 前解除共享 PTE 并放引用。
7. `/dev/vmstat` + `vfs_cache_report()`；monitor `vstat` 追加同样的缓存行。
8. 构建：`build_kernel.ps1/.sh` 加 3 个源文件；`build_sh.ps1/.sh` 与
   `build_iso.ps1/.sh` 加 `vmstat mmaptest`；`tools/all_tests.sh` 15 → 17 套。

**观测**（`/bin/vmstat` 或 `cat /dev/vmstat`）：

```
vnode: total 96 used 2 free 94
buf: total 64 used 64 hits 507 misses 11 reads 182 readahead 171 dirty 0
name: total 64 used 2 hits 4 misses 11
page: total 128 used 89 hits 45 misses 89 fills 89 evicts 0
pages: alloc 550 freed 54 inuse 496
```

**验收（Windows + QEMU，17 套）**

```
test_s3 ... test_ps          15 套全部 PASSED
test_cache  S17 caches: PASSED   buf hits 413->507, disk reads 182->182,
                                 page hits 2->45, fills 89->89, name hits 4
test_mmap   S17 mmap: PASSED     file ok / anon ok / shared ok / mmap: ok
---- suites failed: 0
ALL SUITES PASSED
```

关键判据：第二次 `cat /readme.txt` 期间 **`buf reads` 不变（182→182）、
`page fills` 不变（89→89）、`page hits` +43** —— 第二次完全由内存服务。

### 3.6 B4（UFS/FFS）继续暂缓

缺 ATA 硬盘驱动，`kernel/fs/ufs/*` 也没有任何实现；做了也是死代码。
将来若有块设备写路径，`vfs_bio.c` 的 `bwrite/bdwrite` 与 dirty 计数已就位。

### 3.7 下一步候选

1. **真 `fg`/`bg`**：sh 监视模式 + `tcsetpgrp` + 后台读终端 `SIGTTIN`（最大项）。
2. per-process tty/session 信息，补齐 `ps -a/-x` 语义。
3. demand paging + COW，把 `MAP_PRIVATE` 从“map 时复制”升级为缺页复制；
   需要先有用户态 page fault 处理（当前 fault 会停机）。
4. 路线 A（S100）：把 `vfs_bio.c`/`pagecache.c` 的算法搬进 4.5BSD 结构。

---

## 4. 相关文档

- [`bsd45/RESUME-TASK.md`](bsd45/RESUME-TASK.md)：完整 S0–S17 记录、构建/验收、43+ 坑
- [`docs/cache-mmap.md`](docs/cache-mmap.md)：S17 设计、数据流、坑
- [`docs/kernel-boundary.md`](docs/kernel-boundary.md)：内核边界与原语契约（S100.1）
- [`docs/microkernel-design.md`](docs/microkernel-design.md)：微内核设计（S100.2）
- [`shell.md`](shell.md)：PowerShell → bash 的固化写法与避坑

---

## 5. 待做项目：turnstile 优先级传播（调度机制升级）

来源：`os_sched.md` 问题五机制 4；与 README.md「待做项目」章节同步。

### 5.1 现状

当前优先级继承直接挂在 TCB 字段上（`t_inherit_prio` / `t_blocked_on`），
已具备：一步到位提到等待者级别（非 +1）、多等待者取 max、链式传播、
`thread_join` 场景接入、释放后恢复 base。够用于现状——内核没有可睡眠锁
对象、没有锁嵌套、没有多线程争同一把锁的 donor 队列。

### 5.2 何时才值得上 turnstile（四个前提）

只有同时满足以下条件，才应把 TCB 标量继承升级为独立的 turnstile：

1. **有真正的可睡眠锁对象**（`struct lock` / mutex，竞争时睡眠）；
   自旋锁的等待者不睡眠，按 BSD 规则不用 turnstile。
2. **存在多个线程等同一把锁** —— donor 队列才有意义，取其中最高优先级。
3. **存在锁嵌套**（如 A 持锁 1 又等锁 2）—— 需要链式传播 +
   逐层恢复：释放一把锁后降到"剩余等待者中的最高"，全释放才回 base。
4. **有实时性要求**，反转必须严格有界可预测（Mars Pathfinder 那类场景）。

### 5.3 工程判断

缺任一前提时，turnstile 的链表维护 / donor 排序 / 嵌套恢复是纯开销：
本内核 `NKTHREAD=16`、`NPROC=8`、无锁嵌套、无实时保证需求，
当前 O(1) TCB 标量继承更合适。**先不加，等引入可睡眠锁 + 锁嵌套再说。**

相关讨论：`os_sched.md` 问题三（继承不能 +1）、问题五（四道防线）。

---

## 6. 2026-11-24 更新：当前状态与未完成任务清单

> 本节在全项目重命名重构之后追加，记录**截至目前仍未完成 / 仍待验证**的事项。
> 已完成的历史（S411–S421、重命名）细节见 `os_smp.md` 与
> `.workbuddy/memory/2026-11-{23,24}.md`；本文件 0.x 节是 S17 时代状态，
> 与 SMP 相关的内容以本节为准。

### 6.0 仓库与构建现状（2026-11-24）

- 本地 HEAD / origin / net / device / core：`f723166`
  （`Rename proc.h to kprocess.h and struct proc to struct process`）。
- micro：独立历史线 `d2a613a`（内容树与上面一致，用 `commit-tree` 同步）。
- 工作区干净。内核 + 用户态全编译通过；VMware 8vCPU 冒烟
  `smpon` + `smpuser` + shell + ls + ps，TRAP=0。
- 全项目重命名已完成：`proc.h` → `kprocess.h`（6 个头文件），
  `struct proc` → `struct process`（909 处 / 151 文件）；
  `struct procstat/proclist/prochd` 与 `sys/kproc.h` 保持不变。

### 6.1 SMP / TRAP 12（判定收敛，但未彻底收口）— 最高优先

1. **`smp_user_dispatch` 仍默认 0**。user fault 已清零、BSP TRAP 12 判定收敛
   （S421 后累计 16 轮加压 0 复现，见 `os_smp.md` §15.6），但故障曾为极低频
   偶发。默认打开前需做**长时间 soak**（建议数小时混合负载），确认不再复现后
   再把默认值改为 1。手动 `smpon` + `smpuser` 当前可用于演示多核用户调度。
2. **TRAP 12 未从机制上根治**：收敛很可能是 S421 修正 CR3/cur_pd 失配的连带
   效果，而非定位到独立根因。若 soak 中复现，用 `os_smp.md` §12/§15 的反汇编
   方法（`cmpl $1,(%eax)`、崩溃点 `cpus[cid].ci_curthread`）重新取证。
3. **`kthread_reap()` 锁域错配**：`t_used=0` 的发布不持 `kthread_alloc_lock`，
   与 `tcb_reserve()` 的锁域不一致——这是**真实存在的结构性隐患**。加锁实验
   8 轮全过，但不加锁对照基线同样 8 轮全过、无区分度，故改动已回退、未提交。
   需在能稳定复现时再验证并决定是否修。
4. **多核 idle 串口输出字符交错**：`smp_print()` 与用户命令的 `write` 走两条
   路径（`printf` 有 `console_lock`，用户态 `write` 没有），并发时字符会穿插。
   次要，但影响日志可读性。
5. **疑似 `console_lock` 中断上下文自锁**：高负载下观察到一次系统无响应，
   未稳定复现；若再现，检查中断路径是否尝试再取 `console_lock`。
6. **连续 `vmkeys` 发命令出现 `foreground was ...` 焦点提示**、命令不再生效：
   对照实验证明共享 scratch 版同样如此，属键盘工具 / VMware 窗口焦点问题，
   非内核死锁（两版 TRAP 均为 0）。不必按内核 bug 处理。

### 6.2 其它无锁 / 共享数据结构（结构性隐患，部分已修）

多核审查（见 `os_smp.md` §12）发现的共享队列/链表，当前因大多在 BKL 内或
尚未被 AP 并发触达而未出问题，但属潜在风险，SMP 全面启用前应逐一评估：

- `defer` 环形队列、`aio` 队列（BKL 外内核线程可能触碰）。**注**：S429 之后
  内核线程可在任意核运行，但仍是"全机同一时刻只跑一个"（`kt_active` 票据），
  所以这两个队列还没有真正并发；要让它们并发必须先给队列自己加锁。
- `pagecache` / `bufcache` 的 LRU 与 hash 链表（S17 实现，无锁）。
- `io_uring` 风格环（如已启用）。
- 运行队列 `rq_head/rq_tail` 与 scratch 窗口已分别由 S418 / S420 修复；
  用户线程的每核运行队列由 S428 建立（`sc_rqhead/sc_rqlock`）。

### 6.2.1 调度器 / 内核线程现状（S428/S429 后）

- **每核运行队列（#13，S428）**：就绪用户线程挂在唯一一个核的队列上，
  本地取用 O(1)，空队列时向别的核"偷"（一次遍历），只有在全都找不到时才
  走 `sched_rq_sweep()` 全表修复扫描（空闲路径，代价可忽略）。状态迁移统一
  经 `sched_runnable()/sched_unrunnable()`，避免入队与状态位漂移。
- **内核线程不再绑定 BSP（#11，S429）**：`rq_pop()` 原子出队 + `kt_active`
  单实例票据，任意核都能跑 softirq/AIO/回收，但仍保持"一次一个 body"的既有
  语义；AP 只在 idle 路径（CR3=IdlePTD）里 poll，避免在别人的地址空间里跑
  内核线程。

### 6.2.2 BKL 拆分状态（#10，未完成）

已完成的地基：页分配器有独立 `page_lock`（列表/计数/水位），`proc_alloc()`
早有 `proc_table_lock`，用户线程运行队列有每核 `sc_rqlock`。加锁顺序
`BKL → sc_rqlock`、`BKL → page_lock`，无人持队列锁/页锁再取 BKL。

**尚未拆**：系统调用与缺页仍整体持 BKL。VFS、pipe、pmap、fd 表、zcomp、
pagecache 都还没有自己的锁，所以不能逐个路径"摘掉 BKL"。

一次失败的尝试值得记录（S430 的开发过程）：曾把"单线程进程的堆/栈按需零页"
做成不走 BKL 的快路径，依据是 `uk_p_nthreads == 1` 时该地址空间只有当前线程、
不可能同时被人 fork/munmap。压测（`proctest` phase B，1000 个活进程）暴露两个
后果：先是大面积 `smp: STACK OVERFLOW tid 6`，补上"当前 CR3 必须等于
`cur->uk_pd`"的判定后变成 triple fault。结论：缺页处理不只是"改一个 PTE"——
它会在 fork/exec/exit 临时切换过 CR3 的窗口里被触发，也会触碰 COW 引用计数与
zcomp 状态，这些都还没有自己的锁。**因此缺页路径在 pmap/COW/zcomp 加锁之前
必须留在 BKL 内**，不要按"单线程就安全"的直觉再试一次。

下一步（按依赖顺序）：①`pmap_lock`（页表页分配 + PTE 写入，含 CR3 归属判定）
→ ②COW 引用计数与 zcomp 加锁 → ③缺页才能离开 BKL → ④VFS/pipe/fd 表按子系统
加锁后再放系统调用入口。**进度：①② 已由 S431 完成（见 §6.2.3）；③④ 仍未做，
是当前 #10 的剩余工作。**


### 6.2.3 pmap 锁 + COW 引用计数 + zcomp 锁（S431，路线图第 ① ② 步）

S430 只把页分配器摘出 BKL。S431 补齐缺页路径另外两块共享状态，仍**不**把缺页
移出 BKL（第 ③ 步）——理由是 S430 的失败已经证明：没有 ② 就动 ③ 必炸。

- **`pmap_lock`（用户页表锁）**：一把全局自旋锁，覆盖"写用户页目录 / 页表 /
  PTE"以及随之而来的页表页分配。覆盖点：`user_pd_create`、`user_map`、
  `user_map_prot`、`user_unmap`、`free_address_space`、`copy_address_space`、
  `copy_address_space_cow`（这段会改**父进程**的 PTE：清 RW、置 COW）、
  `zcomp_refault`、`zcomp_reclaim`、缺页的 COW 分支。
  为什么不按地址空间分锁：fork 一边拷父进程、一边让子进程 CR3 生效，同一时刻
  会碰两个地址空间；热点又只是一条 PTE 存储。分锁只多一次查找。
- **对持有 CPU 递归**：`user_map()` 在编辑中途要 `page_alloc()` 一个页表页，
  `page_alloc()` 可能进 `zcomp_reclaim()`，而回收恰好遍历调用方正改的页表。
  递归（`pmap_owner/pmap_depth` 记持有 CPU）让这种重入无害：同一 CPU、同一段
  编辑、不取第二把锁。锁序 `pmap_lock → page_lock`、`pmap_lock → zcomp 锁`，
  没有反向路径（这也正是 S430 里 `page_lock` 必须"pop 完就放"的原因）。
- **COW 引用计数改原子**：`page_ref_inc/page_put` 原来是 `load/add/store`，
  两个 CPU 各加一次会丢一次自增，`page_put()` 于是释放别人还在映射的页。
  现在统一走 `page_ref_add()`（`lock; xaddw`），返回旧值供"我是不是最后一个"
  判断；`page_put()` 用旧值决定"只递减"还是"清零 + 放回空闲链表"。
- **zcomp 自锁**：槽位池、统计和那个 2 KB 的 `zwork` 编码缓冲原来是全局单写者。
  现在 `zcomp_store/load/free_slot` 各自取 `zc_lock`，两个 CPU 同时压缩不会把
  彼此的游程编码搅在一起。
- **`in_fault` 改每 CPU**：原来是全局标志——A 核缺页会让 B 核的缺页也失去
  "不要回收"的保护，更糟的是 B 核退出时清标志会把 A 核的保护一起取消。
  现在是 `in_fault_cpu[MAXCPU]`，`#define in_fault in_fault_cpu[cpu_id()]`。
- **`user_map_if_absent()`**：同一进程两个线程可能对同一个未映射页同时缺页
  （线程栈各一份、堆共享）。原来后写者覆盖先写者的 PTE，每次竞争泄漏一页；
  现在"PTE 仍 absent 才安装"，输的一方 `page_free()` 自己的副本。

**仍然没做的**：缺页与系统调用整体还在 BKL 内（第 ③ 步）。要再往前一步，需要
先把 VFS/pagecache/pipe/fd 表按子系统加锁（第 ④ 步），或者把缺页的 CR3 归属
判定做成"错了就放弃、回到 BKL 路径重试"的机制——S430 试过一次，失败在
triple fault，而不是锁本身。

### 6.2.4 构建链 / 测试工具与本次同步（S432，2026-11-25）

- **`hdd/mkjlfs.sh` 在 Windows 上"卡死"不是死循环**：MSYS2 没有 `fork(2)`，
  每 spawn 一个外部进程约 0.25–0.5 s；脚本原来每个十六进制字段都走
  `$(printf)/$(seq)/xxd/tr`、每个 4 KB 文件块一个 `dd`，3 MB 镜像要 30 分钟
  以上（Linux bash 上几秒，所以一直没暴露）。现在 `le_hex/hex_set_le/
  hex_get_le/hex_set_bit` 全用 `printf -v` + 算术展开，文件名用
  `printf '%02x' "'c"` 代替 `xxd|tr`，inode 表在内存里拼好一次 `dd` 写出，
  文件数据按连续块合并成一次 `dd`：**30 min → 76 s**，输出与 `mkjlfs.py`
  逐字节一致。
- **顺带修掉一个真 bug**：`.sh` 版**从未写入 cylinder-group 块**
  （`fs_csaddr` 处一直是全零），内核每次挂载都走
  `jlfs: rebuilding allocation bitmap`。现在写入的就是 `jlfs.c:cg_rebuild()`
  会算出的位图，与 `mkjlfs.ps1`/`.py` 一致；`hdd/BSD45.JLFS` 已重新生成。
- **测试工具**：`tools/run_qemu_test.sh` 增加 `-Smp/-Mem`；新增
  `tools/qdrive.py`（Python 驱动 QEMU TCP 串口，因为 Git Bash 的 `/dev/tcp`
  能连上但收不到数据）。两条踩过的坑：
  1. `qdrive` **必须在等待命令期间持续读串口**。原来 `time.sleep(700)`
     期间不读，QEMU 的 socket 缓冲填满后 guest 直接阻塞在 `printf` 里，
     `proctest` 压测表现为超时假死（`rc=124`）而内核其实没事（`3b91fbe` 修正）。
  2. Windows 上调用必须 `MSYS_NO_PATHCONV=1`，否则 `/bin/pthread` 会被
     MSYS 改写成 `C:/.../usr/bin/pthread`，guest 报 `not found`。
  3. 传以 `-` 开头的参数要用 `--qemu-extra=-d` 这种等号形式，写成
     `--qemu-extra -d` 会被 argparse 当成新选项而直接退出（`rc=2`）。
- **`proctest` 的"卡住"不是内核死锁**：8 vCPU / 3 GB TCG 下它打印
  `kernel /dev/ps lists 1002 live processes` 后会长时间没有输出——1000 个
  子进程的 `kill(SIGKILL)` + `wait4` 回收在 TCG 下极慢，历次验证都只到
  phase B。用 `--qemu-extra=-d --qemu-extra=cpu_reset --qemu-extra=-D`
  抓过一次：CPU0 的两次 reset 都是初始实模式（`EIP=0000fff0`、
  `EDX=00000663`，属 QEMU 启动 / `smpon` 的 INIT-SIPI 序列），内核运行期间
  没有任何新增 reset，寄存器也从未进入保护模式/内核地址，**排除 triple fault**。
  不要据此去改退出路径。
- **5 仓同步状态**：主仓 `BSD45_multi_core_process` → `3b91fbe`；镜像
  `BSD45_microkernel` → `5ffc6cb`、`BSD45_device_microkernel` → `41c609d`、
  `BSD45_net_microkernel` → `103b629`、`BSD45_multiprocess_allocation` →
  `b97f242`；5 棵树同为 `28965c8`。（`microkernel` 这个 remote 是
  `BSD45_microkernel` 的重复配置，指向同一仓库。）上表是 S432 首次同步时的
  提交号，之后每次文档/工具提交仍按同样方式用 `commit-tree` 推进 4 个镜像。

### 6.2.5 Windows/PowerShell 构建修复与 2026-11-26 基线

- **ld 参数过多会静默崩溃**：`/bin/sh` 链接 140 个 `.o` 时，`ld -m i386pe ... @objs`
  在 Windows 上以 `0xC0000409` 退出且不打印任何诊断。改为把对象列表写进
  `@response.rsp`（路径必须用 `/`，反斜杠会被 ld 当转义）后正常。
  `tools-find.ps1` 新增 `Invoke-LdRsp`，`build_sh.ps1`（sh 与每个工具）和
  `build_kernel.ps1`（两遍链接）统一走它。
- **PowerShell 工具清单漏 `pthmain`**：`build_sh.sh` 与 `build_iso.ps1` 都列了，
  `build_sh.ps1` 没有，导致 ISO 里没有 `/bin/PTHMAIN.ELF`；已补。
- **半成品 `build/incu` 会让头文件准备提前返回**：`Prepare-Headers` 只要看到
  `errno.h` 就 return。一次中断的构建会留下“有 `errno.h`、`sys/` 为空”的目录，
  之后每次都在 `sys/param.h` 上失败。重建前先删掉或移走 `build/incu`。
- **8 核基线（HEAD `d2292c6` + 本次构建修复）**：`smpon`/`smpuser`/`shell`、
  `pthread`/`pthmain`/`pthpre`/`aiotest`、`ps` 全过；日志中 0 个 `TRAP`、
  triple fault、`STACK OVERFLOW`、`address space busy`。
- **全套验收 25 PASS / 2 失效**：`test_subsys` 与 `test_ipl` 依赖 `36a84a1`
  已删除的 subsys personality 与 16 级 IPL monitor 命令（README「已删除 /
  未实现的模块」已注明），不是回归；其余 25 套 PASS。

### 6.3 调度 / 作业控制功能缺口

- **真 `fg`/`bg`**：sh 监视模式（`set -m`）+ `tcsetpgrp` + 后台读终端
  `SIGTTIN`（工作量最大的一项，见顶部 0.0.5 / S10.2 限制）。
- **per-process tty/session 信息**：补齐后 `ps -a`/`-x` 才有真实 BSD 语义；
  当前 `ps` 列出全部进程。
- **`/bin/kill` 的 `%job` 写法**：4.5BSD sh 无 `kill` 内建，需作业表配合。
- **`sleep(1)` 真实现**：当前忙等假实现，`tv_usec` 恒 0；真 sleep 要等时钟
  中断 / 超时队列（与 S10.2 / S16 限制同源）。
- **turnstile 优先级传播**：当前 TCB 标量继承够用；只有引入可睡眠锁 + 锁嵌套
  + 多 donor 争同一锁 + 实时性要求四前提后才值得上（见本文件 §5）。

### 6.4 内存 / 文件系统 / 架构路线（长期，未开工）

- **demand paging + COW**：`mmap` 目前只支持 offset 0、`MAP_PRIVATE` 是
  map 时复制；升级到缺页复制需先有用户态 page fault 处理（当前 fault 停机）。
- **B4 UFS/FFS**：继续暂缓——缺 ATA 硬盘驱动，`kernel/fs/ufs/*` 无实现。
- **S100 微内核化路线**：`S100.2a`（tmpfs 从 `vfs.c` 拆出）已做；
  `S100.3`（KP-4/6/7 + `fs_server` 最小实验）、`S100.4`（宏/微度量对照）
  未开工。硬性约束：不许牺牲 15 套验收、先文档后代码、实验放独立分支
  `exp/microkernel`（见 `bsd45/RESUME-TASK.md` 的 S100 节）。
- **路线 A（真·移植 4.5BSD 内核核心）**：恢复 `vfs_bio.c` 删除体、生成
  `vnode_if.h`、带起完整 kern/* 与 VFS 初始化，长期方向，当前验收用不上。

---

## 7. 下次开工清单（2026-11-25，按优先级排好，可直接动手）

> 把 §6.1 / §6.2.2 / §6.3 / §6.4 里"未完成"的事项收敛成任务卡：每张卡都给出
> **代码起点 → 前置依赖 → 动手步骤 → 验收命令 → 已知坑**。
> 当前 HEAD `b75deba`，5 仓树一致 `6b88cbb`，工作区干净。
> 开工前先读 §6.2.2（BKL 拆分状态）与 §6.2.4（构建/测试工具与三个坑）。

### 7.0 每次开工的固定动作

1. 构建：`bash build_all.sh`（Linux）/ PowerShell 脚本（Windows）；改了内核再
   `bash build_iso.sh`，产物 `out/BSD45-ELF-x86.iso`。
2. 8 核回归基线（Windows 下）：
   ```sh
   export PATH=/c/msys64/ucrt64/bin:$PATH MSYS_NO_PATHCONV=1
   python tools/qdrive.py --iso out/BSD45-ELF-x86.iso --mem 3072 --smp 8 \
       --timeout 900 --cmd smpon --cmd smpuser --cmd "shell+12" \
       --cmd "/bin/pthread+40" --cmd "/bin/pthmain+35" \
       --cmd "/bin/pthpre+35" --cmd "/bin/aiotest+35" \
       --cmd ps --cmd "exit 0" --log reg.log
   ```
   判据：`pthread` / `pthmain` / `pthpre` / `aiotest` 全 PASS；TRAP、triple
   fault、`STACK OVERFLOW`、`address space busy` 全 0。
3. 任何内核改动后都要跑全套验收 `tools/all_tests.sh`（当前 26 套；其中
   `test_subsys` / `test_ipl` 因 `36a84a1` 删除 subsys/IPL 已失效，见 §6.2.5）。
4. 抓 triple fault 用 `--qemu-extra=-d --qemu-extra=cpu_reset --qemu-extra=-D
   --qemu-extra=diag_cpu.log`；**以 `-` 开头的值必须写成等号形式**，
   `--qemu-extra -d` 会被 argparse 当成新选项直接退出。
5. 上库：主仓 `git push origin main`；4 个镜像用
   `git commit-tree $(git rev-parse HEAD^{tree}) -p <remote>/main` 生成提交后
   push，最后 `git fetch` 校验 5 棵树一致。

### 7.1 P1 — 长时间 soak，再把 `smp_user_dispatch` 默认改为 1（§6.1.1 / 6.1.2）

- **代码起点**：定义 `kernel/sys/usermode.c:4163`（`int smp_user_dispatch = 0;`）；
  生效点 `kernel/sys/usermode.c:3982`（`cid == 0 || smp_user_dispatch` 才允许
  AP 派发用户线程）；手动开关 `kernel/sys/mon.c:417`（`smpuser` 命令）。
- **前置认知**：TRAP 12 只是**收敛**、未从机制上根治——很可能是 S421 修正
  CR3 / `cur_pd` 失配的连带效果，而非独立根因。故障曾为极低频偶发。
- **动手步骤**：
  1. 数小时混合负载 soak：循环 `smpon` + `smpuser` + `shell`，反复跑
     `ls /bin`、`cat`、`ps`、`uname`、`date`、`/bin/pthread`，间歇夹一轮
     `/bin/proctest`。逐轮记录 TRAP / 异常计数。
  2. 连续 0 复现后把 `usermode.c:4163` 改为 `= 1`；`smpuser` 开关保留为
     "强制退回 BSP-only" 的应急手段。
  3. 重跑 7.0 全套 + 15 套验收，特别验证**不发 `smpuser`** 的默认启动路径。
- **验收**：数小时 0 次 TRAP 12 / triple fault / kernel TRAP；默认启动下
  `ps` 与多线程测试仍 PASS，cpu1–cpu7 都参与调度。
- **坑**：若 soak 中复现，按 `os_smp.md` §12/§15 的反汇编方法
  （`cmpl $1,(%eax)`、崩溃点 `cpus[cid].ci_curthread`）重新取证；**不要靠
  "再加一把锁"规避**——S422 就是池锁 vs BKL 锁序反转导致硬死锁（时钟静止）。
  `proctest` 在 TCG 下回收 1000 子进程极慢，只当间歇负载，别拿它的停顿判定死锁。

### 7.2 P2 — #10 第 ④ 步：VFS / pipe / pagecache / fd 表分锁

> 目标是把系统调用入口的 BKL 逐路径摘掉；**顺序必须是先给子系统加锁，再摘
> BKL**，反过来必炸。第 ③ 步（7.3）排在它之后。

- **代码起点**：
  - 系统调用入口：`kernel/sys/usermode.c:7492` 附近
    （`smp_bkl_enter()` → `bsd45_syscall(r)` → `softirq_dispatch()` →
    `smp_bkl_exit()`）；缺页分发 `kernel/arch/i386/i386/trap.c:213`。
  - BKL 合同与实现：`kernel/sys/usermode.c:4230-4290`
    （`smp_bkl_enter/exit/drop/held`，同 CPU 嵌套计数）。
  - fd 表：`kernel/sys/kern_descrip.c`（`p->p_fd`、`fd_ofiles`、
    `fd_ofileflags`、`fd_nfiles`）与 `usermode.c` 的 `NFD` / `cur->uk_fd`。
  - VFS：`kernel/fs/vfs.c`、`vfs_cache.c`（namei 缓存）、`vfs_bio.c`
    （buf 缓存）、`kernel/fs/tmpfs.c`。
  - pipe：`kernel/sys/usermode.c` 内的实现（`init_sysent.c` 有 `sys_pipe` 条目）。
  - pagecache / bufcache：`kernel/fs/*.c` 与 `usermode.c` 的 `pagecache_*`
    （S17 的 LRU / hash 链表，目前无锁，见 §6.2）。
  - 现成锁可参照：`page_lock`、`pmap_lock`（`usermode.c`）、
    `zc_lock`（`kernel/vm/zcomp.c`）、`sc_rqlock`（`usermode.c`）、
    `proc_table_lock`；`struct spinlock` 的实现在 `kernel/arch/i386/smp.c`。
- **锁序（全局固定，写进注释）**：`BKL → sc_rqlock`、`BKL → page_lock`、
  `pmap_lock → page_lock`、`pmap_lock → zc_lock`。**任何子系统锁都不能在持有
  时反过来等 BKL**；`page_alloc()` 取 `page_lock` 后必须 pop 完就放，再进
  `zcomp_reclaim()`。S422 的硬死锁就是违反这条。
- **动手步骤**：一次只给**一个**子系统加锁 + **一条**系统调用路径试摘 BKL，
  跑完 7.0 全套再进下一个；禁止一次性大改。锁统一用
  `spin_init/spin_lock/spin_unlock`，不要引入新的锁类型。
- **验收**：8 核下 `proctest` phase A/B、`pthread`、`aiotest`、`ls/cat/ps`
  全过；`-d cpu_reset` 确认无 triple fault；特别留意"加锁后时钟静止"型死锁。
- **坑**：S430 两次失败都源于"以为只改一个 PTE / 一条链就安全"。fork / exec /
  exit 会在 BKL 内临时切走 CR3，缺页与系统调用必须整体看待；`in_fault`
  必须保持每 CPU（`in_fault_cpu[MAXCPU]`），否则 A 核的缺页保护会被 B 核清掉。

### 7.3 P3 — #10 第 ③ 步：缺页离开 BKL（**必须等 7.2 完成**）

- **代码起点**：`kernel/sys/usermode.c:3563` 的 `user_page_fault()`
  （`smp_bkl_enter()` → `in_fault=1` → `user_page_fault_locked()` →
  `in_fault=0` → `smp_bkl_exit()`）。四条路径：`demand_zero` /
  `user_map_if_absent`、`cow_fault`、`zcomp_refault`、vma 文件映射。
- **前置**：7.2 完成；S431 已就位的 ①②（`pmap_lock` 递归、COW 原子
  `page_ref_add()`、`zc_lock`、每 CPU `in_fault`）是必要条件而非充分条件。
- **已知失败，别重蹈（S430）**：把"单线程进程的堆/栈按需零页"做成不进 BKL
  的快路径，依据是 `uk_p_nthreads == 1` 时地址空间只有当前线程。
  `proctest` phase B 先大面积 `smp: STACK OVERFLOW tid 6`，补上
  "当前 CR3 必须 == `cur->uk_pd`" 的判定后直接 triple fault。
- **正确方向**：把 CR3 归属判定做成**"错了就放弃、回到 BKL 路径重试"**，
  而不是断言"单线程就安全"；重试路径要能处理 fork/exec/exit 临时切 CR3
  的窗口，以及 COW 引用计数与 zcomp 状态。
- **验收**：7.0 全套 + 并发 fork/exec/exit 与 mmap 混跑；用
  `--qemu-extra=-d --qemu-extra=int,cpu_reset` 抓 triple fault。

### 7.4 P4 — §6.3 调度 / 作业控制功能缺口

- **代码起点**：sh 是 4.5BSD-Lite2，源码在 `libc/arch/bsd/bin/sh/`，
  **已有 `jobs.c`（作业表）**、`options.c`（`set -m`）、`exec.c`、`redir.c`。
  内核侧 `kernel/include/sys/tty.h:166/170` 已有 `isctty()` /
  `isbackground()` 宏，`TIOCSPGRP` 在 `ttycom.h:93`，`SIGTTIN` 在
  `signal.h:81`，`p_pgrp` / `p_session` 在 `sys/kprocess.h`。
- **动手步骤**：
  1. sh 的 `options.c` 接通 `-m`，`jobs.c` 用 `setpgid` / `tcsetpgrp`
     真正切换前台进程组；
  2. 终端读路径（`kernel/driver/pccon.c` / tty 层）加后台读 `SIGTTIN`，
     配合 `kern_sig.c` 的 stop / continue；
  3. `/bin/kill`（`libexec/kill.c`）支持 `%job` 写法；
  4. per-process tty / session 落地后 `ps -a` / `-x` 才有真实 BSD 语义
     （当前 `ps` 列出全部进程）；
  5. `libexec/sleep.c` 说明 libc `sleep()` 目前是忙等且 `tv_usec` 恒 0；
     真 sleep 要先有超时队列 / 时钟中断等待（与 S10.2 / S16 同源），
     属内核侧依赖，放在最后。
- **依赖**：先核对 `kern_proc.c` / `kern_sig.c` / tty 层的
  `setpgid` / `tcsetpgrp` / `SIGTTOU` / `SIGTTIN` 通路是否完整，再动 sh。
- **验收**：`sh -m` 下 `cmd &` → `fg` / `bg` → `Ctrl-Z` → `jobs` /
  `kill %1`；后台读终端被 `SIGTTIN` 停住；`ps -a/-x` 输出正确。

### 7.5 P5 — §6.4 长期路线（仅登记入口，未开工）

- **demand paging + COW**：`mmap` 目前只支持 offset 0、`MAP_PRIVATE` 是
  map 时复制；升级到缺页复制**依赖 7.3**（当前用户态 fault 停机）。
- **B4 UFS/FFS**：缺 ATA 硬盘驱动，`kernel/fs/ufs/*` 无实现，继续暂缓。
- **S100 微内核化**：`S100.2a`（tmpfs 从 `vfs.c` 拆出）已做；
  `S100.3`（KP-4/6/7 + `fs_server` 最小实验）、`S100.4`（宏/微度量对照）
  未开工。硬约束：不许牺牲 15 套验收、先文档后代码、实验放独立分支
  `exp/microkernel`（见 `bsd45/RESUME-TASK.md` 的 S100 节）。
- **路线 A**：恢复 `vfs_bio.c` 删除体、生成 `vnode_if.h`、带起完整 kern/*
  与 VFS 初始化。
- **turnstile**：见 §5，四个前提（可睡眠锁 / 多 donor 争同一锁 / 锁嵌套 /
  实时性要求）凑齐前**不要上**，当前 TCB 标量继承够用。

### 7.6 明确不必做的事（避免重复劳动）

- §6.1.3 `kthread_reap()` 锁域错配：加锁实验与不加锁基线各 8 轮全过、
  无区分度，改动已回退；除非能稳定复现，否则不要动。
- §6.1.6 `foreground was ...` 焦点提示：VMware 窗口 / 键盘工具问题，
  对照实验证明两版 TRAP 均为 0，非内核死锁。
- `proctest` 停在 `1002 live processes`：TCG 下回收 1000 子进程极慢，
  `-d cpu_reset` 已排除 triple fault（§6.2.4），不要当退出路径死锁去修。
- 缺页"单线程就安全"快路径：S430 两次失败（STACK OVERFLOW → triple
  fault），该思路永久放弃。
- 给槽位发布加 `kthread_alloc_lock`（S422 首版）：会与 BKL 锁序反转，
  正确做法是无锁发布 + 最后 `atomic_xchg(&t_used, 0)`。
