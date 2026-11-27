# BSD45 的内核线程与进程实现

> 代码位置：`kernel/thread/kthread.{c,h}`（TCB 池、内核线程、`kswitch` 上下文切换）、`kernel/thread/kswitch.s`、`kernel/sys/usermode.c`（进程表、fork/exec/exit/wait、用户线程、`proc_sched`、`resume_user` 调用）、`kernel/sys/usermode_asm.s`（`resume_user` 与 IRQ0 帧构造）、`kernel/include/sys/kprocess.h`、`kernel/include/sys/kproc.h`、`kernel/include/sys/platform.h`
> 一句话：**进程是"资源 + 地址空间的容器"，TCB（`struct kthread`）才是调度单位**；同一进程的所有线程共享页目录，内核线程与用户线程共用同一个 16 槽 TCB 池，区别只在于 `t_proc` 是否为 NULL。

## 一、两级模型

```
                 struct process（8 个静态槽，procs[NPROC]）
                 ├── uk_pd          页目录（地址空间）
                 ├── uk_fds[16]     文件描述符表
                 ├── uk_p_vma[8]    mmap 区间
                 ├── uk_estcpu      per-process 公平性计数
                 ├── uk_p_thread ──► 当前运行的 TCB
                 └── uk_p_threads ─► TCB 链表（同进程所有线程）
                                        │
                        struct kthread（16 个静态槽，kthreads[NKTHREAD]）
                        ├── t_proc ──► 所属进程（内核线程为 NULL）
                        ├── t_ksp       保存的内核栈指针
                        ├── t_stack_top 内核栈顶（切换时装入 TSS.esp0）
                        ├── t_regs      非主线程的用户上下文
                        └── t_prio / t_quantum / t_state …
```

两条关键规则：

1. **调度走 TCB，不走进程**。`proc_sched()` 遍历 `kthreads[]`，`struct process` 只提供地址空间和资源。
2. **主线程的上下文存在 `proc->uk_regs` 里，不是 TCB 里**（`usermode.c:3652` 注释）。这样 fork/wait/pipe 这些只认进程的老路径完全不用改；只有"额外线程"把上下文放在 `kthread.t_regs`。

## 二、容量与编号

| 项 | 值 | 位置 |
|---|---|---|
| 进程槽 | `NPROC = 8`（`procs[8]` 静态数组） | `usermode.c:158` |
| TCB 槽 | `NKTHREAD = 16`（`kthreads[16]` 静态数组） | `kthread.h:21` |
| 每进程 fd | `NFD = 16` | `usermode.c:61` |
| 每进程 mmap 区 | `KPROC_NMAP = 8` | `kproc.h:23` |
| 信号数 / 进程名 / cwd | `32` / `16` / `128` 字节 | `kproc.h:24-26` |
| 内核栈 | 每 TCB 8 KB，VA 起点 `0xFF000000`（S410，1024 槽共 8MB） | `kthread.h` |
| pid 起点 | `next_pid = 2`（0/1 保留） | `usermode.c:211` |

**tid 分配规则**：

| 线程 | tid |
|---|---|
| 内核线程 | 槽位下标 `i + 1`（`kthread.c:153`） |
| 进程主线程 | 等于该进程 pid（`do_fork` 里 `proc_add_thread(p, p->uk_pid)`） |
| 进程内额外线程 | `uk_p_next_tid++`，初值 `pid + 100`（`usermode.c:2711`） |

所以 `t->t_tid == t->t_proc->uk_pid` 就是判断"这是不是主线程"的通用手法，代码里到处在用。

## 三、内核线程（KLT）：只有内核栈的执行载体

内核线程**永不进入用户态**，只有自己的内核栈，跑完 `t_entry` 就退出。常驻的两个在 `glue.c` 启动序列里创建：

```
kthread_init();      /* 16 个槽全部置 KT_FREE，打印 "kthread: 16 thread slots at fea00000" */
softirq_init();      /* 创建 softirq 线程（thread/softirq.c:95） */
vfs_aio_init(); aio_user_init();   /* 创建 aio 线程（fs/aio.c:94） */
```

### 3.1 创建：`kthread_spawn()`

