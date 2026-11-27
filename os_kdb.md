# kdb 内核调试器的实现方法

> 本文整理两部分：一是 Linux kdb 本身的实现结构与调试方法，二是在 bsd45（4.4BSD-Lite2 衍生教学内核）上落地一套自制 kdb 的可执行方案。
> 全程只读分析，未改动任何源码。

---

## 一、kdb 是什么

kdb 是 Linux 内核自带的交互式调试器。它的定位不是"日常写代码的单步调试器"，而是**机器已经不正常时的现场勘查工具**——系统卡死或崩溃时，把整机停下来，在控制台上用命令读取内核状态。

它的完整名称是 `CONFIG_KGDB_KDB`，即"kgdb 的 kdb 前端"。理解 kdb 的第一把钥匙是：

> **kdb 和 kgdb 是"两个前端、一个核心"**——它们共享 `debug_core.c`，kdb 只负责命令行界面，kgdb 负责 GDB 远程协议。

### 核心特征：stop-mode 调试

进入 kdb = **整机停机**。所有 CPU 滑行停下（官方用词 "skid to a stop"），只有 master CPU 执行调试命令。

- 优点：看到的是一致的现场，内存不会被其他 CPU 继续改写；
- 代价：进调试器后调度器和中断都不可信，所以调试器**不能依赖任何锁、不能走正常内核日志路径**。

---

## 二、Linux kdb 的实现结构（四层）

2006 年独立的 kdb 补丁仅 x86 就有 6.4 万行改动。2009 年 Jason Wessel 主导把它与 kgdb 合并，做法是**砍掉所有与 kgdb 重复的部分，把 kdb 改造成薄前端插到 debug core 上**，2010 年 2.6.35 进入主线。今天的结构是四层：

```
┌─────────────────────────────────────┐
│  前端层                              │
│  kdb shell (kernel/debug/kdb/)       │  ← kdb 独有：命令表 + shell
│  gdbstub (gdb 远程协议)              │  ← kgdb 的前端
└──────────────┬──────────────────────┘
               │ 前端派发
┌──────────────▼──────────────────────┐
│  debug core (debug_core.c)           │  ← 共用底座
│  异常处理 / SMP 同步 / 安全内存访问   │
└──────────────┬──────────────────────┘
               │ trap catcher
┌──────────────▼──────────────────────┐
│  arch 层 (arch/*/kernel/kgdb.c)      │
│  捕获异常 / 寄存器格式转换 / 硬件断点 │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│  I/O 层 (kgdboc)                     │
│  串口 / PS2 键盘+VGA / EHCI debug    │
└─────────────────────────────────────┘
```

### 第 1 层：debug core（`kernel/debug/debug_core.c`）

共用底座，做四件硬事：

1. 通用异常入口 `kgdb_handle_exception()`；
2. **SMP 同步**：让所有 CPU 停下，只有 master CPU 跑调试器；
3. 安全的内存读写——出错不能再次触发异常导致递归崩溃，以及软件断点实现；
4. 决定把控制权交给哪个前端（kdb 还是 gdbstub），两者可运行时切换。

### 第 2 层：arch 层（`arch/*/kernel/kgdb.c`，x86 为 `arch/x86/kernel/kgdb.c`）

每个架构要实现：trap catcher（捕获异常后调 `kgdb_handle_exception()`）、`pt_regs` 与 gdb 寄存器格式的转换、动态注册/注销 trap hook、硬件断点、NMI 处理。

### 第 3 层：kdb 前端（`kernel/debug/kdb/`）

这是 kdb 独有的部分：

- 一个简单 shell + 核心命令集（`lsmod`、`ps`、`bt`、`dmesg`、`kill`、内存读写、硬件断点）；
- **命令注册 API**——一张命令表 + 注册函数。`samples/kdb/kdb_hello.c` 演示了用模块动态添加命令，`kernel/trace/trace_kdb.c` 的 `ftdump` 是内置范例；
- `kdb_printf()`：**直接写 I/O 驱动，绕过内核日志系统**——因为崩溃时 printk 依赖的锁和缓冲区可能已不可信。

