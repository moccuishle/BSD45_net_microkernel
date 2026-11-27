# 对象化内核设计（BSD_object_orient_kernel）

> 目标：把 BSD45 从“Unix 风格的进程即执行单元 + 文件描述符 + 同步系统调用”
> 改造成 NT 风格的**对象内核**：
>
> - 文件、进程、线程、事件、管道、注册表键都是内核对象；
> - 所有对象共享一棵命名空间树 `\`；
> - **线程是一等公民**（进程只是地址空间 + 对象容器）；
> - **IRP 风格的异步 I/O** + **IOCP 完成端口**；
> - 内核字符串统一 UTF-16；
> - 内核不直接碰硬件，所有端口 I/O/中断收敛到 HAL。
>
> 参考实现：`Z:\迅雷下载\Microsoft leaked source code archive_2020-09-24\nt5src\nt5src\Source\XPSP1\NT`
> （下文简称 `NT/`）。新增代码命名遵循 4.5BSD/Unix 风格，不使用 NT 的
> Hungarian 前缀。

---

## 0. 架构总览：分层单内核（layered monolithic）

**部署形态是单内核，内部按模块边界分层**：所有代码都跑在 ring 0、
共享地址空间（保住系统调用/页表/IPC 的性能），但模块之间只通过明确接口
调用，不允许跨层直接摸对方的数据结构。

```
                 +---------------------------------------+
  user ABI       |  personalities: unix/posix/linux/win32 |
                 +-------------------+-------------------+
                                     | bsd45_syscall()
                 +-------------------v-------------------+
  对象管理器      |  kernel/obj: 对象头/类型/命名空间/句柄  |
                 +---------+-------------------+---------+
                           |                   |
                 +---------v--------+  +-------v---------+
  VM             | kernel/vm:       |  | kernel/thread:  |
                 | 地址空间/页/回收  |  | 线程/调度/软中断 |
                 +---------+--------+  +-------+---------+
                           |                   |
                 +---------v-------------------v---------+
  I/O            | kernel/io: IRP 请求 / IOCP 完成端口    |
                 +---------+-------------------+---------+
                           |                   |
                 +---------v--------+  +-------v---------+
  FS             | kernel/fs: VFS   |  | drivers: bdev   |
                 | vnode/mount/缓存 |  | ATAPI/ATA/...   |
                 +---------+--------+  +-------+---------+
                           |                   |
                 +---------v-------------------v---------+
  HAL            | kernel/hal: 端口IO/中断/DMA/时钟/控制台 |
                 +---------------------------------------+