1. 扫描 `kthreads[]` 找第一个 `!t_used` 的槽；
2. `bzero` 整个 TCB，设 `t_state = KT_RUN`、`t_tid = i+1`、`t_entry/t_arg`、`t_used = 1`；
3. `kstack_alloc(i)`：从 `KSTACK_VA + i * 8192` 开始，**逐页** `vfs_page_alloc()` + `pmap_map(..., PG_RW)`（所以每线程 8 KB = 2 个物理页，不是连续大块）；
4. **手工搭初始栈帧**，让第一次 `kswitch` 之后能"ret"进 `kthread_trampoline`：

```
   高地址  ┌──────────────┐  t_stack_top（16 字节对齐）
           │ trampoline   │  ← kswitch 的 ret 目标
           │ 0 (ebp)      │
           │ 0 (ebx)      │
           │ 0 (esi)      │
   低地址  │ 0 (edi)      │  ← t_ksp 指向这里
           └──────────────┘
```

5. `sched_prio_init(t, PRI_NORMAL)` 初始化优先级（10）；
6. `rq_enqueue(t)` 入运行队列，打印 `kthread: spawn <name> tid <n>`。

### 3.2 上下文切换：`kswitch()`（`kswitch.s`）

```asm
_kswitch:            ; void kswitch(unsigned *old_esp, unsigned new_esp)
    pushl %ebp / %ebx / %esi / %edi     ; 只保存 callee-saved
    movl 20(%esp), %eax                 ; old_esp
    movl 24(%esp), %edx                 ; new_esp
    movl %esp, (%eax)                   ; *old_esp = esp
    movl %edx, %esp                     ; 切栈
    popl %edi / %esi / %ebx / %ebp
    ret                                 ; 新线程第一次 ret 落到 trampoline
```

关键点：**只保存 4 个 callee-saved 寄存器**（调用者已把 caller-saved 存在自己栈上），因此切换开销极小；新线程靠 `kthread_spawn()` 预置的栈帧"凭空"开始执行。

`hal_switch_stack()` 是它的对外封装（`arch/i386/ctx.c:54`），`kthread.c` 用三处：

| 位置 | 用途 |
|---|---|
| `kthread_block()` | 把自己切回调度器 `sched_esp` |
| `kthread_exit()` | 同上，之后槽位释放 |
| `kthread_poll()` | 从 `sched_esp` 切到目标 TCB 的 `t_ksp` |

### 3.3 运行队列与调度

运行队列是**一条显式单链表**（`rq_head`/`rq_tail`，`t_rqnext` 串接），不是优先级队列：

- `kthread_poll()`：取 `rq_head`（**纯 FIFO，不看优先级**）→ 出队 → `curk = t` → `hal_switch_stack`；
- `kthread_run()`：`while (kthread_runnable()) kthread_poll();`，直到队列空；
- `proc_sched()` 每轮循环**先 `kthread_run()` 把内核线程跑空**，才轮到用户线程 —— 内核线程有优先权。

### 3.4 阻塞与唤醒

- `kthread_block()`：`rq_dequeue(t)`、`t_state = KT_BLOCK`、`sched_on_block()`（记录睡眠）、切回调度器；
- `kthread_wake(t)`：`t_sleep_ticks++`、`sched_on_wake()`（I/O 加成 + 选量子）、`t_state = KT_RUN`、`rq_enqueue()` 重新入队尾。

## 四、用户进程：`struct process` 的生命周期

### 4.1 分配：`proc_alloc()`

扫描 `procs[8]` 找 `P_FREE` → `bzero` → 设 `cwd = "/"`、`uk_p_mmap_base = MMAP_BASE`、**0/1/2 三个 fd 预置为控制台**（0 读、1/2 写）。

### 4.2 fork：`do_fork()`（`usermode.c:2693`）

```
proc_alloc()                       → 拿一个进程槽
copy_address_space_cow(cur->uk_pd) → 子进程地址空间（写时复制）
hal_address_space_activate(cur)    → 刷新父进程的脏 TLB
p->uk_pid = next_pid++ / uk_ppid = 父 pid / uk_pd = 新页目录
proc_add_thread(p, p->uk_pid)      → 创建子进程的主线程 TCB
p->uk_p_next_tid = pid + 100
复制 cwd / comm / fd 表（16 项）、vp->ref++、管道读写端计数 ++
p->uk_regs = *r; p->uk_regs.eax = 0;   → 子进程从 fork() 返回 0
```