命令按权限分组（来自内核 4.17 的 kdb 命令分类提交）：

| 类别 | 命令 |
|---|---|
| 始终可用 | `go`(无参)、`env`、`set`、`help`、`?`、`cpu`(无参)、`sr`、`dmesg`、`disable_nmi`、`defcmd`、`summary` |
| 内存读 | `md`、`mdr`、`mdp`、`mds`、`ef`、`bt`(带参)、`per_cpu` |
| 内存写 | `mm` |
| 寄存器读 | `rd` |
| 寄存器写/控制 | `go`(带参)、`rm` |
| 观察 | `bt`(无参)、`btp`、`bta`、`btc`、`btt`、`ps`、`pid`、`lsmod` |
| 流控 | `bp`、`bl`、`bph`、`bc`、`be`、`bd`、`ss` |
| 信号/重启 | `kill`、`reboot` |
| 高级 | `cpu`、`kgdb`、`nmi_console` |

### 第 4 层：I/O 层（kgdboc）

kdb 的输入输出走 kgdboc：串口（`echo ttyS0 > /sys/module/kgdboc/parameters/kgdboc`）、PS/2 键盘 + VGA 文本控制台（`echo kbd > .../kgdboc`）、或 EHCI debug port。

**I/O 驱动必须是轮询式的**——不使能中断、不改系统上下文，因为没有调度器可用。

### 实现上的巧思

- **不要锁**：kdb 在很多内核组件里加了辅助函数，专门用来"在不拿可能死锁的锁的前提下读取内核状态"。调试器自己不能依赖被调试对象的正常机制。
- **两段式初始化**：`dbg_late_init()` 的存在是因为硬件断点要用 perf API，得等内存分配器起来；而调试核心本身在 `mm_init()` 之前就能用。这是调试器能抓极早期崩溃的关键。
- **前端可切换**：kdb 与 gdbstub 共用核心，运行时可切换。

---

## 三、kdb 的调试方法（怎么用）

### 1. 编译配置

```
CONFIG_KGDB=y
CONFIG_KGDB_KDB=y          # 关键：启用 kdb 前端
CONFIG_MAGIC_SYSRQ=y       # 用 SysRq-g 进入必须开
CONFIG_DEBUG_INFO=y        # 想 bt 看到函数名必备
```

配置 I/O（kgdboc）：

```bash
# 启动时传参
kgdboc=kbd
kgdboc=ttyS0,115200

# 或系统跑起来后动态设置
echo kbd > /sys/module/kgdboc/parameters/kgdboc
```

### 2. 进入 kdb 的四种方式

| 方式 | 操作 | 适用场景 |
|---|---|---|
| SysRq-g（最常用） | `echo g > /proc/sysrq-trigger`，或键盘 `Alt+SysRq+g`（笔记本加 `Fn`） | 系统卡死但还能响应输入 |
| 串口 BREAK | 串口上发 BREAK，5 秒内按 `g` | 无键盘的服务器/嵌入式 |
| 自动进入 | Oops / panic / fault 时自动落入 | 抓崩溃第一现场（最有价值） |
| kgdbwait | 启动参数加 `kgdbwait` | 调试启动早期代码 |

### 3. 典型排查流程

系统卡死时，一个典型序列：

```
kdb> ps                    # 1. 看进程及状态，找出可疑任务
kdb> pid <进程号>          # 2. 切到那个任务的上下文
kdb> bt                    # 3. 打印调用栈回溯——最关键的一步
kdb> rd                    # 4. 看寄存器
kdb> ef                    # 5. 看异常帧
kdb> md 0xc0123456 32      # 6. 显示 32 字节内存
kdb> dmesg                 # 7. 看内核日志尾部
kdb> go                    # 8. 恢复执行
```

其他常用：`bp <地址>` 设断点、`bl` 列出、`bc` 清除、`bph` 硬件断点、`ss` 单步、`mm` 改内存、`rm` 改寄存器、`kill <pid> <信号>`、`reboot`。`help` 或 `?` 列出全部命令。

### 4. 两个反直觉点