```

### 0.1 接口契约

| 层 | 对外接口 | 禁止 |
|---|---|---|
| 对象管理器 | `obj_create/insert/lookup/ref/deref`、`obj_handle_*`、命名空间 | 直接碰进程表、页表、设备寄存器 |
| VM | `vm_space_*`、`page_alloc/free`、`pagecache_*`、`mmap_*`、`pagecache_reclaim` | 直接碰 vnode 内部字段 |
| 线程/调度 | `kthread_*`、`thread_*`、`wait/wake`、`softint_*` | 直接调用文件系统或设备 |
| I/O | `io_submit/complete`、`io_port_*`、IRP 主功能码 | 直接操作页表/调度器 |
| VFS/FS | `vfs_*`、`vnodeops`、`mount`、`dentry` | 直接调 `atapi_*`/`inb/outb`（走 bdev/HAL） |
| 驱动 | `bdev_*`、`hal_*` | 直接改进程/对象表 |
| HAL | `hal_inb/outb`、`hal_irq_*`、`hal_clock_*`、`hal_dma_*` | 业务逻辑 |

这些约束由 `tools/check-boundary.sh` 做硬规则检查；新代码必须同时通过
“分层检查 + 21 套验收”。

### 0.2 为什么还是单内核

- 系统调用、缺页、IPC 都在同一地址空间，没有消息往返开销；
- 对象管理器、VM、IO 的**接口**按模块边界分层，调用方不依赖实现细节；
- 分层只用于组织代码与检查依赖，不构成"把服务搬到用户态"的计划。

---


| NT 文件 | 内容 | 本项目对应 |
|---|---|---|
| `NT/base/ntos/inc/ob.h` | `OBJECT_HEADER`、`OBJECT_TYPE_INITIALIZER`、`OBJECT_DIRECTORY` | `kernel/obj/object.h` |
| `NT/base/ntos/ob/obdir.c` | 命名空间查找/插入/删除、`ObpLookupObjectName` | `kernel/obj/objdir.c` |
| `NT/base/ntos/ob/obhandle.c` | 每进程句柄表、引用计数、句柄关闭 | `kernel/obj/objhandle.c` |
| `NT/base/ntos/ob/obtype.c` | 对象类型注册、统计 | `kernel/obj/objtype.c` |
| `NT/base/ntos/ob/obref.c` | 按名/按句柄引用对象 | `kernel/obj/object.c` |
| `NT/base/ntos/inc/ntdef.h` | `UNICODE_STRING`（Length/MaximumLength/Buffer） | `kernel/obj/unistr.c` |
| `NT/base/ntos/ps/*` | 进程/线程对象、`PsCreateSystemThread` | `kernel/thread/` |
| `NT/base/ntos/ke/*` | 调度、等待、事件、DPC | `kernel/thread/sched.c` |
| `NT/base/ntos/io/iop.h`、`ntiobase.h` | `IRP`、`IO_STACK_LOCATION`、完成例程 | `kernel/io/ioreq.c` |
| `NT/base/ntos/io/*` | I/O 管理器、完成端口 | `kernel/io/iocp.c` |
| `NT/base/ntos/config/*` | 注册表键对象（`CM_KEY_BODY`） | `kernel/registry/` |
| `NT/base/hals/*` | HAL：端口 I/O、中断、时钟、总线 | `kernel/hal/` |

---

## 1. NT 参考文件与职责

## 2. 对象模型

### 2.1 头与类型（Unix 命名）

```c
struct obj_header {
	int		 o_ref;		/* 指针引用 */
	int		 o_handles;	/* 句柄引用 */
	unsigned	 o_flags;
	struct obj_type	*o_type;
	struct obj_dir	*o_dir;		/* 所属目录，可为 NULL */
	struct unistr	 o_name;	/* UTF-16，内嵌缓冲区 */
	void		*o_body;
};