写时复制靠 PTE 的 `PG_COW` 位：写异常进 `cow_fault()`，引用计数为 1 时直接去掉 COW 标记（免拷贝），否则 `page_alloc` + `bcopy` 复制一页（`usermode.c:2245` 附近，`cow: shared/faults/copies` 统计）。

### 4.3 vfork：`do_vfork()`

严格 vfork 应共享地址空间，但**用户态没有保护父栈帧的手段**（子进程的 libc 调用会踩父栈），所以这里用「**COW 私有拷贝 + 父进程阻塞到子 exec/exit**」这个安全的超集（`usermode.c:2747` 注释）。父进程 `kthread_block_tcb(0)` 后直接 `proc_sched()`。

### 4.4 exec：`exec_image()`

```
vfs_namei(path) → vp->ops->read() 把整个 ELF 读进 cd_map 缓冲
解析 argv / envp（envp 固定塞 "PATH=/bin:/sbin"）
build_image(...)  → 新建页目录 pd、映射各段、铺好用户栈，返回 entry/esp
mmap_teardown(cur)                → 旧地址空间的 mmap 记账先拆（S17）
cur->uk_pd = pd; hal_address_space_activate(pd)
r->eip = entry; r->uesp = esp; r->eax = 0
若当前是 vfork 子进程 → 唤醒阻塞的父进程，父进程 eax = 子 pid
```

注意：**exec 不换进程、不换 TCB**，只换地址空间和返回帧；旧映像直接被丢弃。用户程序来自 ISO9660（嵌入式 ramfs 仅作无光盘时的兜底）。

### 4.5 exit：`proc_exit()`

1. 找所有 `P_BLOCKED && pid == 父 pid` 的等待者：vfork 等待者拿 `eax = 子 pid` 被唤醒；wait4 等待者先**切到父页目录**写 `*uk_wait_status`（正常退出写 `code << 8`，信号终止写 `termsig & 0x7f`），再 `eax = 子 pid` 唤醒；
2. `fd_close()` 关掉全部 fd；
3. 释放主线程 TCB（`kthread_user_free`，同时从 `uk_p_threads` 链表摘除、`uk_p_nthreads--`）；
4. `mmap_teardown()` → 切回内核页目录 `IdlePTD` → `free_address_space()`（共享页缓存页在 teardown 时已 unmap，不会被错误回收）；
5. `uk_state = reaped ? P_FREE : P_ZOMBIE`（**父进程活着就留僵尸等 wait 收尸**）；
6. 若 `uk_ppid == 0`（init 退出）→ 打印 `[user exit N]` 并停机；否则 `proc_sched()`。

### 4.6 进程状态

| 常量 | 值 | 含义 |
|---|---|---|
| `P_FREE` | 0 | 槽位空 |
| `P_RUN` | 1 | 可运行（有线程在跑或就绪） |
| `P_BLOCKED` | 2 | 整体阻塞（仅单线程进程才会进入） |
| `P_ZOMBIE` | 3 | 已退出，等父进程 wait |

## 五、用户线程（同进程多 TCB）

### 5.1 创建：`thread_create()`（syscall 440）

```
tid = uk_p_next_tid++                     （主线程是 pid，其余从 pid+100 起）
if 用户给了栈顶 → 用它；否则：
    sva = 0xBFA00000 + ((tid & 0xf) << 16)     ← 每个 tid 一个 64 KB 槽
    user_map(pd, sva, page_alloc())
    user_map(pd, sva + 4096, page_alloc())     ← 2 页 = 8 KB 用户栈
    top = sva + 2*4096 - 16
t = kthread_user_spawn(cur, tid)          → 独立内核栈 8 KB，挂进 uk_p_threads
t->t_regs = *r; eip = 入口; eax = 参数; uesp = 栈顶; ebp = 0
t->t_sigmask = 父线程的信号屏蔽字
t->t_state = KT_RUN
```

**内核栈是每线程独立的 8 KB，用户栈也是每线程独立的 2 页** —— 这是 KLT 与 ULT 的本质区别：一个线程阻塞在内核里，同进程其他线程照常跑。

### 5.2 退出：`thread_exit_current()`（syscall 441）