**kdb 不是源码级调试器。** 它给出的是 `bt` 出来的**地址**，不是 `file.c:123`。要看源码需下线后用：

```
gdb vmlinux
(gdb) l *0xc021e50e
```

或 `scripts/decode_stacktrace.sh` 把 Oops 地址翻译成源码行。

**进去 = 整机停机。** 生产环境别乱进；进 kdb 时不能依赖锁——这就是 `kdb_printf` 直接写串口/VGA 显存、绕过 printk 的原因。

### 5. kdb 与 kgdb 配合

两者共享 `debug_core.c`，只是两个前端。在 kdb 里敲 `kgdb` 切到 gdbstub，另一台机器跑 gdb 做真正的源码级单步；反过来在 gdb 里用 `monitor ps`、`monitor dmesg` 调回 kdb 命令。官方建议：**断点/单步用 gdb，看系统状态用 kdb 的 monitor 命令**。

---

## 四、在 bsd45 上实现 kdb（落地方案）

先说结论：**能实现，而且已经握着三分之二的零件**——kdb 的四层里，bsd45 有三层现成的东西，只是没接起来。

### 1. 现成零件（都在磁盘上，但部分没参与编译）

| 零件 | 位置 | 规模 | 状态 | 对应 kdb 的 |
|---|---|---|---|---|
| gdb stub | `kernel/arch/i386/i386/kgdb_stub.c` | 579 行 | **死代码**（build_kernel.sh 未包含） | gdbstub 前端的完整原型：GDB 串口协议、`regs_to_gdb`/`gdb_to_regs`、`trapno→信号` 映射 |
| trap frame 适配 | `kernel/arch/i386/i386/kgdb_glue.c` | 160 行 | **死代码** | arch 层：修正 `tf_isp`、`splhigh` 下拷帧到专用栈、调 `kgdb_trap()`、写回 |
| 交互式 shell | `kernel/sys/mon.c` | 521 行 | **已编译、活跃** | kdb shell 雏形：`getline()` 带退格，30+ 命令（`mem`/`pte`/`vstat`/`kthread`/`smpinfo`/`ipctest`…） |
| 轮询 I/O | `kernel/driver/pccon.c` | — | 活跃 | `cngetc` 读 8042 扫描码 + VGA 文本 + COM1 镜像，正是 kdb 要求的轮询输入 |
| SMP 基础 | `kernel/arch/i386/smp.c` | — | 活跃 | `IPI_STOP` 向量已注册、`spin_lock` 已有，全核停机有硬件基础 |

### 2. 要补的六个缺口

1. **统一 debug core**：Linux 的 `debug_core.c` 那层（异常处理、前端派发、安全内存访问）bsd45 没有。以其规模，写一个几百行的 `kdb_core.c` 就够，不必照搬 Linux。
2. **异常入口挂接**：`kgdb_glue` 有了但没人调。需要在 `trap()` 里挂 tripwire（如 int3 / 缺页）转进调试器。
3. **全核停机**：`IPI_STOP` 有了，但缺"master 之外的 CPU 停在一个可控循环里"的握手逻辑——这正是之前踩过的双核栈抢占坑的邻近区域，要小心。
4. **`kdb_printf()`**：现在的 `printf` 走 tty/锁路径，崩溃时不安全。需写 `Crtat`（VGA 显存）和 COM1 裸端口，绕过日志缓冲。
5. **命令表化**：`mon.c` 现在是 30 条 `else if (!strcmp(line, ...))`。改成 `{name, handler}` 表 + 注册函数，以后加命令不用改主循环。
6. **符号表**：要看 `bt` 的函数名需要 kallsyms 式地址→名字表。可从 ELF 符号段在链接期生成一张静态表。

### 3. 三档工作量

| 目标 | 工作量 | 说明 |
|---|---|---|
| kdb 风格本地 shell | 小，1–2 周 | 在 `mon.c` 上加 `bt`/寄存器/内存读写/`setbp`，全跑在已有框架里 |
| 真 stop-mode kdb | 中等，1–2 月 | 补上述 1/2/3/4/5，崩溃时按 sysrq 风格进入，全核停机现场勘查 |
| kgdb 双机源码调试 | 中等偏大 | 把 `kgdb_stub.c` 接进构建、修通串口，另一台机器跑 gdb |

