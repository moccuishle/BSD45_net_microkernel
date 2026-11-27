# BSD45 IPC 与共享页实现（S300：KP-4 / KP-6 / KP-7）

> 内核里的三块底层原语：`block_on/wakeup` 通用阻塞通道（KP-4）、
> 基于端口的 `send/recv` 消息 IPC（KP-6）、共享物理页映射（KP-7）。
> 它们是**通用内核机制**：`ipctest` 直接验证，进程间传大块数据也能用；
> 与"把文件系统/驱动搬到用户态"没有绑定关系（那个方向已于 2026-11-28 放弃，
> 见 `RESUME-TASK.md` §6.2.18）。
> 默认不激活（无端口注册、无 syscall 调用时行为与基线一致），遵守
> `asmble.md`（汇编只能在 `arch/` 下，新增代码全部为可移植 C，含中英注释）。

## 1. 实现内容

### KP-4 通用阻塞通道（block_on / wakeup）

- 文件：`kernel/thread/kipc.c`，声明 `kernel/include/sys/kipc.h`
- 32 桶通道哈希表 `chanq[]`，每桶是一个已有的 `struct waitq`（来自 S200 的 `sync.c`）
- API：`block_on(chan)` / `wakeup(chan)` / `wakeup_one(chan)`
- `chan_hash()` 把任意通道地址稳定哈希到桶，纯 C，无汇编

### KP-6 端口消息层（send / recv）

- 固定 16 项端口表 `ports[]` + 每端口 4 个有界消息槽（共 64 槽）
- `struct ipc_msg`：`from`（内核填发送方 pid）/ `type` / `len` / `pa`（共享页物理地址）/
  `reply_port` / `inline_data[64]`
- 端口：`port_register` / `port_release` / `ipc_send` / `ipc_recv` / `ipc_proc_exit`
- 语义：
  - `send` 入队并 `wakeup_one` 接收者；内核填 `from`；本身不阻塞（避免单线程自死锁）
  - `recv` 队列空时——内核线程上下文在接收队列真正睡眠；**用户线程上下文返回
    `-EAGAIN`，由调用方轮询重试**（见下方"设计取舍"）
  - 端口无属主（服务已退出）时 `recv` 返回 `-EIO`，实现故障隔离契约
  - 服务退出 `port_release` 唤醒所有等待者并回收以该端口为回复端口的消息槽
  - 进程退出 `proc_exit` 调 `ipc_proc_exit(pid)` 关闭其所有端口

### KP-7 共享物理页映射（map_shared）

- `map_shared(pid, va, pa, prot)`：把物理页 `pa` 映射到进程 `pid` 的用户虚地址 `va`
- `unmap_shared` 解除映射；`shared_page_window` 提供内核侧填充窗口；
  `shared_page_release` 释放物理页
- 复用 usermode.c 现成的页表机制：`proc_pd_by_pid` 找页目录 → `user_map_shared`
  包装 static 的 `user_map_prot` → 当前进程映射后 `hal_address_space_activate` 刷 TLB
- 生命周期：**谁分配谁释放 + ACK** —— 生产者 `vfs_page_alloc` 分配、填充、随消息发 `pa`；
  消费者读取后发 ACK；生产者收到 ACK 才释放。进程退出时 `free_address_space` 兜底回收

## 2. syscall 接口（编号 452–459）

| 编号 | 名称 | 参数（ebx/ecx/edx/esi） | 返回 |
|---|---|---|---|
| 452 | `port_register` | port | 0 / -EINVAL / -EBUSY |
| 453 | `port_release` | port | 0 / -EINVAL |
| 454 | `ipc_send` | port, *msg | 0 / -EINVAL / -EAGAIN |
| 455 | `ipc_recv` | port, *msg | 0 / -EAGAIN / -EIO / -EINVAL |
| 456 | `map_shared` | pid, va, pa, prot | 0 / -EINVAL / -ESRCH |
| 457 | `unmap_shared` | pid, va | 0 / -EINVAL / -ESRCH |
| 458 | `ipc_page_alloc` | — | 物理页地址（0=失败） |
| 459 | `ipc_page_free` | pa | 0 |

用户态镜像结构 `struct ipc_msg` 在 `libexec/ipctest.c` 中给出，布局须与内核一致。

## 3. 设计取舍（如实说明）

1. **用户线程 recv 用 EAGAIN 轮询而非真正阻塞**。BSD45 的用户线程阻塞需要
   "保存 regs + 续传参数 + proc_sched" 的复杂协议（见 `usermode.c` 的 pipe 空读路径）；
   在内核态重建这套机制风险高。因此对用户线程采用非阻塞 + 轮询，内核线程上下文
   则可在接收队列真正睡眠。这是当前实现的明确边界，后续接入 `fs_server` 时若要
   真会合阻塞，需在 usermode.c 补一套续传协议。
