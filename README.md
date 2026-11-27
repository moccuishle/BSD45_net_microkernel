# BSD45 — 4.5BSD-Lite2 i386 内核（KLT / 优先级调度版）

基于 4.5BSD-Lite2 `sys/i386` 的可启动 i386 内核：交叉编译生成 **ELF32
(Intel 80386)** 内核，封装成 **El Torito 无仿真引导** 的 ISO。内核可由
光盘启动，也可从 ISO 安装到 VMware 虚拟磁盘后直接从磁盘启动。

> **当前模型（S28+）**：内核态进程 / 线程模型（KLT）——
> 只有 `struct process`（进程 = 地址空间/资源容器）和 `struct kthread`
> （线程 = 唯一调度单位、独立内核栈），**没有 `struct uproc`**；
> 用户态不持有任何进程/线程模型，`pthread_*` 只是 syscall 包装。
> 20 级优先级（0 最低、19 最高）+ 抢占 + 优先级继承 + estcpu/nice
> 交互性评估。

![BSD45 启动到 BSD /bin/sh：ls /bin、/tmp 读写与管道](screenshot.png)

> 截图是 QEMU 串口里的实际会话（S9 验收场景）：`init` 启动 `/bin/sh`、
> `ls /bin`、`echo vfs-ok > /tmp/a`、`cat /tmp/a`、`echo hi-vfs | cat`、`exit 0`。

## 从哪看起

| 内容 | 位置 |
|---|---|
| 工程主目录（构建脚本、boot、kernel、libc、vmx） | [`bsd45/`](bsd45/) |
| 工程说明（构建、boot 链、VFS 设计） | [`bsd45/README.md`](bsd45/README.md) |
| **任务恢复文档（最重要，先读这个）** | [`bsd45/RESUME-TASK.md`](bsd45/RESUME-TASK.md) |
| KLT 手动测试指南 | [`bsd45/vms/README-KLT-TEST.md`](bsd45/vms/README-KLT-TEST.md) |
| ISO→虚拟磁盘安装指南 | [`bsd45/vms/README-INSTALL.md`](bsd45/vms/README-INSTALL.md) |
| 缓存 / mmap / 地址空间设计 | [`docs/cache-mmap.md`](docs/cache-mmap.md)、[`os_vm.md`](os_vm.md) |
| 调度 / 优先级实现 | [`docs/kernel-preemption.md`](docs/kernel-preemption.md)、[`os_sched.md`](os_sched.md) |
| SMP / 多核适配（拓扑、AP、每核 TSS、多核调度、IPI） | [`os_smp.md`](os_smp.md) |
| 文件系统实现清单（ISO9660/FFS/JLFS/tmpfs） | [`os_fs.md`](os_fs.md) |
| 进程与线程实现（proc/TCB、fork/exec、KLT） | [`os_thread-process.md`](os_thread-process.md) |
| 设计与路线（历史规划） | [`docs/object-kernel.md`](docs/object-kernel.md)、[`os.md`](os.md) |
| Windows 环境安装记录（MSYS2 + qemu） | [`ai_install_gccenv.md`](ai_install_gccenv.md)、[`hole_trap.md`](hole_trap.md) |

> 注：`docs/object-kernel.md` 是早期的**对象内核设计规划**，描述的目标
> （对象管理器、统一命名空间、API personality 等）**尚未在内核中实现**，
> 不代表当前代码状态。

## 已实现的模块（当前代码）

按子系统列出**已在源码中存在并参与构建/验证**的模块；未实现或已删除的见下一节。

### 进程 / 线程 / 调度（内核态 KLT）

- `struct process`（`kernel/include/sys/kprocess.h`）：内核态进程对象，持有地址空间
  `uk_pd`、fd 表、信号、mmap、线程链表 `uk_p_threads`；BSD 原字段保留以兼容
  genassym/machdep。