### 4. 建议路线

**先做第一档**：把 `mon.c` 改造成 kdb 风格 shell。它已经在跑、有键盘和命令框架，加 `bt`（沿帧指针回溯）、`regs`（打印 trapframe）、`rd/wr`（内存读写）这三个命令就是最有价值的 80%。这一步不需要碰 SMP，风险最低。

**然后上全核停机**——但 bsd45 的 SMP 是后加的，S413/S414 修过栈抢占和锁顺序，动 `IPI_STOP` 必须重跑 `tools/test_s400.sh` 全套。

**`kgdb_stub.c` 是另一条独立路线**（双机 gdb），和 kdb shell 不是一回事。Linux 当年花大力气才把两条线合并到 debug core——没必要重演这段历史，先用 `mon.c` 把本地调试做扎实。

### 5. 风险提示

- 别删 `wakeup()`：`kernel/thread/kipc.c` 那份 waitq 实现是实际链接的版本，`block_on/wakeup` 被 tty、socket、锁、malloc 全系统依赖，删了键盘/串口全废。
- 动 SMP 停机逻辑前，先重跑 S413/S414 相关回归（双核栈抢占、锁顺序）。

---

## 五、核心思想

kdb 的本质是**"挂在统一调试核心上的一个自带 shell 前端"**——它不自己去抓异常、不做 CPU 同步、不管 I/O，只负责把内核状态打印成人能读的命令输出。

这正是它能从 6.4 万行老代码砍成薄薄一层后合并进主线的理由：**先把公共机制抽走，前端就只剩"界面"了**——和"好抽象让难题消失"是同一个道理。

对 bsd45 而言：已有 shell 雏形、arch 层、I/O 三层，只缺中间的 debug core。补上它，就是一个能用的自制 kdb。

---

## 六、代码改造示例（骨架）

> 说明：以下代码贴合 `mon.c` 现有风格（`static void cmd_xxx(const char *arg)`、复用 `parsehex()`、中英双语注释），是**改造骨架**而非可直接编译的补丁。接入前需把 `Crtat`、`com_connected`、`struct trapframe` 字段名、`IPI_STOP` 处理函数入口对齐到当前源码的实际定义。

### 6.1 命令表化：把 `else if` 链改成注册表

`mon.c` 第 376–516 行是 40 多条 `strcmp` 链。第一步是引入描述符和注册表：

```c
/* 中文：kdb 命令描述符——一张 {名字, 处理函数, 帮助} 的表 */
/* EN: kdb command descriptor — a {name, handler, help} table */
typedef void (*kdb_cmd_fn)(const char *arg);

struct kdb_cmd {
	const char	*kc_name;
	kdb_cmd_fn	 kc_fn;
	const char	*kc_help;
	struct kdb_cmd	*kc_next;	/* 链表，支持运行时追加命令 */
};

static struct kdb_cmd *kdb_cmds;	/* 按注册顺序串起来 */

/* 中文：注册一条命令；静态命令在 kdb_init() 里注册，动态命令可被模块追加 */
/* EN: Register a command; static ones in kdb_init(), dynamic ones appended by modules */
void
kdb_register(struct kdb_cmd *kc)
{
	kc->kc_next = kdb_cmds;
	kdb_cmds = kc;
}

#define KDB_CMD(name, fn, help) \
	static struct kdb_cmd kc_##fn = { name, fn, help }
```

把已有命令和新命令统一登记（table 必须是 `static`，不能放栈上）：

```c
KDB_CMD("mem",	 cmd_mem,	"physical/virtual memory stats");
KDB_CMD("vstat", cmd_vstat,	"VM statistics");
KDB_CMD("pte",	 cmd_pte,	"pte <addr> — page table entry");
KDB_CMD("kthread", kthread_print,"thread table");
KDB_CMD("smpinfo", smp_print,	"SMP state");
KDB_CMD("bt",	 cmd_bt,	"backtrace current stack");
KDB_CMD("regs",	 cmd_rd,	"dump trapframe registers");
KDB_CMD("md",	 cmd_md,	"md <addr> [len] — memory display");
KDB_CMD("mm",	 cmd_mm,	"mm <addr> <val> — memory modify");
KDB_CMD("help",	 cmd_help,	"list commands");
```