2. **send 不阻塞**。契约里的"发送方阻塞在发送队列"未实现；当前 send 只入队。
   对只有一个在飞请求的服务模型足够，但严格会合语义尚未完整。
3. **单核验证**。默认 `SMP disabled`（1 CPU），互斥用 spinlock（单核退化为禁抢占），
   未在真双核下压测 IPC 竞争。
4. **端口表固定 16 项、槽位固定 64 个**，无动态扩容；满时 send 返回 -EAGAIN。

## 4. 构建与验证

### 构建

Ubuntu（本轮验证环境：26.10 / gcc 15.3 / QEMU 11.0.3 / xorriso 1.5.6）：

```bash
cd ~/bsd45-build
bash build_all.sh          # 依赖 gcc-multilib bison flex xorriso
```

Windows（原生 i686 链路，需 Python 在 PATH 以运行 pe_bss_fix.py）：

```powershell
$env:PATH = "C:\Users\Administrator\.workbuddy\binaries\python\versions\3.13.12;" + $env:PATH
.\build_kernel.ps1         # gcc(-m32) PE -> COFF -> objcopy ELF
.\build_sh.ps1             # 用户态工具（含 ipctest）
.\build_iso.ps1
```

### 自测与验收

```bash
# 内核 monitor 自检（ipctest/ipcinfo/synctest）
bash tools/run_qemu_test.sh -Keys "ipctest|ipcinfo" -WaitFor "ipc: ports"

# 用户态完整验收：消息往返 + 跨进程会合 + 共享页
bash tools/run_qemu_test.sh -Keys "shell|/bin/ipctest|exit 0" -WaitFor "user exit"
```

验证结果（Ubuntu 实机 QEMU）：

- 内核 `ipctest: PASS`，`ipcinfo: ports 0/16 in use`
- 用户态 `ipctest: PASS`：
  - `ipc: roundtrip ok (from=2)` —— 内核正确填发送方 pid
  - `ipc: crossproc ok` —— fork 子进程注册端口并收到父进程消息
  - `ipc: sharedpage ok` —— 物理页映射进用户 VA，写入读回正确，解除并释放
- 默认单核零回退：`tools/all_tests.sh` 与基线一致（3 个历史遗留失败项
  subsys/ipl/preempt 经干净基线 `30a5620` 对比确认非本次引入）

## 5. 变更文件清单

**新增**
- `kernel/include/sys/kipc.h` —— 端口/消息/共享页结构与 API，中英注释
- `kernel/thread/kipc.c` —— KP-4/6/7 实现 + `ipc_selftest` + `ipc_print`，中英注释
- `libexec/ipctest.c` —— 用户态三项验收程序，中英注释

**修改**
- `kernel/sys/usermode.c` —— 8 个 syscall case；桥接函数
  `user_map_shared/user_unmap_shared/user_scratch_window/proc_pd_by_pid/proc_cur_pid`；
  `proc_exit` 调用 `ipc_proc_exit`；共享页映射后刷 TLB
- `kernel/sys/mon.c` —— monitor 命令 `ipctest` / `ipcinfo`
- `build_kernel.sh` / `build_kernel.ps1` —— 加入 `kipc.o`
- `build_sh.sh` / `build_sh.ps1` —— 用户态工具列表加入 `ipctest`
- `build_iso.sh` / `build_iso.ps1` —— 把 `/bin/IPCTEST.ELF` 打包进 ISO

## 6. 定位

这三块是内核里的通用机制，与用户态服务化无关：

- KP-4 给任何需要睡眠等待的内核路径提供统一的 `block_on`（复用 S200 的 waitq）
- KP-6 提供跨进程端口通信，端口无属主时 `recv` 返回 `-EIO`
- KP-7 让大于 64 字节的数据走共享页，消息只传 `pa`

## 7. S400 增强：真会合阻塞

S300 的 IPC 补齐了"真会合"语义（替代原来的 `-EAGAIN` 轮询）：

- `kprocess.h` 新增续传字段 `uk_ipc_waiting / uk_ipc_port / uk_ipc_msguser`
- `SYS_IPC_RECV`：队列空且端口有 owner 时，保存 regs 到 `uk_regs`、
  `kthread_block_tcb()` + `proc_sched()` 真正睡眠（不再是 EAGAIN 轮询）
- `SYS_IPC_SEND`：有阻塞接收者时用 `pipe_copy` 跨进程写其用户缓冲、填
  `uk_regs.eax=0`、`proc_wake()` 直接投递，不入队
- `ipc_fail_waiters()`：端口释放时给所有阻塞接收者写 -EIO 并唤醒，由
  `port_release` / `ipc_proc_exit` 调用

验证：`ipctest` 的跨进程会合（子进程先 recv 真阻塞、父进程 send 直接投递）PASS。