- `struct kthread`（`kernel/thread/kthread.{c,h}`）：内核线程 = 唯一调度单位；
  每线程独立 16 KiB 内核栈（`KSTACK_VA`）、2 MiB 用户栈、TCB 含上下文/状态/优先级；
  线程数与堆/mmap 由启动内存规划按总内存动态推导（S411）。
- 显式 run queue（TCB 入/出队）、`proc_sched()`/`sched_tick()` 调度器。
- 20 级优先级（0 最低 / 19 最高）：高优先级抢占低优先级；优先级继承
  （`prio_inherit`/`prio_restore`，含传递链，join 场景接入，释放后恢复）。
- BSD 交互性评估：`estcpu`（进程 CPU 记账 + 衰减）+ `p_nice` 参与优先级；
  I/O 唤醒提升、饥饿提升；差异化时间片（CPU 密集 8 / I/O 2 / 基准 4 tick）。
- 禁抢占语义：`preempt_disable/enable` + `t_preempt_cnt`；用户态可抢占、
  内核临界区不可抢占、抢占发生在回用户态安全点；自旋锁接入禁抢占。
- syscall 446/447 = `sched_setprio`/`sched_getprio`。
- `kernel/include/sys/kproc.h`、`sys/platform.h`（struct sysregs 与平台钩子）。

### 内存管理

- `kernel/arch/i386/isa/pmap.c`：Mach pmap、页表、COW 地址空间克隆。
- `kernel/vm/pagecache.c`：4K 文件页缓存。
- `kernel/vm/zcomp.c`：内存压缩池（无 swap 设备时的回收层）。
- `user_page_fault()`：demand-zero 缺页、COW 写时复制、zcomp 压缩页换入。

### 文件系统 / IO

- `kernel/fs/vfs.c` + `vnodeops`：统一 VFS 层（ISO9660 与 tmpfs 共用）。
- `kernel/fs/isofs.c`（ATAPI CD 只读）、`kernel/fs/tmpfs.c`（可写 `/tmp`）。
- 缓存系统：`fs/vfs_bio.c`（2K buffer cache + 预读）、`fs/vfs_cache.c`（name cache）、
  `vm/pagecache.c`（页缓存）；`mmap/munmap/mprotect/msync`。
- `kernel/fs/ffs/ffs.c`：只读 FFS (UFS1) 解析器，用于从磁盘启动。
- `kernel/fs/ffs/install.c`：板载 `install` 命令，把 ISO 内 FFS/内核写进虚拟磁盘。
- `kernel/fs/aio.c` + uring：异步 IO。
- `kernel/driver/atapi.c`、`ata.c`（PIO ATA 磁盘，含写）、`pccon.c`（VGA+COM1）。
- `/dev/null|zero|tty|mounts|ps|vmstat` 等设备。

### 中断 / 时钟 / HAL

- 8259 PIC + IDT，`clock.c` 对齐的 PIT 100Hz 时钟（`arch/i386/timer.c`）。
- `arch/i386/io.h` + `x86.asm`：端口 I/O 与 CPU 控制（原 `hal.h` 内容）。
- `arch/i386/ctx.c`：上下文切换钩子（TSS esp0、`kswitch`、地址空间、系统调用门）。

### SMP（S413：每核上下文 + TSS + TCB 归属 + AP 调度）

- `kernel/include/sys/smp.h`、`kernel/arch/i386/smp.c`：CPUID 动态拓扑
  （逻辑 CPU / 物理核 / SMT / HTT）、`MAXCPU=16`、每 CPU TSS、Local APIC、
  INIT-SIPI-SIPI、IPI/TLB shootdown 统计。CPU 身份由 `str`（TSS 选择子）
  解码，没有「当前 CPU」全局变量。
- `kernel/arch/i386/ap_tramp.s`：独立链接在物理 0x7000 的实模式 trampoline；
  `kernel/arch/i386/smp_ipi.s`：IPI 中断 stub。