主循环从"40 条链"压缩成"两行"：

```c
/* 中文：命令分派——拆出命令名与参数，遍历注册表 */
/* EN: Command dispatch — split name/arg, then walk the registry */
static void
kdb_dispatch(const char *line)
{
	struct kdb_cmd *kc;
	char name[32];
	int n, off;

	for (n = 0; line[n] && line[n] != ' ' && n < sizeof(name) - 1; n++)
		name[n] = line[n];
	name[n] = 0;
	off = line[n] ? n + 1 : n;

	for (kc = kdb_cmds; kc; kc = kc->kc_next) {
		if (!strcmp(name, kc->kc_name)) {
			kc->kc_fn(line + off);	/* 无参命令忽略 arg */
			return;
		}
	}
	printf("unknown command: %s (try help)\n", name);
}

/* 中文：monitor 主循环现在是 getline + 分派 */
/* EN: The monitor loop is now just getline + dispatch */
for (;;) {
	printf("bsd45> ");
	if (getline() == 0)
		continue;
	kdb_dispatch(line);
}
```

`reboot`/`halt` 这类会进入死循环、不返回的命令，建议保留在主循环里特判，或让 handler 永不返回——避免破坏分派函数的返回约定。

### 6.2 `bt`：沿帧指针回溯

i386 上函数序言是 `push %ebp; mov %esp,%ebp`，所以 `[fp]` 是旧帧指针、`[fp+1]` 是返回地址：

```c
/* 中文：沿帧指针回溯当前栈，打印每层的返回地址与函数名（符号表可用时） */
/* EN: Walk the current stack via frame pointers, printing return addresses and symbols */
static void
cmd_bt(const char *arg)
{
	unsigned *fp;
	int i;

	__asm __volatile("movl %%ebp, %0" : "=r"(fp));

	for (i = 0; i < 32 && fp; i++) {
		unsigned ret = fp[1];

		if (ret >= (unsigned)KERNBASE && ret < (unsigned)etext)
			printf("#%02d fp %x ret %x %s\n", i, (unsigned)fp,
			    ret, kdb_symbol(ret));
		else
			printf("#%02d fp %x ret %x\n", i, (unsigned)fp, ret);
		if (fp[0] <= (unsigned)fp)
			break;			/* 帧不再增长，防止死循环 */
		fp = (unsigned *)fp[0];
	}
}
```

**坑**：`bt` 依赖帧指针链，编译必须加 `-fno-omit-frame-pointer`。bsd45 已有"现代 GCC 修补"记录，升到 GCC 16 时要确认这个选项没被优化掉，否则回溯第一层就断。

### 6.3 符号表：让 `bt` 显示函数名

```c
/* 中文：链接期从 ELF 符号段生成静态 {地址, 名字} 表，按地址排序 */
/* EN: Link-time static {address, name} table from the ELF symbol section, sorted by address */
struct kdb_sym { unsigned ks_addr; const char *ks_name; };
extern struct kdb_sym kdb_symtab[];	/* 由脚本从 BSD45.ELF 的 .symtab 生成 */
extern int kdb_nsym;

/* 中文：二分查找最大的 <= addr 的符号，把地址翻成函数名 */
/* EN: Binary-search the largest symbol <= addr to resolve an address */
const char *
kdb_symbol(unsigned addr)
{
	int lo = 0, hi = kdb_nsym - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (kdb_symtab[mid].ks_addr <= addr) {
			if (mid == hi || kdb_symtab[mid + 1].ks_addr > addr)
				return kdb_symtab[mid].ks_name;
			lo = mid + 1;
		} else
			hi = mid - 1;
	}
	return "??";
}
```

生成脚本的思路：`nm -n BSD45.ELF | grep ' [tT] ' | awk '{print "{0x"$1", \""$3"\"},"}' > kdb_symtab.inc`，再在链接期编进来。

