# 汇编源码目录规则（asmble）

> 工程规则，适用于本仓库所有工程（`bsd45/` 及后续新增工程的内核树）。
> 建立日期：2026-11-18。最近一次收紧：2026-11-28（S432）。

## 规则

**内核（`kernel/`）下的所有汇编代码，无论是独立汇编源文件还是 C 里的内联汇编，
只允许放在 `arch/` 目录树下。**

- 允许：`kernel/arch/<arch>/` 及其子目录，例如
  `kernel/arch/i386/`、`kernel/arch/i386/isa/`、`kernel/arch/sparc/`。
- 禁止：`kernel/sys/`、`kernel/thread/`、`kernel/lib/`、`kernel/fs/`、
  `kernel/vm/`、`kernel/driver/`、`kernel/include/sys/` 等任何非 `arch/` 目录。
- 受管内容有两类，**两类都算违规**：
  1. 独立汇编源文件：后缀 `.s`、`.S`、`.asm`；
  2. C 源或头文件里的内联汇编：`__asm` / `__asm__` / `asm volatile` /
     `asm(...)`，包括只做编译器屏障的空汇编 ``__asm__ __volatile__("")``。
- 不违规：注释里提到汇编（例如"此函数由 arch 汇编实现"）；通用头文件里
  **声明**一个由 `arch/` 实现的函数原型。

## 为什么

1. **架构相关性**：汇编直接操作寄存器、栈帧、特权指令、上下文切换，
   本质上与 CPU 架构绑定。放进 `arch/<arch>/` 才能让移植新架构时
   只替换一个目录。
2. **构建隔离**：构建脚本按 `ARCH_<arch>` 选择 `arch/<arch>/` 下的汇编
   与 C 源；汇编散落在通用目录会让"换架构"变成到处改路径。
3. **审阅边界**：看到 `arch/` 就知道要按目标架构分别验证；看到通用目录
   就默认它应该是可移植 C。汇编出现在通用目录通常是设计走样的信号。
4. **内联汇编比独立文件更隐蔽**：一个 `.s` 文件很容易被 `find` 出来，
   而 ``__asm __volatile("str %0")`` 藏在通用头文件的 `static __inline` 里，
   移植时既不会被构建脚本筛掉，也不会被 `find` 命中——它会被静默地编译
   成错误的指令序列。这是 2026-11-28 收紧规则的原因（见下）。
5. **与当前内核模型一致**：本内核已把端口 I/O、CPU 控制、上下文切换钩子
   分别收敛到 `arch/i386/io.h`（声明）+ `arch/i386/x86.asm`（实现）、
   `arch/i386/ctx.c`、`arch/i386/timer.c`，本规则是同一原则的延续。

## 当前状态审计（2026-11-28：两类都合规）

`bsd45/kernel/` 下：

- `arch/` 之外**无** `.s/.S/.asm` 文件；
- `arch/` 之外**无**任何内联汇编，包括 C 头文件里的 `static __inline` 版本；
- 全树内联汇编只出现在 `arch/i386/atomic.c`、`arch/i386/smp.c`（`str`）两处。

检查命令见下节，`bash tools/check-asm.sh` 输出 `PASS` 即合规。

### 合规清单（全部在 arch/）

| 文件 | 说明 |
|---|---|
| `kernel/arch/i386/ap_tramp.s` | AP 启动 trampoline（S200 已加入构建） |
| `kernel/arch/i386/usermode_asm.s` | `resume_user` / `irq0_entry`（2026-11-20 从 `sys/` 迁入） |
| `kernel/arch/i386/kswitch.s` | 内核栈切换（2026-11-20 从 `thread/` 迁入） |
| `kernel/arch/i386/i386/locore.s` | 引导早期、页表、上下文切换汇编 |
| `kernel/arch/i386/isa/locore.s` | 机器相关低层例程 |
| `kernel/arch/i386/isa/icu.s` | 8259 中断控制器 stub |
| `kernel/arch/i386/x86.asm` | 端口 I/O、CPU 控制（`hal_inb`、`cpu_idle` 等） |
| `kernel/arch/i386/smp_ipi.s` | IPI 入口 stub |
| `kernel/arch/i386/atomic.c` | `lock` 前缀原子指令 + `mfence/lfence/sfence` |
| `kernel/arch/i386/smp.c` | `cpu_id()` 的 `str`（S432 迁入） |
| `kernel/arch/sparc/{mul,rem,sdiv,udiv,umul,urem}.s` | SPARC 算术（2026-11-20 从 `lib/sparc/` 迁入，不参与 i386 构建） |

### 2026-11-20 整改记录

1. **S200 同步原语**：`sys/sync.h` 初稿把 atomic 用 `__asm` 内联写在通用头里，
   违反本规则。已迁出：`kernel/arch/i386/atomic.c`（实现）+ `atomic.h`（声明），
   `sync.h` 仅保留数据结构/函数声明，`#include "arch/i386/atomic.h"`。