- 调度器：`cur/cur_thread/cur_pd/need_resched` 全部 per-CPU；TCB 用
  `t_oncpu` 原子归属，一个 TCB 只在一个核上跑；AP 调 `proc_sched()`，
  用户进程/线程可并行跑在多个核上。内核态用一把大内核锁（BKL）串行化，
  用户态无锁并行；TCB 槽位延迟回收，内核栈带溢出金丝雀。
- 开关：`smpon`（AP 上线）+ `smpuser`（AP 是否参与用户调度，默认关；
  `os_smp.md` §5.2 记录了仍存在的 TRAP 12 与排查方向）。
- monitor：`cpuinfo` / `smpon` / `smpuser` / `smpping` / `smpinfo` /
  `memplan`。详见 [`os_smp.md`](os_smp.md)。

### 用户态 / 工具链 / 安装

- `libc/arch/bsd/`：4.5BSD `/bin/sh` 及 libc 子集。
- `libexec/`：echo/ls/cat/mkdir/rm/cp/mv/… 用户态工具；pthread/schedtest/
  pthpreempt/spin/cowtest 等测试程序。
- `hdd/` 三个 Python 工具各有等价 PowerShell / Bash 版（`mkffs`/`mkdisk`/
  `make_install_ffs` 的 `.py`/`.ps1`/`.sh`），**构建不依赖 Python**。
- 构建脚本双份：`*.ps1`（Windows：gcc→PE→COFF→objcopy→ELF）与 `*.sh`
  （Ubuntu：`ld -m elf_i386` 直链 ELF）。
- ISO→VMware 虚拟磁盘安装：`vms/BSD45-install.vmx`（挂载 ISO + 空盘）、
  `vms/BSD45-diskboot.vmx`（从已装好的盘启动）。

## 已删除 / 未实现的模块（不要在这找）

| 名称 | 状态 | 说明 |
|---|---|---|
| `struct uproc` | **已删除** | 进程模型统一到内核态 `struct process` |
| `kernel/hal/`（IPL/抢占层） | **已删除** | 能力移至 `io.h`/`ctx.c`/`timer.c`，软中断改为延迟队列 |
| `kernel/arch/i386/hal/` | **已删除** | 同上 |
| `kernel/subsys/`（unix/posix/linux/win32 personality） | **已删除** | syscall 直连 `bsd45_syscall()`；`test_subsys.ps1` 已失效 |
| 对象管理器 / 统一命名空间 `\` / 句柄表 / 注册表 | **未实现** | 仅 `docs/object-kernel.md` 的规划，无代码 |
| 16 级 IPL (`kernel/hal/ipl.c`) | **已删除** | 由 20 级优先级调度取代 |
| `kernel/obj/`、`kernel/other/` | **已删除/合并** | 内容并入 `lib/`、`sys/` |
| `bsd45/src/`（原始 tarball 解包树） | **已删除** | 已合并进 kernel/libc，构建不需要 tarball |
| 完整 SMP | **部分实现** | AP 上线 / 每核 TSS / 多核用户调度已可用；`smpuser` 默认关，见 `os_smp.md` §5.2 |

## 快速构建

```powershell
cd bsd45
$env:HTTPS_PROXY=""; $env:https_proxy=""; $env:HTTP_PROXY=""; $env:http_proxy=""
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test_s9.ps1
```

```bash
cd bsd45
bash build_all.sh
bash tools/test_s9.sh
```

> Windows 构建前需清空代理变量，否则 `Start-Process` 因大小写重复键抛异常。

产物：`bsd45/out/BSD45-ELF-x86.iso`（约 8.5 MB，El Torito 引导）和
`bsd45/build/bsd45.elf`。

启动：

```powershell
& "C:\msys64\ucrt64\bin\qemu-system-i386.exe" `
  -cdrom bsd45\out\BSD45-ELF-x86.iso -boot d -m 64 -serial stdio
```