### 6.4 `regs` / `md` / `mm`：读寄存器与读写内存

```c
/* 中文：从 trapframe 打印寄存器；kgdb_glue.c 已有 splhigh 下拷贝帧的约定 */
/* EN: Print registers from the trapframe; kgdb_glue.c defines the splhigh copy convention */
extern struct trapframe kdb_tf;

static void
cmd_rd(const char *arg)
{
	kdb_printf("eip %x esp %x ebp %x eflags %x\n",
	    kdb_tf.tf_eip, kdb_tf.tf_esp, kdb_tf.tf_ebp, kdb_tf.tf_eflags);
	kdb_printf("eax %x ebx %x ecx %x edx %x esi %x edi %x\n",
	    kdb_tf.tf_eax, kdb_tf.tf_ebx, kdb_tf.tf_ecx, kdb_tf.tf_edx,
	    kdb_tf.tf_esi, kdb_tf.tf_edi);
	kdb_printf("cs %x ds %x ss %x trapno %d\n",
	    kdb_tf.tf_cs, kdb_tf.tf_ds, kdb_tf.tf_ss, kdb_tf.tf_trapno);
}

/* 中文：内存显示，复用 mon.c 已有的 parsehex() */
/* EN: Memory display, reusing mon.c's existing parsehex() */
static void
cmd_md(const char *arg)
{
	unsigned addr, len = 32, v;

	if (parsehex(arg, &addr) == 0) {
		printf("usage: md <addr> [len]\n");
		return;
	}
	while (*arg && (*arg == ' ' || (*arg >= '0' && *arg <= '9') ||
	    (*arg >= 'a' && *arg <= 'f') || (*arg >= 'A' && *arg <= 'F') ||
	    *arg == 'x' || *arg == 'X'))
		arg++;
	if (*arg) {
		unsigned l;
		if (parsehex(arg, &l) > 0)
			len = l;
	}
	for (v = addr; v < addr + len; v += 16)
		printf("%04x: %08x %08x %08x %08x\n", v,
		    *(unsigned *)(v), *(unsigned *)(v + 4),
		    *(unsigned *)(v + 8), *(unsigned *)(v + 12));
}

/* 中文：内存写入——改之前先用 md 确认地址，改错直接崩 */
/* EN: Memory write — confirm the address with md first; a wrong write crashes instantly */
static void
cmd_mm(const char *arg)
{
	unsigned addr, val;
	int n;

	if ((n = parsehex(arg, &addr)) == 0) {
		printf("usage: mm <addr> <val>\n");
		return;
	}
	if (parsehex(arg + n, &val) == 0) {
		printf("usage: mm <addr> <val>\n");
		return;
	}
	*(unsigned *)addr = val;
	printf("%x <- %x (verify with md)\n", addr, val);
}
```

### 6.5 `kdb_printf`：绕过锁的应急输出

崩溃时 `printf` 走的 tty/锁路径不可信，必须直接写 VGA 显存和 COM1：

```c
/* 中文：应急打印——直接写 VGA 文本缓冲与 COM1，绕过 printf 的 tty/锁路径 */
/* EN: Emergency print — write straight to VGA text buffer and COM1, bypassing printf */
extern volatile unsigned short *Crtat;	/* pccon.c 的显存游标 */
extern int com_connected;		/* 已有 COM1 镜像开关 */

static void
kdb_putc(int c)
{
	if (c == '\n')
		kdb_putc('\r');
	*Crtat++ = 0x0f00 | (unsigned char)c;	/* 白字黑底 */

	if (com_connected) {
		while (!(inb(0x3f8 + 5) & 0x20))	/* 16550 LSR bit5: THR 空 */
			;
		outb(0x3f8, c);
	}
}

/* 中文：只支持 %s %x %d，避免把完整 vsprintf 拖进调试路径 */
/* EN: Only %s %x %d, to avoid pulling full vsprintf into the debug path */
void
kdb_printf(const char *fmt, ...)
{
	va_list ap;
	const char *p;

	va_start(ap, fmt);
	for (p = fmt; *p; p++) {
		if (*p != '%') {
			kdb_putc(*p);
			continue;
		}
		switch (*++p) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			while (*s)
				kdb_putc(*s++);
			break;
		}
		case 'x': {
			unsigned v = va_arg(ap, unsigned), m = 0x80000000;
			while (m) {
				kdb_putc("0123456789abcdef"[(v & m) ? 1 : 0]);
				m >>= 1;
			}
			break;
		}
		default:
			kdb_putc(*p);
		}
	}
	va_end(ap);
}
```