| 情况 | 行为 |
|---|---|
| 主线程且 `uk_p_nthreads <= 1` | 走 `proc_exit()`，整个进程结束 |
| 主线程但还有其他线程 | 自己置 `KT_ZOMBIE` 并阻塞，**进程由兄弟线程续命** |
| 普通线程 | 置 `KT_ZOMBIE`、`prio_restore()` 归还继承的优先级、`uk_p_nthreads--`、唤醒所有 `t_join_tid == 自己` 的 joiner（把退出码写进对方 `eax`）；`t_detached` 则直接 `kthread_user_free()` |

### 5.3 join 与 detach

- `thread_join(tid)`（442）：目标已是 `KT_ZOMBIE` → 取退出码并释放；否则记 `t_join_tid`、`t_blocked_on = 目标 tid`、**`prio_inherit(joiner, joinee)` 把优先级借给被 join 者**，然后阻塞 + `proc_sched()`。
- `thread_detach(tid)`（445）：置 `t_detached = 1`，退出时自动释放；已被 join 过则返回 `EINVAL`。
- `thread_yield()`（444）：保存上下文、`proc_sched()` 让出 CPU。
- `thread_self()`（443）：返回 `t_tid`（无 TCB 时返回 pid）。

### 5.4 用户态接口

`libc/src/pthread.c` 只是薄包装（`__syscall3`）：

| pthread API | syscall |
|---|---|
| `pthread_create` | 440 `thread_create(pthread_tramp, &pstart{fn,arg}, 0)` |
| `pthread_exit` | 441 |
| `pthread_join` | 442 |
| `pthread_self` | 443 |
| `pthread_yield` | 444 |
| `pthread_detach` | 445 |
| `sched_setprio` / `sched_getprio` | 446 / 447 |

用户传 `stack = 0`，**栈由内核分配**；`pthread_tramp` 是 libc 里的跳板，负责从 `struct pstart` 取出 `fn/arg` 再调用，返回后走 441。库内线程上限 `NTHREADS = 4`（静态 `pss[4]` 表）。

## 六、用户态模型与内核态模型的关系

用户态用的是**BSD 进程模型 + POSIX 线程 API**：进程靠 `fork/execve/wait4/exit` 表达，线程靠 `pthread_*` 表达，两者是"容器"和"容器内的执行流"的关系。内核里只有两个真实对象：`struct process`（资源与地址空间）和 `struct kthread`（唯一可被调度的执行体）。**两套模型不是一一照搬，而是"进程做容器、TCB 做调度"的投影关系。**

```
用户态 API     fork() execve() wait4() exit()        pthread_create() join() ...
                   │                                        │
系统调用        2 / 59 / 7 / 1                        440 / 442 / 441 / 445
                   │                                        │
内核对象     struct process ◄──────── t_proc ─────────► struct kthread
            （8 槽：地址空间/fd/mmap/estcpu）      （16 槽：内核栈/上下文/优先级）
                   └────── uk_p_threads 链表 ─────► 该进程的全部 TCB
```

最关键的一点：**用户态没有"线程表"**。`/dev/ps` 的 `proc_list_text()` 只遍历 `procs[NPROC]`，每个进程一行（PID/PPID/PGRP/STAT/COMM），完全不体现线程；用户只能通过 `thread_self()`（443）知道自己那条线程的 tid。反过来，**内核态的调度器完全不看 `procs[]` 的状态机**（除整进程阻塞那条例外），它只遍历 `kthreads[NKTHREAD]`，进程只是 `t_proc` 指向的容器。

### 6.1 用户态动作 → 内核对象

| 用户态动作 | 产生的内核对象 | 关键代码 |
|---|---|---|
| 系统启动/进入用户态 | 1 个 `proc` + 1 个主线程 TCB（`t_tid == pid`） | `proc_alloc`（`usermode.c:2306`）+ `proc_add_thread`（`usermode.c:2290`） |
| `fork()` | 新 `proc` + 新主线程 TCB + COW 地址空间 | `do_fork`（`usermode.c:2693`） |
| `vfork()` | 同上，额外把父进程置 `P_BLOCKED` 等子 exec/exit | `do_vfork`（`usermode.c:2740`） |
| `execve()` | **不新建任何对象**，只换 `uk_pd` 与返回帧 | `exec_image`（`usermode.c:3443`） |
| `pthread_create()` | **+1 个 kthread**（`t_proc` 指向本进程，挂进 `uk_p_threads`） | `thread_create`/440 → `kthread_user_spawn` |
| `pthread_exit()`（非主线程） | TCB 置 `KT_ZOMBIE`，等 join 或 detach 回收 | `thread_exit_current` |
| `pthread_join()` | 不建对象，只记 `t_join_tid` 并做优先级继承 | 442 |
| `exit()` / 被信号终止 | 整个 `proc` 结束，所有线程一起消失 | `proc_exit`（`usermode.c:2785`） |

