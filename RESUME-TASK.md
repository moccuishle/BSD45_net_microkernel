# BSD45-ELF-x86 任务恢复文本（入口 + 缓存系统 S17）

更新时间：2026-11-26（Asia/Shanghai）
状态：**S0–S17 全部落地**；当前工作在 **P2（BKL 拆分）**：VFS / pipe /
page cache / buffer cache / name cache / fd 表均已分锁，**普通文件 `read(2)`
已离开 BKL**（§6.2.9），其余系统调用仍在 BKL 内。
仓库：https://github.com/moccuishle/BSD45_multi_core_process （5 仓同步，见 §0.4）

> **新机器请直接跳到 [§0.4 新机器快速恢复](#04-新机器快速恢复2026-11-26)**：
> 克隆 5 仓、装工具链、构建、跑全套验收、以及"上次停在哪、下一步做什么"。
> 下一步的具体动作见 [§7.2.1](#721-下一步可直接动手2026-11-26-交接)。

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

### 0.4 新机器快速恢复（2026-11-26）

**① 拉代码（5 个仓，树必须一致）**

| 仓 | 用途 |
|---|---|
| `git@github.com:moccuishle/BSD45_multi_core_process.git` | 主仓（开发在这里） |
| `git@github.com:moccuishle/BSD45_multiprocess_allocation.git` | 镜像 |
| `git@github.com:moccuishle/BSD45_net_microkernel.git` | 镜像 |
| `git@github.com:moccuishle/BSD45_device_microkernel.git` | 镜像 |
| `git@github.com:moccuishle/BSD45_microkernel.git` | 镜像 |

- 只开发主仓；4 个镜像仓用 `commit-tree` 生成同树提交后快进推送（§7.0）。
- SSH 22 端口在国内常被拒：`~/.ssh/config` 里给 `github.com` 指定
  `HostName ssh.github.com` / `Port 443` 即可。
- 上次停下的位置：主仓 `2844c02`，树 `261ddce`（5 仓一致）。

**② 装工具链**：按 [`ai_install_gccenv.md`](ai_install_gccenv.md) 装 MSYS2
（`C:\msys64`）+ `mingw-w64-ucrt-x86_64-gcc` / `mingw-w64-i686-gcc` /
`qemu` / `bison` / `flex` / `xorriso`。本文档里有全部踩过的坑（TUNA 的 i686
路径 404、`ParallelDownloads`、`SigLevel`、i686 用 `mirror.msys2.org` 等）。

**③ 构建（Windows 只用 PowerShell，bash 留给 Ubuntu）**

```powershell
cd bsd45
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1   # 内核+用户态+ISO
# 单步：build_kernel.ps1 / build_sh.ps1 / build_iso.ps1
```

- 产物：`bsd45/out/BSD45-ELF-x86.iso`（约 16.2 MB）、`bsd45/build/bsd45.elf`。
- **不要**在 bash 里跑 `build_all.sh`：`MINGW_BIN` 这类变量会被环境层过滤，
  且 `ld` 接 140 个 `.o` 会崩；PowerShell 链已改用 `@response.rsp`。

**④ 跑验收**

```powershell
cd bsd45
$tests = @('test_s3','test_s4','test_proc','test_tty','test_s7','test_s8','test_s9',
  'test_s10','test_sig','test_s10_3','test_s10_4','test_dev','test_tools',
  'test_mount','test_ps','test_cache','test_mmap','test_subsys','test_kthread',
  'test_pthread','test_wpath','test_ipl','test_preempt','test_cow','test_fault',
  'test_badptr')
foreach ($t in $tests) {
  $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (".\tools\" + $t + ".ps1") 2>&1
  $m = $out | Select-String -Pattern 'PASSED|FAILED' | Select-Object -Last 1
  "{0,-14} {1}" -f $t, ($m ? $m.Line.Trim() : 'NO VERDICT')
}
```

**期望基线：25 PASS + 2 套已知失效**（`test_subsys` / `test_ipl` 依赖
`36a84a1` 已删除的 subsys personality 与 16 级 IPL monitor 命令，不是回归）。

```bash
# 8 核混合基线（PATH 里要有 qemu-system-i386）
cd bsd45
python tools/qdrive.py --iso out/BSD45-ELF-x86.iso --mem 3072 --smp 8 --timeout 600 \
  --boot-wait 60 --cmd smpon --cmd smpuser --cmd "shell+8" \
  --cmd "/bin/pthread+30" --cmd "/bin/rdstress+45" --cmd "/bin/pthpre+30" \
  --cmd "/bin/aiotest+30" --cmd ps --cmd "exit 0" \
  --log C:/tmp/reg8.log
```

**判读要点**
- `-d cpu_reset` 抓 triple fault；日志里 `TRAP`/`STACK OVERFLOW`/
  `address space busy` 出现即失败。
- qdrive 偶发报 `boot timeout (no monitor prompt)` 后命令静默丢弃：这是 TCG
  启动抖动，先看日志是否已经跑完再判断死锁。
- ISO 内程序名必须是**大写程序名 + `.ELF`**（`/bin/rdstress` → `RDSTRESS.ELF`），
  否则 shell 报 `not found`。

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
- [`os_lock_preempt.md`](os_lock_preempt.md)：**锁设计方法论 + BSD45 的 IRQL/
  preempt 改造方案**（锁的对象是"执行流 × 共享状态"、五步决策链、执行流 ×
  数据锁选择矩阵、BSD45 现状与改造路线、风险与验收清单、代码引用索引）。
  去掉 BKL 之前先读它，尤其是"哪些地方不能被抢占、哪些地方不能被锁"。

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

### 6.2.6 P2 第一步：VFS vnode 池 + pipe 表加锁（2026-11-26）

只加锁，**没有摘 BKL**；系统调用入口仍整体持 BKL，所以这些锁目前是"为后续拆分
准备"，不是并发正确性的唯一依赖。

- **`vfs_lock`（`kernel/fs/vfs.c`）**：保护 vnode 池的空闲扫描/分配、`ref`、
  `v_id` 发号、目录保留环。`vn_free()`/`dir_cache_keep()` 改成 `_locked`
  内部版本，调用者必须已持锁。新增 `vn_ref()`，并把 `fd_ref`、fork 继承 fd、
  name-cache 命中、mmap、aio 里直接 `vp->ref++` 的地方全部改为 `vn_ref()`。
  `vfs_stats()` 只在统计 `vnode_used[]` 时持锁。
- **锁边界**：`vfs_namei()`、各后端 `lookup/read/write` 仍不持 `vfs_lock`——
  它们会走 ATAPI 阻塞 I/O，不能拿自旋锁。因此 namei 的"查 cache + 加引用"
  仍依赖 BKL，后续要单独做 name-cache 锁与 vnode 引用的一体化。
- **`pipe_lock`（`kernel/sys/usermode.c`）**：一把锁覆盖 8 个 pipe 槽、读写端
  计数、环形位置和阻塞 pid。`pipe_alloc()`/`pipe_addref()`/fd_close/fork/
  fcntl/dup/dup2/pipe() 都走锁；`pipe_wake_reader/writer` 改为 `_locked`
  版本，调用者持锁。
- **阻塞规则**：read/write 在 `pipe_lock` 内标记 `uk_regs`、`pp->rpid/wpid`
  并 `kthread_block_tcb()`（只入队，不切换），**先 `spin_unlock` 再
  `proc_sched()`**。锁序 `pipe_lock -> sc_rqlock`，没有反向路径。
- **未做**：pagecache/buffer-cache 自己的锁、name-cache 表锁、fd 表锁、
  VFS 路径级锁；BKL 入口未动。P2 剩余工作仍是这些。

**验证（2026-11-26）**：全套 25 PASS（`test_subsys`/`test_ipl` 仍为已失效项）；
`test_s3/s7/tools/cache/mmap/mount` 单独 PASS；8 核基线 pthread/pthmain/pthpre/
aiotest 全过，0 TRAP；阻塞管道 `cat /readme.txt | head -5` 通过；`proctest`
到 phase B（1002 live）0 异常，`cpu_reset` 日志只有启动/AP 初始化的实模式 reset。

### 6.2.7 P2 第二步：page cache + buffer cache 加锁（2026-11-26）

同样只加锁、不摘 BKL。两条缓存的共同原则：**锁只保护表，不跨设备访问**。
`readraw()` → `bread()` → `atapi_read()` 是同步 PIO，持自旋锁做这个读会把
其它 CPU 全程空转，一旦读写路径将来需要同一把锁就是硬死锁。

- **`pc_lock`（`kernel/vm/pagecache.c`）**：保护 128 条 `pcs[]`、32 个哈希桶、
  LRU 链、`refs` 与命中/缺失/填充/淘汰计数。所有 `pc_*` 内部辅助函数改成
  "调用者必须持锁"。`pagecache_get()` 未命中时先在锁内 `pc_take()` 独占一个
  条目（`used=1`、`refs=1`，**已脱离哈希与 LRU**），**放锁后**才做
  `getattr`+`readraw` 填充，再回到锁内插入哈希。填充期间该条目对
  `pc_find()` 不可见、`pc_take()` 拿不走、`pagecache_purge_vp()` 因
  `refs != 0` 跳过，所以不需要额外状态位。
- **`bio_lock`（`kernel/fs/vfs_bio.c`）**：保护 64 个 `bufs[]`、哈希链、LRU、
  `b_refcnt` 与统计。原来的 `bio_evict()` 在锁内调 `bwrite()` 直接写盘，
  现在拆成 `bio_evict()`（只摘链）+ `bio_take()`（摘链并清 `B_DIRTY`、递减
  `bio_dirty`，通过 `*dirty` 告诉调用方欠一次回写），回写在**放锁后**做。
- **唯一性**：`bread()`/`breada()` 在插入哈希前**二次 `bio_find()`**，若别的
  CPU 已经发布了同一 `(dev,blkno)`，就丢弃自己读到的缓冲（`bio_reset()` +
  挂回 LRU）并返回已发布的那一个。缓存里因此不会出现同一块的两份副本
  （多份副本会让可写盘上的数据分叉）。代价只是极端并发下多读一次扇区。
- **初始化**：`pc_inited`/`bio_inited` 改为**最后置位**，避免别的 CPU 看到
  半成品表；两者都由 `vfs_init()` 在 `smp_init()` 之前调用，构建期是单 CPU，
  所以 init 本身不加锁。
- **锁序**：`vfs_lock -> pc_lock`（`vn_free -> pagecache_purge_vp`）、
  `pc_lock -> page_lock`、`bio_lock -> page_lock`（均在 init 内）。
  `pc_lock` 与 `bio_lock` **永不嵌套**：page cache 填充时 `pc_lock` 已释放。
- **未做**：name-cache 表锁、fd 表锁；BKL 入口未动。

**验证（2026-11-26）**：全套 25 PASS（同上两套已失效项）；`test_cache`/
`test_mmap`/`test_mount`/`test_tools`/`test_s7`/`test_s3` 单独 PASS；8 核基线
`pthread`/`pthmain`/`pthpre`/`aiotest` 全过、0 TRAP；阻塞管道
（`cat /readme.txt | head -20` 让写端填满 4KB 并阻塞）正常；`aiotest` 在
`pthpre` 之后连跑 4 轮全 PASS。有一次 8 核基线在 `aiotest` 处静止 13 分钟，
但同一次 qdrive 已报 "boot timeout (no monitor prompt)"，且同一序列重跑
4 次全过，判为 TCG 启动抖动；后续 soak 继续观察。

**Soak（2026-11-26 05:43-06:36，约 53 分钟）**：8 次 8 核 3GB 启动 × 24 循环
（`ls`/`cat`/`ps`/`uname`/`date`/`pthread` 共 144 条命令/轮），boot3 额外
`proctest` 到 phase B（1002 live）。**192 循环、0 个 `TRAP`/triple fault/
`STACK OVERFLOW`/`address space busy`**；8 轮里 7 轮 rc=0，boot3 rc=1 是
proctest 之后 TCG 回收超时（异常仍为 0）。第二次基线那次 `aiotest` 静止没有
在 8 轮 soak 中复现。

### 6.2.8 P2 第三步：name cache 表锁（2026-11-26）

这一步的难点不是"加一把锁"，而是**锁序**：`vn_free()` 在持 `vfs_lock` 时
调用 `namecache_purge_vp()`，而 `namecache_lookup()` 又必须在"表项校验通过"
与"`vp->ref++`"之间不被回收 vnode 槽——否则就回到 S17 之前那个"复活已释放
vnode"的 bug。按这两条同时成立，锁序只能是 `vfs_lock -> nc_lock`。

- **`nc_lock`（`kernel/fs/vfs_cache.c`）**：保护 64 条 `nc[]`、32 个哈希桶、
  LRU 与命中/未命中计数；`nc_*` 内部辅助函数改为"调用者持锁"。
- **关键处理**：`namecache_lookup()` **先取池锁**（新增
  `vfs_pool_lock()/vfs_pool_unlock()` 导出），再取 `nc_lock`，校验 `v_id`
  后用新增的 `vn_ref_locked()` 递增引用。`vn_ref()` 改成
  `spin_lock -> vn_ref_locked -> spin_unlock` 的组合，行为不变但消除重复。
- **`enter`/`purge`/`purge_vp`/`stats`**：只需 `nc_lock`。它们读的
  `v_id` 只当提示用，`lookup` 会重新校验，所以不需要池锁（也不会因此多一层
  嵌套）。
- **初始化**：`nc_inited` 改为最后置位，与 pc/bio 一致；同样在 `vfs_init()`
  的单 CPU 阶段调用。
- **锁序总结（当前全部子系统锁）**：
  `vfs_lock -> nc_lock`、`vfs_lock -> pc_lock`、`pc_lock -> page_lock`、
  `bio_lock -> page_lock`；`pc_lock` 与 `bio_lock` 永不嵌套；任何子系统锁都
  不跨设备访问、不等 BKL。
- **未做**：fd 表锁；BKL 入口仍未摘。

**验证（2026-11-26）**：全套 25 PASS（同上两套已失效项）；`test_wpath`/
`test_tools`/`test_s3`/`test_mount`/`test_cache` 单独 PASS（namei 与路径
遍历是本次改动的主路径）；8 核基线 `pthread`/`pthmain`/`pthpre`/`aiotest`
全过、0 TRAP。

### 6.2.9 P2 第四步：fd 表锁 + 第一条摘 BKL 的系统调用路径（2026-11-26）

这里是 §7.2 说的"一条路径试摘 BKL"，选的是**普通文件 `read(2)`**。

- **`fd_lock`（`kernel/sys/usermode.c`）**：一把全局锁（本移植 16 fd/进程、
  NPROC 进程，不值得做 per-process）。规则写进注释：只在锁内做 fd 数组的
  扫描/校验/偏移更新，**绝不跨后端调用**；锁内允许递增 vnode 引用，锁序
  `fd_lock -> vfs_lock`，反向不存在。
- **`read` 的文件分支改成三步**：
  1. 持 `fd_lock`：校验 `vp != 0` 且非目录、快照 `off`、`vn_ref(vp)`，放锁；
  2. `smp_bkl_drop()` → `vfs_read_sync()` → `smp_bkl_enter()`；
  3. 持 `fd_lock`：仅当该 fd 仍指向同一 vnode 才提交 `off += n`，放锁后再
     `vn_put(vp)`（把 vfs 锁从 fd 锁里挪出来，只在取引用时需要嵌套）。
- **为什么这条路径能摘**：它触及的结构全都已经有自己的锁——页缓存
  `pc_lock`、vnode 池与名字缓存 `vfs_lock -> nc_lock`、buffer cache
  `bio_lock`；fd 本身由 `fd_lock` 保护；它不睡眠、不阻塞、不碰进程表。
- **仍未摘**：`write`、`open`、`close`、`fork`、`exec`、信号、pipe、终端，
  以及 `read` 的其它 fd 类型（pipe/console/devfs/tmpfile）都还在 BKL 内。
- **新测试**：`libexec/rdstress.c`（ISO 内 `/bin/rdstress`，打包名
  `RDSTRESS.ELF`；四个构建清单都已同步）。4 个子进程 × 32 次
  open/read/close，每次与 1043 字节基准副本逐字节比对——这是这条路径在
  多核上真正并行的证据。

**验证（2026-11-26）**：全套 25 PASS（同上两套已失效项）；`test_s3`/
`test_tools`/`test_s7`/`test_cache`/`test_mmap` 单独 PASS；8 核同一次启动内
`pthread` → `rdstress` → `pthpre` → `aiotest` 全 PASS，0 TRAP；单独跑
`rdstress` 也是 PASS。




### 6.2.10 P2 第五步：lseek + fstat 摘 BKL（2026-11-28）

按 `§7.2.1` 的顺序做完了前两条（`lseek`、`fstat`），**都没有引入新锁**——
它们触及的状态全部已有归属：fd 表 = `fd_lock`、vnode 池 = `vfs_lock`。

- **共同模板**（与 §6.2.9 的 `read` 一致）：`fd_lock` 内校验 fd、`vn_ref()`
  固定 vnode、快照偏移/类型 → 放锁 → `smp_bkl_drop()` → 调后端 →
  `smp_bkl_enter()` → 回 `fd_lock` 内**确认 fd 仍指向同一 vnode** 才提交。
- **`lseek`（syscall 199）**：唯一碰后端的是 `getattr()`（`SEEK_END` 取文件
  大小），它在放掉 BKL 后执行；`SEEK_SET/CUR` 只改 `off`。fd 已关闭或被复用
  时返回 `EBADF`。
- **`fstat`（syscall 188）**：`getattr()` 同样在 BKL 外调用；`fill_stat()`
  写用户内存在**重新进入 BKL 之后**做，且**不持 `fd_lock` 写用户内存**（避免
  在自旋锁里触发缺页）。
- **非 vnode 类型仍留在 BKL 内**：`FD_TMPFILE` 读 `tmpfs[].size`，
  `FD_TMPDIR`/`FD_DIR` 改 `dpos`，设备类读 `len`。原因是 tmpfs 还没有节点锁
  （§7.2.1 第 4 条），此时摘 BKL 会把 `size` 与 `data` 的两步分配暴露成
  race；这两条路径下 fd 表在 BKL 内稳定，锁只做快照。
- **锁序**：`BKL -> fd_lock -> vfs_lock`；`vn_put()` 在 `fd_lock` 外调用。
  与 `read` 完全一致，没有新增反向路径。

**验证（2026-11-28）**：全套 26 套 **24 PASS + 2 已知失效**（`test_subsys`/
`test_ipl`，与基线逐套一致，无回归）；8 核 3 GB 基线 `smpon`+`smpuser`+
`pthread`+`pthmain`+`pthpre`+`aiotest` 全过，**0 TRAP / triple fault /
STACK OVERFLOW / address space busy**，`ps` 正常，`exit 0` 干净退出。

**仍未摘**：`write`、`open`/`close`、`fork`/`exec`、信号、pipe、console 以及
`read` 的非文件 fd 类型；下一步从 §7.2.1 第 3 条（`/dev` 伪文件，需先加
`devbuf_lock`）起。

### 6.2.11 测试提速：并行驱动 + 修掉两个白等 60s 的超时（2026-11-28）

Windows 上全套验收从 **5m33s → 59s**，三条做法都不改任何判定语义：

- **`test_kthread` / `test_ipl` 的 `-TimeoutSec` 60 → 20**：两者的 `-WaitFor`
  永远匹配不上——`kthread` 等 `"softirqtest: PASS"`，实际输出却是
  `"softirqtest: count 1 PASS"`；`ipl` 等 `"ipltest: PASS"`，但 `ipl`/
  `ipltest` 命令已在 `36a84a1` 删除。于是每套都白等满 60s（67s/68s →
  25s/26s）。**没有改 `-WaitFor` 本身**：`kthread` 第 5 条命令
  （`kthread: switches`）的输出在 WaitFor 之后，改字符串会漏采。
- **`run_qemu_test.ps1` 固定等待收紧**：每命令间隔 500→300 ms、`Pump`
  200→120 ms、`SettleMs` 300→200 ms、启动连接前 800→400 ms，全部 ≤1 s。
- **并行驱动**（`parallel=4`，16 核实测）：默认端口 5555/4444 改为用
  `TcpListener(0)` **自动分配空闲端口**，串口日志改为
  `build\qemu-serial-<PID>.log`，多套测试可同时启动、互不冲突。26 套 4 路
  并发的墙钟 **59 s**。

判读：并行后单套时间略增（争 CPU），整体墙钟降约 75%；结果与串行基线逐套
一致（24 PASS + 2 已知失效）。

### 6.2.12 P2 第六步：/dev 伪文件读缓冲加锁（2026-11-28）

按 §7.2.1 第 3 条给 `/dev` 伪文件加锁。**这一条只加锁、不摘 BKL**——不是
保守，而是这里有一条硬性的锁序限制，值得写清楚：

- **为什么不能顺手摘 BKL**：`read` 的模板是"先放内层锁，再 `smp_bkl_enter()`"。
  但 `/dev` 的渲染必须和拷贝一起被保护（否则缓冲会被另一个读者覆盖），即
  `devbuf_lock` 必须跨越"渲染 → 拷贝"。而**跨 `smp_bkl_enter()` 持锁**就构成
  `BKL -> devbuf_lock` 的反转：A 核持 `devbuf_lock` 等 BKL，B 核持 BKL 等
  `devbuf_lock` ——正是 S422 的"时钟静止"死锁。所以只要还用文件级静态缓冲，
  这条路径就不能离开 BKL。
- **`devbuf_lock`** 保护 `ps_snap_buf`/`ps_snap_len`（`/dev/ps`）、
  `mbuf`/`mlen`（`/dev/mounts`）、`vbuf`（`/dev/vmstat`）。三条分支统一写成
  "锁内渲染 + 锁内 `bcopy` 到用户 + 锁内提交 offset + 放锁后 `r->eax = n2`"，
  避免在锁内 `break`（原来的 `break` 会漏解锁）。
- **`FD_NULL`/`FD_ZERO` 不加锁**：它们不碰静态缓冲（null 直接返回 0，zero
  直接写用户页），没有需要保护的共享状态。
- **要真正让 `/dev/ps` 离开 BKL 的办法**（后续可选）：把渲染改成**每次调用
  自己的缓冲**——`/dev/mounts`（256B）、`/dev/vmstat`（512B）可以直接放栈上，
  那样就不需要 `devbuf_lock`，也就可以安全地 `smp_bkl_drop()`；
  `/dev/ps` 的 32KB 快照放不进 16KB 内核栈，需要一个小的页池或按行流式输出。

**验证（2026-11-28）**：全套 26 套 **24 PASS + 2 已知失效**；`test_ps`/
`test_mount`/`test_cache`/`test_tools` 单独 PASS；8 核 3 GB 基线
`pthread`/`pthmain`/`pthpre`/`aiotest` 全过，**0 TRAP / triple fault /
STACK OVERFLOW / address space busy**，`ps` 正常，`exit 0` 干净退出。

### 6.2.13 P2 第七步：tmpfs 节点锁 + write(2) 摘 BKL（2026-11-28）

按 §7.2.1 第 4 条。先说一个**代码盘点结论**：文档里写的 "
`FD_TMPFILE` 直接读写 `tmpfs[].data`"指的是 `usermode.c` 里那套遗留路径，而
`fd_alloc_tmpfile()`/`tmpfs_create()`/`tmpfs_remove()` **全都没有调用者**
（死代码，真正的 `/tmp` 早就是 `kernel/fs/tmpfs.c` 的 VFS 后端）。所以这一条
实际修的是 VFS 后端：

- **发现的真实竞争**（比文档描述更严重）：`read(2)` 走通用 vnode 分支，
  **§6.2.9 之后 tmpfs 的读早就在 BKL 外执行了**，而 `write`/`truncate`/`remove`
  还在 BKL 内。也就是说：`readraw()` 在填页时，另一个核可以同时 `write()` 增长
  `n->size`、`truncate()` 把数据页还回页池、`remove()` 让槽位被复用——**这是
  一个已经在线上的缺锁 bug**，不是"还没摘所以没问题"。
- **`tmpfs_lock`（`kernel/fs/tmpfs.c`）**：一把锁覆盖全部节点字段
  （`used/isdir/name/data/pa/size`）。`tmpfs_getdata/putdata/find/create_node`
  统一改为 `_locked` 内部版本，调用者持锁；`getattr`（快照 size）、
  `readraw`（填页）、`readdir`、`lookup`、`create`、`remove`、`mkdir`、
  `rmdir`、`truncate`、`rename` 全部进锁。
- **"分配页 + 改 size"必须同一临界区**：文档提醒的两步，现在整段在
  `tmpfs_write_op()` 的锁内完成；`pagecache_purge_vp()` 放在解锁之后，
  否则会引入 `tmpfs_lock -> pc_lock`。
- **锁序**：`tmpfs_lock -> page_lock`（`getdata/putdata` → `vfs_page_alloc/free`）。
  没有 `tmpfs_lock -> pc_lock`（purge 在锁外）；也没有 `pc_lock -> tmpfs_lock`
  ——`pagecache_get()` 是**先放 `pc_lock` 再回调 `ops->readraw()`**
  （`pagecache.c:332`），所以 `readraw` 里取节点锁是最内层，不反转
  `vfs_lock -> pc_lock`。`lookup`/`create` 里 `vn_alloc()`（取 `vfs_lock`）
  放在锁外，只是把下标拷出来，避免多一种嵌套。
- **`write(2)` 摘 BKL**：与 `read(2)`（§6.2.9）同一模板——`fd_lock` 内校验 fd、
  `vn_ref()` 固定 vnode、取偏移快照；放 `fd_lock` 后 `smp_bkl_drop()` 执行
  `vfs_write_sync()`，回来再确认 fd 仍指向同一 vnode 才回写偏移。
  **只放行 tmpfs**（新增 `tmpfs_is_vnode()`）：JLFS 写仍走 ATA + 日志，
  在它自己的锁审计完成前继续由 BKL 串行。

**已知遗留**（不影响内存安全，先记录）：vnode 用"节点下标"标识，`unlink` 后
槽位被新文件复用，仍持有旧 fd 的进程会读到新文件的内容。这在加锁前就存在，
彻底修需要节点引用计数 + `zombie` 标记，留到 open/close 那一条一起做。

**验证（2026-11-28）**：全套 26 套 **24 PASS + 2 已知失效**（59s）；
8 核 3 GB 基线 `pthread`/`pthmain`/`pthpre`/`aiotest` 全过，
**0 TRAP / triple fault / STACK OVERFLOW / address space busy**，`exit 0`。

### 6.2.14 P2 第八步：pipe read/write 摘 BKL（2026-11-28）

按 §7.2.1 第 5 条。`pipe_lock` 早已覆盖 8 个槽的读写者计数、环形位置和阻塞
pid，这一条只补两处：

- **BKL 在 `kthread_block_tcb()` 之前就放**：原来阻塞路径持着 BKL 一直到
  `proc_sched()` 才放手（`proc_sched()` 开头 `smp_bkl_drop()`）。现在先
  `smp_bkl_drop()` 再进 `pipe_lock`，停机的核不会占着 BKL。关键点：
  **`smp_bkl_drop()` 是嵌套安全的**（`if (!smp_bkl_held()) return;`），
  所以 `proc_sched()` 里的那次释放自动退化成空操作，不需要加"是否已释放"的
  标志位。非阻塞路径在 `smp_bkl_enter()` 之后才写 `r->eax`，因为
  返回前的 `softirq_dispatch()` 仍需要 BKL。
- **唤醒契约的发布顺序**：`uk_regs`/`uk_pipeidx`/`uk_piperw`/`uk_pipeat`/
  `uk_pipelen` 与 `pp->rpid`（写端是 `wpid`）**全部在 `pipe_lock` 内、
  `kthread_block_tcb()` 之前**写好。原来顺序是"先 block 再登记"，虽然
  `kthread_block_tcb()` 只入队不切换、且整段持 `pipe_lock`，唤醒方进不来，
  但顺序反着写属于"靠锁保护一个本可以自证的顺序"，现在改成直读即正确。
- **fd 校验**：`fd_lock` 内校验 `used/type/rw` 再取出 `pp`，**放开 `fd_lock`
  之后**才取 `pipe_lock`，避免引入 `fd_lock -> pipe_lock` 嵌套（`fd_close()`
  也是只持 `pipe_lock`）。
- **锁序不变**：`pipe_lock -> sc_rqlock`（`proc_wake`/`sched_runnable`）。
  `proc_wake()` 在没有 BKL 的情况下跑是安全的：`P_FREE`/`P_EXITING` 的检查
  本来就是为 S425（`proc_exit` 会放 BKL）加的，运行队列由 `sc_rqlock` 保护。
- **`test_preempt` 判据修正**：这条测试要求两个死循环进程的 A/B 输出至少
  4 次跳变，但实测分布是 **3～5 次**（30 个字母/进程，每个 quantum 能塞几个
  字母是随机的），阈值 4 正好压在边界上，未改内核的对照版 8 次里也有 1 次
  失败。改为 **≥3**：无抢占时恰好 **1** 次跳变（全是 A 再全是 B），3 次已经
  要求"切出去再切回来"，判据强度不变。`.ps1`/`.sh` 两份同步改。

**验证（2026-11-28）**：全套 26 套 **24 PASS + 2 已知失效**（60s）；
8 核 3 GB 基线 `pthread`/`pthmain`/`pthpre`/`aiotest` 全过，
额外跑 `echo hello-pipe | cat`、`cat /readme.txt | head -5` 两条管道，
**0 TRAP / triple fault / STACK OVERFLOW / address space busy**，`exit 0`。
`test_preempt` 单独复跑 12 次，跳变 3～5，全部 PASS。

### 6.2.15 P2 第九步：console 读等键时放掉 BKL（2026-11-28）

按 §7.2.1 第 6 条。先纠正文档里的一个前提：**不需要新建 tty 锁**。
`console_read()` → `cngetc()` → `pccngetc()` 的取字符是在 `spltty()` 里做的
（`kernel/arch/i386/isa/pccons.c:399`），取字符期间键盘 IRQ 被屏蔽，
环形的 head/tail 不可能被读到"半更新"状态。真正的 SMP 问题是另一件事：

- **一个停在提示符的 shell 会把整台机器饿死**：`console_read()` 的循环
  `c = cngetc(); if (c == 0) continue;` 一直持着 BKL 轮询，用户不敲键就一直
  不放手。修法是**只在轮询那一小段 `smp_bkl_drop()`，取到字符立刻
  `smp_bkl_enter()`**。行规程缓冲、回显、以及 `^C`/`^Z`/`^\` 的
  `sig_send_pgrp()`（要动进程表）都仍在 BKL 内，语义不变。
- **`write` 端没动，理由要写清楚**：控制台输出改的是 `pccon.c` 里的
  `crt_row/crt_col` 和 `klog_len`，这两个量**今天就没有锁**（`printf` 可以从
  中断上下文进来，本来就不持 BKL，所以"syscall 写 vs 内核 printf"的竞争
  在本次改动前就存在）。要真正把写端也摘出去，得给 `pccnputc()` 本身加锁；
  而 `panic()` 会调 `printf()`，**panic 撞上一把别人持有的控制台锁就会
  连一行 panic 信息都打不出来**。所以这一步先不动写端：写循环的工作量是
  有界的（不等待外部事件），不像读端那样会无限期占着 BKL。要做得下一步
  得先设计"panic 时无锁"的旁路。

**验证（2026-11-28）**：全套 26 套 **24 PASS + 2 已知失效**（58s），
`test_tty`/`test_sig` 单独 PASS（回显与 `^C` 都正常）；
8 核 3 GB 基线全过，**0 TRAP / triple fault / STACK OVERFLOW / address space
busy**，`exit 0`。

### 6.2.16 P2 第十一步：close(2) 摘 BKL（open 仍留 BKL）（2026-11-28）

按 §7.2.1 第 7 条，但只做能做的那一半：**close 摘掉，open/namei 不动**。

- **为什么 open 不能跟着做**：open 的主体是 `vfs_namei()`，它整条路径不持锁
  （§6.2.6 已经写明原因：后端 `lookup` 会走 ATAPI 阻塞 I/O，不能拿自旋锁），
  靠 BKL 保护"查 name cache + 给 vnode 加引用"这一个原子动作。要摘它，先得
  设计**路径级锁**（或把 name-cache 查询与 vnode 引用合成一个操作），
  这是独立一块工作，不适合塞进这一步。硬塞的结果是 namei 与 `vn_free()`
  抢同一个 vnode，正是 §7.2.1 结尾提醒的那类复活已释放 vnode 的 bug。
- **close 的改造**：`fd_close()` 拆成 `fd_close_common(fd, bklfree)`：
  1. `fd_lock` 内**先认领**描述符——把 `pipeidx`/`rw`/`vp` 拷进局部变量，
     然后 `fp->vp = 0`、`fp->pipeidx = -1`、`fp->used = 0`。一次放锁就把
     槽位还回去，后面只用局部值，不怕槽位被别人复用。
  2. `pipe_lock` 里减读写端计数、按需 `pipe_wake_{reader,writer}_locked()`
     唤醒对端（EOF/EPIPE）；最后 `vn_put(vp)`。
  3. 这些只需要 `pipe_lock` 和 `vfs_lock -> pc_lock / nc_lock`，都不需要
     BKL，所以 `bklfree == 1` 时在它们外面 `smp_bkl_drop()/enter()`。
- **`bklfree` 只给 close(2) 传 1**：
  - `exec` 的 fd 清扫传 0——那里正在替换进程映像，BKL 在做有用的事。
  - `dup2` 也传 0——它需要在 `fd_close(to)` 之后**紧接着**用
    `cur->uk_fds[to] = cur->uk_fds[from]` 覆盖同一个槽位；中途放 BKL 的话，
    另一个核可能先把 `to` open 走了，赋值就盖掉了别人的 fd。close(2) 没有
    这个"关掉再立刻重写"的序列，可以安全放开。

**验证（2026-11-28）**：全套 26 套 **24 PASS + 2 已知失效**；
8 核 3 GB 基线全过，**0 TRAP / triple fault / STACK OVERFLOW /
address space busy**，`exit 0`。

### 6.2.17 P2 第十二步：open(2)/namei 摘 BKL（§7.2.1 第 7 条收口）（2026-11-28）

**§7.2.1 的 7 条到此全部完成。** 这一步没有引入任何新锁类型，也没有上
turnstile——不是偷懒，而是重新算了一遍发现**不需要**。

- **为什么不需要路径级锁**：§6.2.16 里的顾虑是"查 name cache + 给 vnode
  加引用只能靠 BKL 原子"。其实 §6.2.8 之后命中侧已经是原子动作
  （`namecache_lookup()` 在 `vfs_lock -> nc_lock` 里校验 `v_id` 后
  `vn_ref_locked()`）。真正缺的是**回填**那一半：老的 `namecache_enter()`
  是无条件覆盖，两个核同时 miss 同一个名字时各自拿到一个 vnode，于是同一个
  文件有两个句柄、两套页缓存键。把"查到就复用、查不到就插入"合成一个临界区
  （`namecache_enter_unique()`）之后，命中与回填两侧对称，路径级锁就多余了。
  §5.2 的四条前提（可睡眠锁 + 锁嵌套 + 多 donor + 实时性）也都不成立。
- **改动清单**：
  1. `namecache_enter()` → `namecache_enter_unique()`（`vfs_cache.c` /
     `vfs.h`）：在池锁 + 名字缓存锁内先校验既有表项，有效就
     `vn_ref_locked()` 并返回它、调用者 `vn_put()` 自己刚分配的那个；否则
     插入并返回传入的 vnode。返回值恒为"已加引用"，不等于 `vp` 时调用者
     负责释放——**发布结果与取引用是同一个原子动作**。
  2. `vfs_namei()`（`vfs.c`）：改用上面的形式；**修掉错误路径的引用泄漏**
     （新的 `namei_put()`，以前 lookup 失败时父目录的引用会漏掉一个，
     `VNODES` 只有 96 个槽，多核并发遍历下立刻就会耗尽）；把三条不变式写进
     注释：每步持有正在搜索目录的引用 / 发布与取引用原子 / 后端 lookup
     不持核心锁。
  3. `vfs_create()`：purge 之后也用 `namecache_enter_unique()` 发布，
     两个核并发创建（或创建与查找）同一个新名字时共用一个 vnode。
  4. **ISO9660（`isofs.c`）删掉文件级 `secbuf`**：`find_in_dir()` /
     `iso_readdir()` / `iso_mount()` 改成就地在 `bread()` 的块里解析、扫完
     `brelse()`。这是摘 BKL 的硬前提——原来两个并发 lookup 会互相覆盖扇区
     缓冲，可能返回**错 extent 的 vnode**（比死锁更难查）。
  5. **tmpfs（`tmpfs.c`）**：`tmpfs_lookup_op()` 在锁外 `vn_alloc()` 之后
     **重新校验**这个名字仍落在同一个槽；否则 remove/rmdir + create 复用槽
     的窗口里，会交出一个描述"别的文件"的 vnode。
  6. **`open(2)`**（`usermode.c`）：路径解析抽成 `open_resolve()`，
     `vfs_namei_bklfree()` 为真时 `smp_bkl_drop()/enter()`；`/dev/*` 与
     `/srv/` 分支不动；`O_TRUNC`/`O_APPEND`/`fd_alloc_vnode` 仍留在 BKL
     内（都是内存操作，快）。失败映射保持原样：不带 `O_CREAT` 一律 `ENOENT`。
  7. **fd 表加固（顺带修 §6.2.16 留下的窗口）**：close(2) 在 BKL 外交还
     槽位之后，"扫描槽位 + 填字段"与"把一个描述符拷成另一个再加引用"都必须
     持 `fd_lock`：5 个 `fd_alloc*`、open 的 `/dev` 分支、`dup`、`dup2`、
     `fcntl(F_DUPFD)`、`fork` 的 fd 拷贝。否则 `vn_ref()` 会**复活**一个
     已经被 close 释放的 vnode（`dup2` 的"先查 from 再 fd_ref"序列最典型）。
     为此把 `fd_lock` 的声明从 pipe 段前移到 fd 段之前。
- **门控**：`vfs_namei_bklfree()` 只在根后端是 ISO9660 / tmpfs 时为真。
  JLFS / FFS 没有节点锁，安装到硬盘的系统上 `open(2)` 仍在 BKL 内——与
  `write(2)` 对后端设的门槛完全一致。`vfs_namei()` 本身对两种用法都正确，
  门控只决定"要不要放 BKL"。
- **已知限制（记录在案，不在这一步修）**：
  - `unlink` 一个仍被打开的文件后复用其 tmpfs 槽，既有 fd 会指向新文件
    （缺节点引用计数，摘 BKL 之前就存在）。`tmpfs_lookup_op()` 的重新校验
    只覆盖"lookup 与 remove 竞争"这一个窗口。
  - `read(2)` 的伪设备分支只看 `uk_fds[fd]` 的几个标量、不持 `fd_lock`，
    与 BKL 外的 close(2) 并发时最坏是重渲染一份快照，无内存安全问题。
- **并发证据**：`libexec/rdstress.c` 扩了一段"共享一个 tmpfs 名字做
  open(O_CREAT|O_TRUNC)/write/close/unlink"的循环，与原有的
  "4 子进程 × 32 次 open/read/close" 并行跑——前者正对着"一个名字两个
  vnode""lookup 撞 remove"两个新窗口。

**验证（2026-11-28）**：全套 26 套 **24 PASS + 2 已知失效**（`test_subsys`/
`test_ipl`，与基线逐套一致，无回归）；8 核 3 GB（`--smp 8`，含 `smpon` +
`smpuser`）`rdstress` / `pthread` / `pthmain` / `pthpre` / `aiotest` 全 PASS，
**0 TRAP / triple fault / STACK OVERFLOW / address space busy**，`exit 0`。
注：`tools/qdrive.py` 偶发打印 `boot timeout (no monitor prompt)`——QEMU 的
`-serial ... wait=off` 不等客户端连接，客人先跑起来时提示符已经滚过去了；
它只影响日志开头是否完整，命令仍会照发，判据看日志里的 PASS 行。
注：QEMU 不在 `PATH` 里时 `qdrive.py` 会直接
`FileNotFoundError`（它按名字起进程），本机路径是
`C:\msys64\ucrt64\bin\qemu-system-i386.exe`，跑之前先
`export PATH="/c/msys64/ucrt64/bin:$PATH"`。另外 `--log` 是**追加**写的，
重跑前记得先删旧日志，否则会把上一次的输出混进来。

**一次未复现的挂起（记录在案）**：`reg8d` 那次 8 核基线停在
`execve /bin/aiotest: entry ...` 之后，五秒内 QEMU 的 CPU 时间几乎不涨
（354.83 → 354.86），`qdrive` 超时退出。当时机器上**同时跑着第二个
qemu + qdrive**（端口/CPU 争抢，两份日志都是空的），随后单独连跑 6 次
`/bin/aiotest` 全部 PASS（`token=3..12`），再跑一次完整 8 核基线
（`reg8e`，独占机器）也 PASS（`token=1/2`，`[user exit 0]`）。
倾向判断是并发跑测导致的调度饥饿，而不是 `open(2)` 改动引入的死锁——
`aiotest` 走的是 `aio_wait_sys()` 阻塞 + `softirq` 唤醒，与 namei 无关；
但**没有定位到根因**，后续开 `smp_user_dispatch` 默认值前应补一次
长时间 soak 观察（见 §7.1）。

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

- **已积累的 soak 证据（2026-11-26，仍未到"数小时"）**：
  - 03:20-04:05：6 轮 × 24 循环，130 个循环，0 异常；
  - 05:43-06:36（53 分钟）：8 轮 × 24 循环 = **192 个循环 × 6 条命令**，
    boot3 夹 `proctest` 到 phase B（1002 live），**0 个 TRAP / triple fault /
    STACK OVERFLOW / address space busy**；
  - 累计约 1.5 小时。脚本 `soak_p1.sh`（在 WorkBuddy workspace，
    参数：轮数、每轮循环数；正式跑用 `bash soak_p1.sh 20 30` ≈ 3 小时）。
    注意：qdrive 的 `--log` 是追加写，每轮前脚本已清空；整轮只开一个 shell，
    不要每轮 `exit`。
- **坑**：若 soak 中复现，按 `os_smp.md` §12/§15 的反汇编方法
  （`cmpl $1,(%eax)`、崩溃点 `cpus[cid].ci_curthread`）重新取证；**不要靠
  "再加一把锁"规避**——S422 就是池锁 vs BKL 锁序反转导致硬死锁（时钟静止）。
  `proctest` 在 TCG 下回收 1000 子进程极慢，只当间歇负载，别拿它的停顿判定死锁。

### 7.2 P2 — #10 第 ④ 步：VFS / pipe / pagecache / fd 表分锁

> 目标是把系统调用入口的 BKL 逐路径摘掉；**顺序必须是先给子系统加锁，再摘
> BKL**，反过来必炸。第 ③ 步（7.3）排在它之后。
> **进度（2026-11-26）**：VFS vnode 池、pipe 表、page cache、buffer cache、
> name cache、fd 表都已加锁；**普通文件 `read(2)` 已摘掉 BKL**（§6.2.9），
> 其余系统调用仍在 BKL 内。
> **进度（2026-11-28）**：§7.2.1 的 7 条**全部落地**
> （`lseek`+`fstat` §6.2.10、`/dev` 缓冲锁 §6.2.12、tmpfs 节点锁 §6.2.13、
> pipe §6.2.14、console 读 §6.2.15、`close(2)` §6.2.16、`open`/namei
> §6.2.17）。第 7 条原本预计要上 turnstile，实际**没有引入新锁类型**：
> 把"发布 lookup 结果 + 取 vnode 引用"合成为 `namecache_enter_unique()`
> 一个原子动作后，路径级锁就不需要了（详见 §6.2.17）。
> 注意：8 核基线里的 `smpuser` 一直把 `smp_user_dispatch` 打开，所以上面
> 每一步的 8 核验证**都是在 AP 派发用户线程的前提下**跑出来的。
> **下一步看 [§7.2.1](#721-下一步可直接动手2026-11-26-交接)**；P1 仍未收口，
> `smp_user_dispatch` 默认值还是 0，等 §7.1 的数小时 soak。

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

#### 7.2.1 下一步（可直接动手，2026-11-26 交接）

已经做完的：`vfs_lock`（vnode 池）、`pipe_lock`、`pc_lock`（页缓存）、
`bio_lock`（缓冲缓存）、`nc_lock`（名字缓存）、`fd_lock`；系统调用里
**普通文件 `read(2)`**（§6.2.9）、**`lseek` 与 `fstat`**（§6.2.10）已摘掉
BKL。下面按"风险从低到高、每次只动一条路径"排好序，
**每做完一条都要跑 §0.4 的全套 + 8 核基线，再进下一条**。测试已改为
并行驱动（§6.2.11，全套 59 s），一轮验证的成本已很低。

1. **`lseek`（最小一步，纯 fd 表）** ✅ 已完成（§6.2.10）：`usermode.c` 里
   `lseek` 只改 `uk_fds[fd].off`，套上 `fd_lock` 后 `smp_bkl_drop()`；
   已建立"两条路径离开 BKL"的模板。
2. **`fstat` / `stat`** ✅ `fstat` 已完成（§6.2.10）：只读 vnode 的 `getattr`，
   vnode 由 `vfs_lock` 保护；`stat` 仍走 namei，**尚未摘**。
3. **`/dev` 伪文件 read** ✅ 已加锁（§6.2.12）：`FD_ZERO`/`FD_NULL`/`FD_VMSTAT`/
   `FD_MOUNTS`/`FD_PS`，新增 `devbuf_lock` 保护文件级静态缓冲（`ps_snap_buf`、
   `mbuf`、`vbuf`）。**未摘 BKL**：跨 `smp_bkl_enter()` 持 `devbuf_lock` 会
   反转锁序造成 S422 型死锁；要摘需先把渲染改成每次调用自己的缓冲。
4. **tmpfs 文件 read/write** ✅ 已完成（§6.2.13）：真正的 `/tmp` 后端是
   `kernel/fs/tmpfs.c`（`usermode.c` 的 `FD_TMPFILE` 是无调用者的死代码）。
   新增 `tmpfs_lock` 覆盖全部节点字段，"分配数据页 + 改 `n->size`"同一临界区，
   `pagecache_purge_vp()` 移到锁外；`write(2)` 已按 `read(2)` 模板摘 BKL
   （只放行 tmpfs，JLFS 仍由 BKL 串行）。顺带修掉一个已在线上的缺锁 bug：
   `readraw` 早就在 BKL 外跑，却没有任何节点锁。
5. **pipe read/write** ✅ 已完成（§6.2.14）：`pipe_lock` 在；阻塞路径改为在
   `kthread_block_tcb()` **之前**就 `smp_bkl_drop()`（`smp_bkl_drop()` 嵌套
   安全，`proc_sched()` 里的再次释放退化为空操作），唤醒契约
   （`uk_regs`/`uk_pipeidx`/`uk_piperw`/`uk_pipeat`/`uk_pipelen`/`rpid`）
   在锁内、阻塞前一次性发布。
6. **console/tty read/write** ✅ 读端已完成（§6.2.15）：`console_read()` 只在
   轮询 `cngetc()` 的那一段放 BKL——原来停在提示符的 shell 会持着 BKL 无限期
   轮询，把其它核全饿死。键盘环形缓冲**不需要新锁**（`pccngetc()` 用
   `spltty()` 屏蔽 IRQ）。写端仍未摘：`crt_row/crt_col`/`klog_len` 本来就没有
   锁，且 `panic()` 也走 `printf()`，给 `pccnputc()` 加锁必须先做"panic 无锁
   旁路"。
7. **`close` / `open`** ✅ 两条都已完成：
   - **`close(2)`**（§6.2.16）：`fd_close()` 拆成
     `fd_close_common(fd, bklfree)`，`fd_lock` 内认领描述符并取出
     `vp/pipeidx/rw` 到局部，放锁后 `pipe_lock` 计账 + `vn_put()`，在 BKL 外
     执行。`exec` 清扫与 `dup2` 传 0（后者"关掉再立刻重写同一槽位"，
     中途放锁会被别的核抢走）。
   - **`open(2)` / namei**（§6.2.17）：路径级锁**没有引入**——
     `namecache_enter_unique()` 把"查到就复用 / 查不到就插入"和取引用
     合成一个 `vfs_lock -> nc_lock` 临界区，命中侧 `namecache_lookup()`
     本来就是这个形状；再补上 ISO9660 的静态扇区缓冲（改成就地解析
     `bread()` 的块）、tmpfs lookup 的槽重新校验、错误路径的引用泄漏、
     以及 fd 表所有槽位认领/引用拷贝的 `fd_lock`。`open_resolve()` 在
     `vfs_namei_bklfree()` 为真（根为 ISO9660/tmpfs）时放掉 BKL，
     JLFS/FFS 仍留在 BKL 内。

配套要做的事：
- **P1 仍未收口**：`smp_user_dispatch` 默认值还是 0。要改成 1，需要一次
  **3 小时以上**的 8 核混合负载 soak（`soak_p1.sh`：`bash soak_p1.sh 20 30`
  约 3 小时），确认 TRAP 12 不再出现。现在可用的最强负载是
  `rdstress`（并发读）+ `proctest`（1000 进程）+ `pthread/pthpre`。
- **每摘一条路径就扩大一次 `rdstress`**：把新路径也写进
  `libexec/rdstress.c`（例如加上 lseek/stat 的循环），让并发证据跟着代码走。


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