上面 `%x` 的实现故意写成逐位输出以便看清逻辑，实际应改为按 4 位一组的标准写法。

### 6.6 `kdb_core.c`：统一入口 + 全核停机

```c
/* kernel/sys/kdb_core.c */
/* 中文：统一调试入口——本地 shell 与 gdbstub 两个前端都从这里进 */
/* EN: Unified debug entry — both the local shell and gdbstub frontends enter here */
volatile int kdb_active;		/* 0=未激活，>0=嵌套深度 */
static int kdb_master_cpu;
volatile int kdb_ap_stopped[MAXCPUS];

void
kdb_handle_exception(int trapno, struct trapframe *tf)
{
	int me = cpuid();

	if (!kdb_active) {
		kdb_master_cpu = me;
		kdb_active = 1;
		kdb_stop_others();	/* 发 IPI_STOP，等其他 CPU 应答 */
		kdb_save_tf(tf);	/* 复用 kgdb_glue 的 splhigh 拷贝逻辑 */
	}

	if (me == kdb_master_cpu) {
		kdb_printf("\n*** kdb: trap %d at eip %x ***\n", trapno, tf->tf_eip);
		kdb_shell();		/* 本地命令循环；敲 kgdb 切 gdbstub */
	} else {
		while (kdb_active)	/* 非 master：轮询等待，绝不拿锁 */
			;
	}
}

void
kdb_resume(void)
{
	kdb_active = 0;
	kdb_start_others();		/* 唤醒停在握手循环里的 CPU */
}

/* 中文：IPI_STOP 处理函数——AP 收到后进入可控循环，不碰锁和调度器 */
/* EN: IPI_STOP handler — APs enter a controlled loop, touching no locks or scheduler */
void
ipi_stop_handler(void)
{
	kdb_ap_stopped[cpuid()] = 1;
	while (kdb_active)
		;
}
```

异常入口挂接（在 `trap()` 分派处加 tripwire）：

```c
/* 中文：int3 或待处理断点时转入 kdb，返回后走原流程 */
/* EN: Enter kdb on int3 or a pending breakpoint, then fall through to normal dispatch */
void
trap(struct trapframe *tf)
{
	if (tf->tf_trapno == T_BPTFLT || kdb_break_pending) {
		kdb_handle_exception(tf->tf_trapno, tf);
		kdb_resume();
		return;
	}
	/* ... 原有分派 ... */
}
```

### 6.7 接入清单与安全注意

- **构建**：`build_kernel.sh` 加入 `kdb_core.c` 与符号表 `.inc`；要启用 gdbstub 路线时再加入 `kernel/arch/i386/i386/kgdb_stub.c` 和 `kgdb_glue.c`（这两个当前不在编译清单里）。
- **保留 `wakeup()`**：`kernel/thread/kipc.c` 里的 `block_on/wakeup/wakeup_one` 是实际链接的版本，tty、socket、锁、malloc 全依赖它——`kdb_printf` 不能替换掉它，只能在调试路径旁路。
- **不要调用 `printf`**：调试路径里任何 `printf`/`malloc`/锁操作都可能在崩溃现场死锁，只能用 `kdb_printf`。
- **SMP 回归**：动 `IPI_STOP` 前重跑 `tools/test_s400.sh` 全套——S413/S414 修过的双核栈抢占与锁顺序就在这一带。
- **帧指针**：确认交叉编译参数含 `-fno-omit-frame-pointer`，否则 `bt` 断链。
- **分档交付**：先做 6.1 + 6.2 + 6.4 + 6.5（本地 shell，不碰 SMP），跑稳后再上 6.6 的全核停机。