所以可以写成一条等式：

```
一个用户进程 = 1 个 struct process
             + (1 + n) 个 struct kthread        n = 额外线程数
             + 1 个页目录（uk_pd，被这 1+n 个 TCB 共享）

全系统 TCB = Σ(1 + n_i) + 内核线程 2 个（softirq、aio） ≤ 16
全系统 proc ≤ 8
```

反过来，**内核线程不属于任何进程**（`t_proc == NULL`），没有 pid、不在 `procs[]` 里，用户态完全不可见 —— 它们和用户线程只是共用同一个 16 槽 TCB 池。

### 6.2 三处刻意的不对齐

这三处是理解两个模型关系的关键：内核并没有机械复制用户态的划分。

**① 调度单位是 TCB，记账单位是 proc**

| 数据 | 挂在哪 | 含义 |
|---|---|---|
| `t_prio` / `t_base_prio` / `t_quantum` / `t_state` | **TCB** | 每个线程独立的优先级、量子、状态 |
| `uk_estcpu` / `p_nice` | **proc** | 整个进程的 CPU 使用量与 nice 惩罚 |

`sched_effective_prio()` 的算法是"本线程的 base 优先级 − 本进程的 estcpu/nice 惩罚"，于是：**同一进程的线程共享惩罚，但可以有各自的 base 优先级**。注意 446/447 只作用于**调用者自己**的 TCB（`cur_thread->t_prio`，不接受 tid 参数），不能替别的线程设优先级。

**② 主线程的寄存器备份在 `proc` 里，不在 TCB 里**

`proc->uk_regs` 是主线程的上下文，额外线程才用 `t->t_regs`。这是为了不改动 fork/wait/pipe 这些只认进程的老路径（`usermode.c:3652` 注释）。代价是每次取上下文都要判 `main_thread = (t->t_tid == p->uk_pid)`：

```c
regs = main_thread ? &p->uk_regs : &t->t_regs;   /* proc_sched() */
```

**③ 阻塞语义按线程数分叉**

```c
if (p->uk_p_nthreads <= 1)  p->uk_state = P_BLOCKED;   /* 单线程：整进程阻塞，经典 BSD 语义 */
else                        t->t_state  = KT_BLOCK;    /* 多线程：只阻塞这个 TCB，进程仍 P_RUN */
```

`tcb_runnable()` 和 `proc_sched()` 都按这条规则判断，所以"一个线程卡在 read()，另一个线程还能算"只在多线程进程里成立 —— 单线程进程仍然表现得像传统 BSD 进程。**这就是 KLT 与 ULT 的分水岭。**

### 6.3 跨层的可见性与副作用

| 现象 | 原因 |
|---|---|
| 用户态看不到线程 | `/dev/ps` 只渲染 `procs[]`，没有线程列；只能 `thread_self()` 看自己 |
| 用户态看不到内核线程 | `t_proc == NULL`，不在 `procs[]`，无 pid，不能被 fork/wait/kill 操作 |
| 但内核线程会拖慢用户线程 | `proc_sched()` 每轮**先 `kthread_run()` 把内核线程队列跑空**，再挑用户线程 |
| 信号默认是**进程级**的 | 默认动作终止时走 `proc_exit(sig)`，整个进程所有 TCB 一起消失，`wait4` 只看到 `termsig & 0x7f` |
| 但存在**线程级**信号入口 | `tgkill(tid, sig)`（syscall 451）把信号记到 `t->t_sig`；`sig_deliver()` 把进程待决位与线程待决位取并集，再按**该线程的** `t_sigmask` 过滤。libc 未封装它 |
| 只有进程有身份 | pid/ppid/pgrp/comm 全在 `struct process`；线程只有 tid，没有独立身份、没有 tid 级 nice |

### 6.4 与经典模型的对照