2. **历史遗留 8 个汇编文件全部迁移**：`usermode_asm.s`、`kswitch.s` →
   `arch/i386/`；6 个 SPARC `.s` → `arch/sparc/`。同步更新了
   `build_kernel.sh`/`.ps1`、`kernel/{sys,thread,arch/i386}/config.mk` 的引用，
   README 相关描述一并订正。
3. **验证**：Windows（gcc→PE→COFF→objcopy→ELF）完整重编译无
   `undefined reference`；`synctest`/`ipitest`/`kthreadtest` PASS（证明
   `kswitch.s` 栈切换正常），`test_s3` PASSED（证明 `usermode_asm.s`
   的 `resume_user` 正常进用户态 shell）。

### 2026-11-28 整改记录（S432，规则收紧到内联汇编）

2026-11-20 那次只清理了**文件后缀**，通用头与系统调用文件里的内联汇编仍有
四处漏网（`find` 审计看不见它们）。本轮全部清理，并把规则本身改成覆盖内联汇编：

| 原位置 | 内容 | 迁往 |
|---|---|---|
| `sys/smp.h` `cpu_id()` | `str %0` 读任务寄存器 | `arch/i386/smp.c`，头文件只留 `int cpu_id(void);` |
| `sys/smp.h` `atomic_cmpxchg/xchg/add` | `lock cmpxchgl` / `xchgl` / `lock xaddl` | `arch/i386/atomic.c`（`atomic_cmpxchg/atomic_xchg/atomic_fetch_add`） |
| `sys/usermode.c` `page_ref_add()` | `lock xaddw`（16 位引用计数） | `arch/i386/atomic.c` 的 `atomic_xadd16()` |
| `sys/usermode.c` 空闲循环 | `sti; hlt` | `arch/i386/x86.asm` 的 `cpu_idle()`，经 `hal_cpu_idle()` 调用 |

同时把 ARCH 选择（`#if defined(ARCH_i386) #include "arch/i386/atomic.h"`）
从 `sys/sync.h` 移到 `sys/smp.h`——`kthread.c` 也要用原子，而它已经包含
`smp.h`，这样只在一处做架构选择。

**语义不变**：`atomic_add()` 返回加之后的值（C11 约定），
`atomic_fetch_add()` 返回加之前的值；原先 `sys/smp.h` 里的 `atomic_add`
返回旧值，三处调用点（`sched_switches`、两处 `next_pid`）改用
`atomic_fetch_add()`，所以 PID 分配顺序逐字节不变。

**验证**：26 套验收 24 PASS + 2 已知失效（`test_subsys`/`test_ipl`，
与基线逐套一致）；8 核 3 GB 基线 `rdstress`/`pthread`/`pthmain`/`pthpre`/
`aiotest` 全 PASS，0 TRAP。

## 执行要求

1. **新增汇编**：一律写入对应 `kernel/arch/<arch>/`，不得放在其他目录。
   通用 C 里需要一条指令时，在 `arch/<arch>/` 里加一个函数
   （参考 `atomic_xadd16()`、`cpu_idle()`），不要在调用点内联。
2. **迁移既有文件**：移动源码的同时，必须同步更新所有引用它的构建脚本
   （`build_kernel.ps1`/`.sh`、`build_iso.ps1`/`.sh` 及任何 `config.mk`），
   并重新构建 + 跑回归测试，确认无 `undefined reference`。
3. **C 侧声明**：汇编实现的函数原型可以留在通用头文件（例如
   `sys/smp.h` 的 `cpu_id()`、`sys/platform.h` 的 `hal_*`），但**实现必须在
   `arch/`**；且通用头里不得出现 `__asm`。内联版本（`static __inline`）
   一律视为违规。
4. **例外**：确需例外的，必须在本文件"例外清单"中登记理由与评审人，
   否则视为违规。

## 检查方法

```bash
# 一键检查（两类违规都查，PASS 才算合规）
bash tools/check-asm.sh

# 等价手写命令
find kernel -type f \( -name '*.s' -o -name '*.S' -o -name '*.asm' \) \
  | grep -v '/arch/' || echo "OK: 汇编文件都在 arch 下"
grep -rnE --include='*.c' --include='*.h' \
  '(__asm__|__asm[[:space:]]|asm[[:space:]]+volatile)' kernel \
  | grep -v '/arch/' || echo "OK: 内联汇编都在 arch 下"
```

`tools/check-asm.sh` 退出码：0 合规、1 违规、2 路径不存在。新工程建立时，
把它加进 `verify.sh` / `verify.ps1` 或 CI。

## 例外清单

（暂无。新增例外须写明文件、理由、评审人、日期。）