struct obj_type {
	struct unistr	 t_name;	/* UTF-16 类型名 */
	int		(*t_close)(struct obj_header *);
	int		(*t_delete)(struct obj_header *);
	int		(*t_parse)(struct obj_header *, const struct unistr *,
			    struct obj_header **);
	int		(*t_query_name)(struct obj_header *, struct unistr *);
	unsigned	 t_count;	/* 活跃对象数 */
	unsigned	 t_handle_count;
};
```

对应 `OBJECT_HEADER` 的 `PointerCount/HandleCount/Type/NameInfo`，但去掉了
可选的 quota/handle/creator 扩展块（本项目的对象都是小对象，统一内嵌即可）。

### 2.2 命名空间

- 根：`\`（`Directory` 对象）。
- 固定子目录：`\Device`、`\FileSystem`、`\KernelObjects`、`\Processes`、
  `\Pipes`、`\Registry`。
- 目录体：64 个目录项，线性查找；名字比较用 `ustr_icmp`（大小写不敏感，
  与 NT 命名空间一致）。
- 路径分隔符同时接受 `\` 和 `/`；支持 `.`、`..`（向上到根钳制）。
- 符号链接对象（`SymbolicLink`）用于 `\DosDevices\X:`、`\??\` 之类的别名，
  解析时替换前缀后继续。

### 2.3 句柄

每个进程一张句柄表（`struct obj_handle`），句柄值 = 槽位 + 1 的 4 的倍数：

```c
struct obj_handle {
	struct obj_header	*h_obj;
	unsigned		 h_access;
	int			 h_used;
};
```

`obj_handle_open()` 按名查找 + 引用 + 分配句柄；`obj_handle_close()` 减
句柄计数；`obj_deref()` 在指针计数与句柄计数都归零时调用类型的
`t_delete`/`t_close`。

---

## 3. 线程模型（一等公民）

> **抽象顺序（2026-11-16 定稿）**：先把执行流抽象成独立的 thread 对象，
> thread 对象挂在 process 对象下；调度器只认 thread；地址空间、fd、cwd 只是
> thread 共享的 process 资源。这个顺序定下来之后，部署形态（单内核）不再
> 影响线程/进程的接口设计。

### 3.1 结构

```c
struct kthread {
	int		 t_state;	/* FREE/RUN/BLOCK/STOP/ZOMBIE */
	int		 t_tid;
	struct uproc	*t_proc;	/* 共享地址空间、fd、cwd */
	struct sysregs	 t_regs;	/* 用户上下文（协作式恢复点） */
	void		*t_kstack;	/* 内核栈（后续抢占用） */
	struct obj_header *t_obj;	/* Thread 对象 */
	struct io_req	*t_io;		/* 正在等待的 IRP */
	struct io_port	*t_port;	/* 阻塞在哪个完成端口 */
	int		 t_wait_obj;	/* 等待对象类型 */
	void		*t_wait_ptr;
	unsigned	 t_wake_reason;
};
```

- `struct uproc` 退化为**进程容器**：页目录、fd 表、cwd、信号、句柄表。
- 每个进程至少一个主线程；`thread_create` 在同一 `uproc` 里加线程。
- 调度器从“选进程”改为“选线程”，切 CR3 用 `t_proc->pd`。
- 阻塞：`thread_block(reason)` 保存 `t_regs`，调用 `sched()`；唤醒方设置
  `t_regs.eax` 并置 RUN（沿用现有 pipes/wait4 的协作式恢复机制）。
- 后续加时钟中断时，每个线程分配独立内核栈，`t_kstack` 才真正使用；
  当前协作式模型复用 TSS 的 esp0，无需切换内核栈。

### 3.2 系统调用（4.5BSD 风格命名，编号避开现有）

| 调用 | 语义 |
|---|---|
| `thread_create(entry, arg, stack, flags)` | 同进程新线程，返回 tid |
| `thread_exit(code)` | 结束当前线程 |
| `thread_join(tid, &status)` | 等待线程退出 |
| `thread_yield()` | 主动让出 |
| `thread_self()` | 返回 tid |
| `event_create(name, manual_reset)` | 创建/打开事件对象 |
| `event_signal(handle)` / `event_reset(handle)` | 置位/复位 |
| `wait_for_object(handle, timeout)` | 等待事件/线程/进程/端口 |

---

## 4. IRP 风格异步 I/O

### 4.1 请求对象

```c
struct io_req {
	struct obj_header *r_obj;	/* IoRequest 对象（可等待） */
	int		 r_major;	/* IO_READ/WRITE/CLOSE/IOCTL */
	int		 r_status;
	unsigned	 r_info;	/* 实际传输字节 */
	struct vnode	*r_vp;
	unsigned	 r_off;
	void		*r_buf;		/* 用户缓冲 */
	unsigned	 r_len;
	struct uproc	*r_proc;	/* 目标地址空间 */
	struct io_port	*r_port;	/* 完成端口，可为 NULL */
	unsigned	 r_key;
	void		*r_overlapped;
	int		 r_state;	/* FREE/PENDING/ACTIVE/DONE */
	struct io_req	*r_next;
};
```

- 对应 NT 的 `IRP` + `IO_STACK_LOCATION`；本项目只有一层驱动，不做栈式转发，
  但保留“提交 → 驱动处理 → 完成例程 → 完成端口”的生命周期。
- 同步 read/write 实现为：创建 req → `io_submit()` → `wait_for_object(req)`；
  异步：`io_submit()` 立即返回，完成端口/事件收到通知。

### 4.2 工作线程

- 启动时创建 N 个内核工作线程（`struct kthread` 的 `t_proc == NULL` 变体，
  或绑定到内核进程 `proc0`）。
- 工作线程循环：`io_get()`（队列取请求）→ 调 `vnodeops->read/write` 到内核
  缓冲 → 按目标进程页目录复制到用户缓冲 → `io_complete(req)`。
- 当前无抢占：提交线程在 `io_submit` 后继续跑；只有阻塞（wait/port get）时
  工作线程才被调度。这已经能表达异步语义；加时钟中断后自然变成真并行。

---

## 5. IOCP 完成端口

```c
struct io_port {
	struct obj_header *p_obj;
	struct io_packet  *p_head, *p_tail;	/* 完成包队列 */
	int		  p_count;
	int		  p_active;		/* 已唤醒的等待者 */
	int		  p_max_active;
	struct kthread	  *p_waiters;
};
struct io_packet {
	unsigned	  k_key;
	void		 *k_overlapped;
	int		  k_status;
	unsigned	  k_bytes;
	struct io_packet *k_next;
};
```

- `io_port_create(max_active)` → 创建 `IoCompletion` 对象，挂到
  `\KernelObjects\<name>`（可选命名）。
- `io_port_associate(port, handle, key)` → 把文件/管道句柄绑定到端口。
- 提交 IRP 时带 `port + key + overlapped`；完成时
  `io_port_post()` 把包入队，并唤醒一个等待线程。
- `io_port_get(port, &key, &ov, &status, &bytes, timeout)` → 取一个包；
  没有包则阻塞（协作式）。
- 并发控制：`p_active < p_max_active` 才唤醒新等待者；线程处理完调用
  `io_port_done(port)` 归还配额（对应 NT 的 `NtSetIoCompletion` 语义）。

---

## 6. UTF-16 / ASCII：一个宏，两种内核字符串

- 内核内部统一 `struct kstr`；**编译期宏 `KERNEL_UNICODE`** 决定表示：
  - `KERNEL_UNICODE=1`：UTF-16LE（`Length/MaximumLength` 按字节，NT 风格），
    完整支持 UTF-8↔UTF-16（含代理对）；
  - `KERNEL_UNICODE=0`：ASCII 字节串；`kstr_set_utf8()` 拒绝非 ASCII 输入，
    保证“ASCII 内核”名副其实。
- 两种构建共用同一份源码、同一套 `kstr_*` API，只有 `kstr.h` 的布局和
  `kstr.c` 的实现分支不同。构建开关：
  `build_all.ps1 -Unicode` / `build_kernel.ps1 -Unicode`，
  Linux 用 `KERNEL_UNICODE=1 bash build_all.sh`。
- **A/W API 约定**：syscall/libc 边界只提供 UTF-8/A 版；内核内部只提供
  `_u` 版（`obj_lookup_u`、`vfs_namei_u` 等，名字里的 u 表示“kernel string”，
  不是“一定 UTF-16”）。A 版在 trap 层做一次转换。
- 打印/调试是唯一的反向边界：`kstr_to_utf8()` 后交给 `printf`。
- 对象名、类型名、命名空间路径已经是 `kstr`；VFS 的 ASCII 接口在 S19
  继续迁移，A 版只保留在 syscall 层。

---

## 7. HAL（内核不直接碰硬件）

`kernel/hal/` 暴露：

| 接口 | 作用 |
|---|---|
| `hal_inb/hal_outb/hal_insw/hal_outsw` | 端口 I/O |
| `hal_irq_register(irq, handler)` / `hal_irq_mask/unmask` | 中断注册（后续） |
| `hal_clock_init/read` | 时钟/RTC |
| `hal_console_putc/getc` | 控制台 |
| `hal_dma_*` | DMA（后续） |

驱动（`atapi.c`、`pccon.c`、未来的磁盘/网卡）只调用 `hal_*`，不直接
`inb/outb`。`locore.s`/`pmap.c` 属于 HAL 的机器相关部分，保留汇编入口。

---

## 8. API 子系统（personality）

内核只暴露一套内部服务 ABI（`bsd45_syscall()`，4.5BSD 编号）。子系统是
“API → 服务 ABI”的翻译模块，和内核核心解耦：

| 子系统 | 文件 | 做法 | 状态 |
|---|---|---|---|
| **unix** | `subsys_unix.c` | 直接转发原生 4.5BSD ABI（默认） | ✅ |
| **posix** | `subsys_posix.c` | 只放行 POSIX 表面（open/read/write/fork/exec/wait/stat/sig…），`vfork` 等返回 ENOSYS | ✅ |
| **linux** | `subsys_linux.c` | Linux i386 编号 → 原生编号翻译表（mkdir 39→136、wait4 114→7、mmap2 192→197、exit_group 252→exit…） | ✅ |
| **win32** | `subsys_win32.c` | 最小 Win32 句柄 API：GetCurrentProcessId/ThreadId、CreateFileA、ReadFile、WriteFile、CloseHandle、Sleep | ✅（最小面） |

统一入口：

```
int 0x80 → syscall_entry → subsys_dispatch(r)
                ├─ eax=424 set_personality / 425 get_personality（核内策略）
                └─ active personality->s_dispatch(r)
                        └─ bsd45_syscall(r)  （对象/VFS/进程服务）