| | 经典 ULT（N:1） | **本系统（1:1 KLT）** | 经典重量级进程 |
|---|---|---|---|
| 用户线程 ↔ 内核执行体 | n : 1 | **1 : 1**（每线程 1 TCB + 8 KB 内核栈） | 1 : 1 |
| 地址空间 | 同进程共享 | 同进程共享（同一 `uk_pd`） | 每进程独立 |
| 一个线程阻塞 | 整进程阻塞 | **只阻塞该 TCB** | — |
| 切换开销 | 用户态跳转，无陷核 | TCB 切换 + 换内核栈；同 `pd` 时不换 CR3 | CR3 重载 + TLB 冲刷 |
| 上限 | 受用户栈限制 | 16 个 TCB（含 2 个内核线程） | 8 个 proc |

也就是说：**它没有走"用户态线程库 + 内核只认进程"的 N:1 老路，而是每个用户线程都配一个内核栈和一个 TCB 的 1:1 模型**；只是 TCB 池是固定 16 槽的静态数组，且主线程的寄存器备份为了兼容留在了 `struct process` 里。

### 6.5 代码阅读发现（未运行时验证）

1. **TCB 池是两个模型共享的稀缺资源**：内核线程和用户线程抢同一个 `kthreads[16]`，softirq/aio 固定占 2 个；`kthread_user_spawn()` 满槽返回 0，`thread_create()` 返回 `-EAGAIN`，用户态只看到 pthread_create 失败。
2. **失败路径不回滚用户栈**：`thread_create()` 先 `user_map()` 两页用户栈，再去申请 TCB；`kthread_user_spawn` 返回 0 时这两页没有 `user_unmap`/回收。
3. **`proc_exit()` 只回收一个 TCB**（`kthread_user_free(cur->uk_p_thread)`，`usermode.c:2824`），没有遍历 `uk_p_threads` 清理兄弟线程。若进程带着多个线程被 `exit()` 或信号终止，其余 TCB 会留在 `kthreads[]` 里（槽位泄漏），且地址空间已被释放。
4. **`uk_p_thread` 只在 `proc_add_thread()` 赋值**（`usermode.c:2299`），上下文切换时不更新；它和调度器维护的 `cur_thread` 不是一回事，所以 `proc_exit()` 靠它释放的未必是调用者本身。

## 七、两种上下文切换路径

| | 内核线程 | 用户线程 |
|---|---|---|
| 切换原语 | `kswitch()` 换内核栈 | 构造/改写中断帧 + `iret` |
| 入口 | `kthread_trampoline` → `t_entry` | `resume_user(pd, regs)`（`usermode_asm.s:56`） |
| 保存内容 | 4 个 callee-saved 寄存器 | 完整 `struct sysregs`（含 eip/cs/eflags/uesp/ss） |
| 触发点 | `kthread_poll` / `kthread_run` | `proc_sched()` / `sched_tick()` |

`resume_user()` 的关键设计：

```asm
movl 4(%esp), %eax      ; pd（页目录物理地址）
testl %eax, %eax
jz 1f                   ; pd == 0 → 同一地址空间，保持 CR3（同进程线程切换省掉 TLB 刷新）
movl %eax, %cr3
1: iret                 ; 用预构造的 eip/cs/eflags/uesp/ss 直接回用户态
```

所以**进程切换 = CR3 重载 + TSS.esp0 更新**（`hal_kernel_stack_set(t->t_stack_top)`），**同进程线程切换只换栈和寄存器**。

`proc_sched()` 里判断 `if (p->uk_pd != cur_pd)` 才 `hal_address_space_activate()`，就是这个优化。

## 八、阻塞语义：整进程阻塞 vs 单线程阻塞

`kthread_block_tcb(wchan)`（`usermode.c`）：

```c
if (cur->uk_p_nthreads <= 1) {     /* 单线程进程：整体阻塞 */
        cur->uk_state = P_BLOCKED;  /* 经典 BSD wait/pipe 语义，所有唤醒者只看进程状态 */
        return;
}
t->t_state = KT_BLOCK;             /* 多线程：只阻塞这一个 TCB */
t->t_wchan = wchan;                 /* 进程保持 P_RUN，兄弟线程继续跑 */
```

配套的 `tcb_runnable()`（`usermode.c:2522`）也遵守同一规则：`P_BLOCKED && uk_p_nthreads <= 1` 才算不可运行。这是 KLT 相对 ULT 的核心优势 —— **一个线程卡在 read() 上，同一个进程的另一个线程还能继续算**。

## 九、启动与运行全景