## 目录结构（顶层）

```
bsd45/
  boot/            i386/（stage1、stage2、kernel.ld）+ arm/ amd64/ mips/ riscv/（占位）
  kernel/          4.5BSD sys 树 + 本项目 VFS/驱动/进程线程/调度代码
    fs/            VFS、isofs、tmpfs、ffs（含 install）、aio、缓存
    vm/            pagecache、zcomp、Mach VM
    thread/        kthread（KLT 调度主体）
    driver/        ata、atapi、pccon、dev
    include/sys/   kprocess.h / kproc.h / platform.h / smp.h 等
    arch/i386/     pmap、machdep、io.h、ctx.c、timer.c、x86.asm、smp.c、ap_tramp.s
  libc/            用户态 C 库 + libc/arch/bsd/（4.5BSD 用户态源码）
  libexec/         用户态工具与测试程序
  hdd/             FFS/磁盘镜像工具（.py/.ps1/.sh 三份）
  rootfs_overlay/  ISO 根目录覆盖层
  tools/           测试与辅助脚本（test_*.ps1 / test_*.sh）
  vms/             VMware 配置与安装/测试指南
```

## 验收

Windows 与 Ubuntu（26.04）双平台跑同一套 `*.ps1`/`*.sh`：
`test_s3`（shell）、`test_s9`（VFS/tmpfs/管道）、`test_pthread`、
`test_sched`（20 级优先级 + 继承 + 禁抢占）、`test_proc`(S5)、
`test_s10*`、`test_kthread`(S21)、`test_cache`(S17) 等均 PASSED。
手动 KLT 抢占交错测试见 `vms/README-KLT-TEST.md`。

## 待做项目（Future Work）

### turnstile 优先级传播（`os_sched.md` 问题五机制 4）

当前优先级继承是直接挂在 TCB 字段上（`t_inherit_prio` / `t_blocked_on`），
已支持"一步到位提级、多等待者取 max、链式传播、join 场景接入"，
但**没有独立的 turnstile 数据结构，也没有每把锁的 donor 队列**。
这够用于现状（内核无可睡眠锁对象、无锁嵌套、无多线程争同一把锁），
但若要升级到完整 BSD 级优先级传播，需要满足以下前提才值得上 turnstile：

1. **有真正的可睡眠锁对象**（`struct lock` / mutex，竞争时会睡眠）——
   自旋锁等者不睡眠，按 BSD 规则不使用 turnstile。
2. **存在多个线程等同一把锁**——donor 队列才有意义。
3. **存在锁嵌套**（如线程 A 持锁 1 又等锁 2）——需要链式传播 +
   逐层恢复（释放一把锁后降到"剩余等待者中的最高优先级"）。
4. **有实时性要求**（反转必须严格有界、可预测）——类似 `os_sched.md`
   提到的 Mars Pathfinder 复位事故那类场景。

只有同时具备这些条件，turnstile 的链表维护 / donor 排序 / 嵌套恢复
开销才划算；否则当前 O(1) 的 TCB 标量继承更合适。
（相关讨论见 [`os_sched.md`](os_sched.md) 问题三、问题五。）

## 恢复 / 重建

仓库自带合并后的源码与全部脚本，构建不需要任何 tarball 或外部源码树；
产物 ISO / ELF 也在仓库里，不重新编译也能直接启动。

当前代码同时推送到两个仓库（`main`）：
- `git@github.com:moccuishle/BSD45_proc-kthread_real_kernel.git`
- `git@github.com:moccuishle/BSD45_priority_sched_kernel.git`

## 许可

`bsd45/kernel/` 与 `bsd45/libc/arch/bsd/` 中的 4.5BSD 代码遵循其原始 BSD 版权
许可与 UC Berkeley advertising 条款；本项目新增的脚本、引导器与 glue 代码
可视为同一 BSD 风格许可。