```

- 子系统注册用 `subsys_register()`，新增子系统不需要改 trap 入口或 VFS。
- 用户态通过 `/bin/subtest` 切换 personality 并分别调用四套 ABI；
  monitor 的 `subsys` / `subsystest` 展示注册表与内核自测。

---

## 9. 信号与 fork：进程定向 vs 线程定向

> 这是对象化线程模型最容易踩的语义坑：信号到底发给进程还是线程，以及
> `fork()` 在多线程进程里只复制调用线程——两者组合就是经典的
> “子进程继承已加锁的 mutex”死锁。

### 9.1 信号状态分成两层

| 状态 | 归属 | 内容 |
|---|---|---|
| `p_sig` | process | 进程定向 pending 位图 |
| `p_sighand[NSIG]` / `p_sigignore` / `p_sigcatch` | process | 处置方式（所有线程共享） |
| `t_sig` | thread | 线程定向 pending（`tgkill`/`pthread_kill`/同步异常） |
| `t_sigmask` | thread | 该线程的阻塞掩码 |
| `t_sigwait` | thread | `sigwait`/`sigtimedwait` 正在等的集合 |

### 9.2 投递规则

1. **线程定向**（`tgkill(tgid,tid,sig)`、`pthread_kill`、`SIGSEGV/SIGFPE/
   SIGILL/SIGBUS` 这类同步异常）→ 只放进 `t_sig`，只唤醒该线程。
2. **进程定向**（`kill(pid,sig)`、终端 `^C`、`SIGCHLD`、`SIGTERM`）→ 放进
   `p_sig`。返回用户态时，选择一个**未阻塞该信号**的线程投递；若所有线程都
   阻塞，则留在 `p_sig`，直到某个线程解除阻塞。
3. **进程级强制信号**：`SIGKILL`/`SIGSTOP`/`SIGCONT` 作用于整个进程；
   `SIGSTOP`/`SIGCONT` 改变进程状态并唤醒/停止所有线程。
4. 默认动作终止进程时，记录 `p_termsig`（`WIFSIGNALED`）；线程级致命信号
   （如 `SIGSEGV`）也按整个进程终止处理。
5. `sigprocmask` 只改当前线程的 `t_sigmask`；`pthread_sigmask` 同一语义。

### 9.3 fork / exec / exit

- **`fork()` 只复制调用线程**：子进程的线程表里只有当前线程（tid 变为子
  进程的 pid），其它线程不存在；地址空间、fd、cwd、信号处置/掩码按 POSIX
  继承，pending 信号不继承。
- 被复制的地址空间里可能带着**其它线程持有的锁**（mutex、rwsem、用户态
  malloc 锁）。内核无法自动解锁；POSIX 的解法是 libc 的
  `pthread_atfork(prepare,parent,child)`：在 fork 前统一加锁、fork 后父子
  各自解锁。S21 的线程库必须实现这对钩子，并在文档里标明“多线程程序
  fork 后只能调用 async-signal-safe 函数，直到 exec”。
- **`execve()`** 丢弃所有线程，只保留调用线程（新地址空间、新 tid 归一）；
  所有 `pthread_atfork` 的 child 钩子在 exec 前执行。
- **`exit()`** 是线程退出（`pthread_exit`），**`exit_group()`** 才结束整个
  进程；`wait4()` 报告进程退出，`pthread_join()` 报告线程退出。
- 线程退出时，若它是最后一个线程，进程变成 zombie；父进程的 `wait4`
  回收进程，`pthread_join` 回收线程对象。

### 9.4 兼容性

现有的 `kill(pid,sig)`、`^C/^Z` 先按“进程定向”实现；S21 加线程时再引入
`tgkill` 和线程掩码。这样旧测试的行为不变，新语义可以逐项验证。

## 10. 内核线程与软中断

### 10.1 为什么需要

页面回收、软中断、IRP 完成这些工作不应该借用“刚才调用系统调用的那个用户线程”
的内核栈和上下文。S21 引入**内核线程**作为独立执行载体：

- 每个内核线程有自己 8 KB 的内核栈（`0xFF000000 + tid*8192`，`pmap_map`
  进内核窗口，PDE 0x3FC/0x3FD 与所有用户页目录共享；S410 起 1024 槽共 8MB）；
- 调度器在返回任何用户线程之前先运行可运行的 kernel thread；
- 每个系统调用返回用户态前也会 poll 一次，deferred work 不会等下一次阻塞。

### 10.2 接口

| 接口 | 作用 |
|---|---|
| `kthread_init()` | 预分配 NKTHREAD=4 个 8 KB 栈 |
| `kthread_spawn(name, entry, arg)` | 建线程、准备 `kswitch` 初始栈帧 |
| `kthread_block()` / `kthread_wake(t)` | 协作式阻塞/唤醒 |
| `kthread_exit()` | 释放线程状态，切回调度栈 |
| `kthread_run()` / `kthread_poll()` | 运行到无可运行线程 / 运行一个 |
| `kswitch(old, new)` | 保存 callee-saved 寄存器并切栈 |
| `softint_schedule(id)` | 置 pending 位并唤醒 softirq 线程 |
| `softint_register(id, fn)` | 注册 deferred 处理函数 |

### 10.3 软中断

- `NSOFT=4`：`SOFT_RECLAIM`、`SOFT_INPUT`、`SOFT_NET`、`SOFT_TEST`。
- 设备/VM 只置位；`softirq` 内核线程在自己的栈上清位并调用 handler。
- `SOFT_RECLAIM` 的 handler 调 `pagecache_reclaim(8)`；页缓存满时
  `pagecache_get()` 会 `softint_schedule(SOFT_RECLAIM)`，把淘汰工作交给
  softirq 线程，而不是在分配者上下文里做。
- 目前没有时钟中断，所以软中断在“用户阻塞点 + 系统调用返回”被调度；
  S27 接上时钟中断后自然变成周期性执行。

### 10.4 已知限制

- 协作式：没有抢占，长内核路径会延迟软中断。
- 内核线程继承当前 CR3（内核映射共享），不单独切页目录。
- 线程栈固定 8 KB，暂不支持增长/guard page。

---

## 11. 阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| **S18** | UTF-16/kstr + 对象头/类型 + 目录命名空间 + 句柄表 + 进程/线程/事件对象 + `objtree/objstat/objtest` | 19 套（原 17 + test_obj + test_subsys） |
| **S19** | `KERNEL_UNICODE` 宏：同一源码构建 UTF-16 或 ASCII 内核；对象层、路径解析、打印边界统一走 `kstr_*` | ✅ ASCII 与 UTF-16 两种 ISO 都通过 test_obj |
| **S20** | API 子系统：unix / posix / linux / win32 四个 personality，`subsys_dispatch` 解耦 | ✅ `test_subsys`（内核 + `/bin/subtest`） |
| **S21** | 内核线程 + softirq + 页回收独立载体 | ✅ `test_kthread`（21 套） |
| **S22** | 文件/管道/注册表键对象化：fd 引用 File/Pipe 对象，`\Registry` 下 Key 对象 | ✅ `test_reg`（22 套） |
| **S23** | IRP 异步 read/write + 完成端口（内核 API + monitor 验收） | ✅ `test_io` |
| **S24** | HAL 收口：驱动改走 `hal_*`（端口 I/O 只在 `kernel/hal`） | ✅ 全绿 |
| **S25** | 用户线程：thread 表调度、`thread_create/exit/join/detach`、每线程用户栈 | ✅ `test_pthread`（24 套） |
| **S26** | 用户态 pthread：`pthread_create/join/detach/yield/self` + `/bin/pthread` | ✅ `test_pthread` |
| **S27** | C++ `std::thread` 兼容层（基于 pthread） | 暂停（内核优先） |
| **S28** | IOCP 用户 ABI：`io_port_create`/`aio_submit`/`io_get`/`io_port_destroy` + `/bin/aiotest` | ✅ `test_aio`（25 套） |
| **S29** | VFS 字符串迁移到 `kstr`（A/W 边界只留 syscall） | 两种字符串构建全绿 |
| **S30** | 时钟中断 + 抢占 + 每线程内核栈 | 全绿 + 抢占测试 |

命名约定：新增结构/函数使用 4.5BSD 风格（`obj_*`、`thread_*`、`io_*`、
`kstr_*`），文件放 `kernel/{obj,thread,io,fs,registry,hal}/`，不引入 NT 的
`Nt*`/`Ob*`/`Io*` 前缀。

---

## 12. 文件系统路线：FAT32 / exFAT / NTFS / CNFS

### 12.1 先补的公共底座（F1）

| 组件 | 位置 | 作用 |
|---|---|---|
| `struct fs_type` / `fs_register()` | `kernel/fs/fs.c` | 文件系统注册表，按名字挂载 |
| `struct bdev` / `bdev_register()` | `kernel/driver/bdev.c` | 块设备接口（read/write/块大小/容量） |
| `ramdisk` | `kernel/driver/ramdisk.c` | 把一段内存当块设备，用于测试 FAT/exFAT/NTFS/CNFS 镜像 |
| `vfs_mount_fs(type, path, dev)` | `kernel/fs/vfs.c` | 把某个 fs 实例挂到命名空间（先 `/fat`、`/exfat`、`/ntfs`、`/cnfs`） |

所有磁盘文件系统只依赖 `bdev` 的读写接口，不直接调 ATAPI/ATA；这样同一套
FAT/NTFS 代码可以跑在 IDE、ramdisk、未来的 virtio 上。

### 12.2 FAT32（F2，读写）

- 引导扇区/BPB 解析（`FAT32` 标记、每簇扇区、保留区、FAT 数、根簇）。
- FAT 表缓存（复用 S17 buffer cache，按簇链 `CLUSTER->next` 遍历）。
- 目录项：8.3 名 + LFN（UTF-16！直接进入 `kstr`，不再转 ASCII）。
- 文件读写按簇链，支持跨簇、稀疏空洞（读补零）。
- 写路径：分配簇、更新 FAT+FSInfo、目录项时间戳；先做只读，再开写。
- 验收：`ls /fat`、`cat /fat/HELLO.TXT`、`echo x > /fat/A.TXT`、`rm`。

### 12.3 exFAT（F3，读写）

- 引导扇区 + 校验和；FAT 起始/长度、簇堆起始、根目录簇。
- 目录项集合（0x85 文件/0x90 流扩展/0xA0 文件名/0xC0 卷标/0x81 分配位图）。
- 大小写转换表（upcase table）与 UTF-16 文件名。
- 连续/分片簇链（NoFatChain 标志）、分配位图分配。
- 验收：与 FAT32 相同的工具路径，镜像由构建期 Python 生成。

### 12.4 NTFS（F4，只读）

只读子集，不碰日志重放/压缩/加密：

1. 引导扇区 → `$MFT` 起始簇、每簇字节、每 MFT 记录字节；
2. `$MFT` 记录：`FILE` 签名、`$STANDARD_INFORMATION`、`$FILE_NAME`、
   `$DATA`、`$INDEX_ROOT`/`$INDEX_ALLOCATION`（目录 B+ 树）；
3. 属性 runlist（稀疏/分片）→ 簇链读；
4. 大目录用 index buffer 遍历；文件名 UTF-16 直接进 `kstr`；
5. 写操作返回 `EROFS`（NTFS 写入需要日志/事务，放到后续）。
- 验收：`ls /ntfs`、`cat /ntfs/README.TXT`、`cat /ntfs/dir/file`（镜像用
  构建期生成/内置的 NTFS 测试卷）。

### 12.5 CNFS（F5，日志式，China File System）

**目标**：一个带日志、ACL、多数据流、稀疏文件、配额的现代文件系统。

**磁盘布局**

| 区域 | 内容 |
|---|---|
| Superblock (LBA 0) | magic `CNFS`、version、block size、UUID、root inode、journal inode、feature flags、quota root |
| Journal | 循环日志：`TX_BEGIN` / `BLOCK_REDO` / `TX_COMMIT` / `CHECKPOINT`，元数据先写日志再写原位 |
| Inode table | 定长 inode + 内联 extent 数组；溢出 extent 用 B+ 树 |
| Directory B+ tree | key = (UTF-16 名字, stream id)，value = inode 号 |
| Stream table | 多数据流：每个流一条 `(stream id, name, size, extents, flags)` |
| ACL records | `(principal type/id, rights mask, inherit flags)`，按 inode 引用 |
| Quota tree | `(uid/gid, used blocks, used inodes, soft/hard limit)` |
| Bitmap | 块/inode 分配位图 |

**特性实现要点**

- **日志**：`cnfs_journal_begin()` → 把脏元数据块写入日志环 → `commit`
  （写 commit 记录并 flush）→ 原位写回 → `checkpoint` 释放日志空间；
  挂载时扫描日志并 replay 未 checkpoint 的事务。
- **ACL**：inode 指向 ACL 记录；`cnfs_access(inode, uid, gid, mask)` 按
  owner/group/other + ACL 条目判定；POSIX personality 映射到 `rwx`，
  Win32 personality 映射到 `FILE_READ_DATA/WRITE_DATA/...`。
- **多数据流**：目录项 key 带 stream id；未命名流是默认 `$DATA`，
  命名流如 `file:thumbnail`（Win32）或 `file@thumbnail`（POSIX xattr 映射）。
- **稀疏文件**：extent 带 `CNFS_EXT_HOLE`，读空洞返回零、不分配块；
  `truncate` 扩洞；配额只计实际块。
- **配额**：写/分配块前查 quota tree；超软限制只告警，超硬限制返回
  `EDQUOT`；`cnfs_quota_set/get` 系统调用暴露给两个 personality。

**验收**：`test_cnfs` 在 ramdisk 上格式化一个卷，创建/写入/读取/删除
文件，触发日志（模拟失败点 replay），验证 ACL 拒绝/放行、命名流、
`seek` 过洞读零、配额超限 `EDQUOT`；并验证对象命名空间里
`\FileSystem\CNFS` 对象存在。

### 12.6 阶段

| 阶段 | 内容 | 验收 |
|---|---|---|
| F1 | fs 注册表 + bdev + ramdisk + `/fat` 挂载点 | 全绿 |
| F2 | FAT32 只读 → 读写 | `test_fat` |
| F3 | exFAT 只读 → 读写 | `test_exfat` |
| F4 | NTFS 只读 | `test_ntfs` |
| F5 | CNFS：日志 + ACL + 多流 + 稀疏 + 配额 | `test_cnfs` |

---

## 13. 后续高级模块：ZFS 与 DTrace

### 13.1 ZFS（Solaris）

**核心洞察**：文件系统的敌人是**静默数据损坏**和**更新撕裂**；ZFS 用
**COW + 事务组（txg）+ 端到端校验和**一次解决，快照、去重、压缩都是这个
架构的免费副产品。

移植不追求一次搬完 OpenZFS（>100k 行），按只读→读写分层：

| 阶段 | 内容 | 验收 |
|---|---|---|
| Z1 | vdev label（L0/L1/L2/L3）+ uberblock ring 解析；单 vdev pool 导入 | `zpool import` 能打印 pool 名/GUID/txg |
| Z2 | nvlist/XDR 解码；objset、dnode、ZAP、SPA 读路径；`ls`/`cat` 池内文件 | `test_zfs_ro` |
| Z3 | fletcher4/sha256 校验和验证；COW 写事务组；`zil` 回放（只读挂载时 replay） | 校验和注入错误被拒绝 |
| Z4 | 快照（dataset snapshot）、压缩（lz4）、去重表（DDT）查询 | `test_zfs_snap` |

关键约束：

- 块设备只通过 `bdev` 接口（ramdisk/ATA），ZFS 不直接碰 ATAPI；
- 所有名字/属性是 UTF-16 `kstr`，ZAP 的 key 直接进入命名空间；
- 校验和不匹配时返回 `EIO` 并在 `zpool status` 里记录，不静默返回坏数据；
- 测试镜像在构建期用 OpenZFS 工具生成（Ubuntu 构建机），以 ramdisk 形式
  嵌入 ISO，避免在目标机依赖 `zpool` 命令。

### 13.2 DTrace（Solaris）

**核心洞察**：生产环境内核可观测性和安全性可以兼得——探针静态列出、
D 语言受限、未知代码不允许注入。

| 阶段 | 内容 | 验收 |
|---|---|---|
| D1 | 静态探针注册表：`provider:module:function:name` + 5 个参数；`dtrace_probe(id, a0..a4)` | `dtrace -l` 列出探针 |
| D2 | 每 CPU 环形缓冲；`/dev/dtrace` ioctl：LIST/ENABLE/READ；探针默认关闭，关闭时零开销 | `dtrace -l`/`-n` 打开/关闭 |
| D3 | 受限 D 子集：`predicate`（比较/逻辑）、`printf/trace/count/sum`；用户态编译成小字节码，内核解释执行，**不注入机器码** | `test_dtrace` |
| D4 | 探针点：syscall entry/return、scheduler switch、IRP submit/complete、VFS read/write、page fault | 统计输出正确 |

DTrace 的探针点用 `DTRACE_PROBE*` 宏静态埋点，未启用时只做一次位图判断；
这保证“生产可观测”不牺牲性能。

### 13.3 顺序

在 F1–F5（文件系统底座）与 S22（用户线程）、S24（IOCP 用户 ABI）之后：

1. **Z1/Z2**（只读 ZFS）——先证明能导入并读文件；
2. **D1/D2/D3**（DTrace 探针与受限 D）——先能列出/统计 syscall；
3. **Z3/Z4**（COW 写、快照、压缩、去重）——需要 bdev 写路径；
4. **D4**（全探针点）——与 ZFS/CNFS 的读写路径一起接。