```
main()                                   (glue.c)
  ├─ 探测并挂载根文件系统（JLFS/FFS/ISO/tmpfs）
  ├─ kthread_init()        16 个 TCB 槽，全部 KT_FREE
  ├─ softirq_init()        spawn "softirq" 内核线程
  ├─ vfs_aio_init()/aio_user_init()  spawn "aio" 内核线程
  ├─ clock_init()          100 Hz IRQ0
  └─ monitor()             交互 monitor（mem/kthread/schedtest/install/usermode…）

monitor → "usermode" → 进入用户态跑 /sbin/init
  → init fork/exec /bin/sh → sh 里每个命令都是 fork+exec
  → 每次时钟中断（用户态）都可能被抢占：sched_tick() → fair_pick_next()
  → 没有可运行线程时 proc_sched() 打印 "[no runnable process]" 并 halt
```

## 十、限制与设计取舍

1. **TCB 16 个是硬上限**：8 个进程各占一个主线程 TCB，加上 softirq、aio 两个内核线程，**只剩 6 个槽给额外用户线程**；libc 的 `pthread_create` 又限制每进程 4 个，所以实际很难打满。
2. **进程数 8 个固定**、pid 单调递增不做回收（`next_pid++`），槽位复用靠 `P_FREE`。
3. **fork 复制整张 fd 表（16 项）**，没有 fd 的惰性共享；`vp->ref++`、管道读写端计数必须手工维护，漏了就会提前 `close`。
4. **vfork 不是真正的共享地址空间**，是 COW 私有拷贝 + 父阻塞的安全超集（注释里明确写了理由）。
5. **主线程上下文在 `proc->uk_regs`，其余线程在 `t->t_regs`** —— 这种双存储是为了兼容老代码，代价是每处切换都要判断 `t_tid == uk_pid`。
6. **调度器只按 TCB 走，但公平性按进程算**（`uk_estcpu` 挂在 `struct process` 上），避免一个 100 线程的进程拿到 100 倍 CPU。调度细节见 `os_sched.md`。
7. **无内核抢占**：内核态只在系统调用返回的安全点（`need_resched`）才可能切走，内核临界区内绝不切换。
8. **异常不崩内核**：用户态异常（SIGSEGV 等）只杀掉该进程所有线程、把信号记进 `uk_p_termsig`，让 `wait4` 报 `WIFSIGNALED`。

## 十一、一句话总结

**进程 = `struct process`（8 槽，持地址空间/fd/mmap/estcpu），线程 = `struct kthread`（16 槽，持内核栈与上下文）；调度单位是 TCB，地址空间单位是进程。** 内核线程靠 `kthread_spawn` 预置栈帧 + `kswitch` 换栈启动；用户线程靠 `thread_create` 分配 8 KB 独立内核栈与 2 页独立用户栈，靠 `resume_user` 的 `iret` 帧恢复；fork 用 COW 复制地址空间，exec 只换地址空间与返回帧，单线程进程整体阻塞、多线程进程只阻塞单个 TCB。

两套模型的关系一句话：**用户态的"进程/线程"是这两个内核对象的投影 —— 1 个用户进程 = 1 个 `proc` + (1+n) 个 TCB；用户线程与内核线程共用同一个 16 槽 TCB 池，但只有 `proc` 在用户态可见。** 用户态看到的是 BSD 进程语义与 pthread 接口，内核看到的是"一堆 TCB + 它们各自指向的资源容器"。

## 十二、S400：用户态同步原语

BSD45 的用户态线程库补上了 mutex / condition variable，供用户程序内部协作使用：

- `pthread.h` 新增 `pthread_mutex_t` / `pthread_cond_t` 及
  `pthread_mutex_{init,lock,trylock,unlock,destroy}`、
  `pthread_cond_{init,wait,signal,broadcast,destroy}`
- 实现（`libc/src/pthread.c`）基于 BSD45 的单核协作式调度：线程只在 syscall/
  时钟 tick 让出，不会真并行，因此 mutex 用 `volatile` 状态 + `pthread_yield`
  自旋即可正确互斥，无需内核 futex；condvar 用 generation 计数防止 signal 丢失
- 零内核改动，纯用户态 C

这是进程同步能力在用户态的最后一块——此前内核态有 S200 的 mutex/condvar，
但用户态 `pthread.h` 只有 create/join/self/yield，服务写不了需要锁的协作逻辑。
