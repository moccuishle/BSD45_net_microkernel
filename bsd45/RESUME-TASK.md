# BSD45-ELF-x86 任务恢复文本

更新时间：2026-11-16（Asia/Shanghai）
状态：**S17 完成**（缓存系统移植：buffer cache + name cache + page cache + mmap）；
Windows（MSYS2 ucrt64 GCC 16.2 + `xorriso`）**17 套验收全绿**，Linux(Ubuntu 26.04 LTS)
走同一套 `.sh` 脚本。
下一步：B4 UFS/FFS 暂缓；可做真 `fg`/`bg`、per-process tty/session、
或路线 A（真·移植 4.5BSD 内核核心，见 0.15 / S100）。
仓库：https://github.com/moccuishle/BSD45_cache-mmap_filesystem

本机环境（网吧 Windows，无管理员、防火墙不可关）：

| 组件 | 位置 |
|---|---|
| i686 工具链 | `C:\msys64\mingw32\bin`（GCC 16.2.0，pacman 装的 mingw-w64-i686-gcc） |
| mkisofs | `C:\Program Files (x86)\VMware\VMware Workstation\mkisofs.exe` |
| QEMU | `C:\msys64\ucrt64\bin\qemu-system-i386.exe`（11.1.3） |
| 4.5BSD 源码 | `kernel\`（sys 树）+ `libc\arch\bsd\`（用户态）；`src\` 已合并删除 |
| 4.3BSD-Reno 源码 | `C:\Users\Administrator\Desktop\work\reno-src-main`（ctype 等参考） |

> 注意：`C:\msys64\mingw32\i686-w64-mingw32\bin\as.exe` / `ld.exe` 是坏的
> （0xC0000139/0xC0000135）。所有编译命令必须带 `-B C:/msys64/mingw32/bin/`，
> 构建脚本里的 `Get-MingwBin` + `-B` 已处理。

---

## 0. 在新机器上恢复（clone 仓库 → 继续 S10）

> S10 的完整任务定义在本文档最前面（标题下方第一个大节），
> 打开文件往下滚几屏即可看到 4 个子任务与验收方式。


### 0.0 一键恢复（新机器 / 新会话：只读这一节就能开工）

> **分工（2026-11-14 约定）**
> - **环境安装由用户负责**（装 MSYS2 / gcc / qemu / bison / flex / xorriso / git）。
>   执行者**不要**自己装环境、不要跑 pacman、不要改镜像源。
> - **执行者从 0.0.2 开始**：验证工具链 → 构建 → 15 套验收 → 按 0.0.5 继续开发。
> - 环境确实缺东西时，只报告缺什么（贴出 0.0.1 的检查输出），由用户补装。

### 0.0.1 环境（用户负责；执行者只做这一条检查）

```bash
export PATH=/usr/bin:/c/msys64/usr/bin
for t in /c/msys64/ucrt64/bin/gcc.exe /c/msys64/ucrt64/bin/ld.exe \
         /c/msys64/ucrt64/bin/objcopy.exe /c/msys64/ucrt64/bin/qemu-system-i386.exe \
         /c/msys64/usr/bin/git.exe /c/msys64/usr/bin/xorriso.exe; do
	[ -x "$t" ] && echo "ok   $t" || echo "MISS $t"
done
```

预期 6 行全 `ok`。有 `MISS` 就把这几行贴给用户，然后停止（不要自己安装）。

构建还需要 ucrt64 gcc 支持 `-m32`（本机没有 i686 工具链，全项目走 x86_64 gcc 的
32 位模式）。快速确认：

```bash
printf 'int main(void){return 0;}\n' > /tmp/t.c
/c/msys64/ucrt64/bin/gcc.exe -m32 -c -o /tmp/t.o /tmp/t.c; echo "gcc -m32 exit=$? (expect 0)"
```

（安装细节与踩坑记录在仓库根的 `Ai_Coding_Unix_install_gccenv.md`，仅供参考。）

### 0.0.2 取代码并构建（Windows）

```bash
# 所有命令都走 bash；PATH 缺 /usr/bin 会到处 command not found
export PATH=/c/Windows/System32/WindowsPowerShell/v1.0:/usr/bin:/c/msys64/usr/bin
cd /c/Users/Administrator/Desktop/work        # 或 clone 出来的仓库根

# 首次构建会自动从 4.5BSD-jialiang.tar.gz 抽出源码树（可选，仓库已带）
cd bsd45
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build_all.ps1
# 预期：最后两行
#   ISO: ...\out\BSD45-ELF-x86.iso (4591616 字节左右), kernel at LBA 2048
#   built ...\build\bsd45.elf
```

Linux 等价（已实测 Ubuntu 26.04）：

```bash
sudo apt install build-essential gcc-multilib libc6-dev-i386 binutils \
                 genisoimage qemu-system-x86 bison flex python3
cd bsd45 && bash build_all.sh
```

### 0.0.3 验收（17 套，必须全绿）

```bash
cd /c/Users/Administrator/Desktop/work/bsd45
bash tools/all_tests.sh
# 预期最后两行：
#   ---- suites failed: 0
#   ALL SUITES PASSED
```

覆盖：S3 S4 S5 S6 S7 S8 S9 S10.1 S10.2 S10.3 S10.4 S11 tools S12 S13
**S17 caches** **S17 mmap**（后两套见 0.16）。

### 0.0.4 失败处理（按症状查，三分钟能定位）

| 症状 | 原因 | 处理 |
|---|---|---|
| 十几套同时 `NO VERDICT`、ISO 只有 ~2.3 MB | 残留 QEMU/VMware 占着 `out\BSD45-ELF-x86.iso`，`build_iso` 写出截断镜像 | `powershell -NoProfile -Command "Get-Process qemu-system-i386 \| Stop-Process -Force"` → `rm -f out/BSD45-ELF-x86.iso` → 重跑 `build_iso.ps1` |
| `'powershell' is not recognized` | bash PATH 缺 WindowsPowerShell 目录 | 补 `/c/Windows/System32/WindowsPowerShell/v1.0` |
| `grep: command not found` | bash `--norc` 没带 MSYS2 PATH | 补 `/usr/bin:/c/msys64/usr/bin` |
| `syntax error near unexpected token` | 把带 `"` `\|` `$` 的 bash 塞进了 `bash -c '...'`（PowerShell 先解析一遍） | 落成 `.sh` 文件再执行 |
| `gcc` 静默返回 1、无任何错误 | `-B` 指向不存在的工具链目录，找不到 `as.exe` | `tools-find.ps1` 的 `Find-CcBin` 已处理；不要写死 `-B C:/msys64/mingw32/bin/` |
| 内核启动就 TRAP 12 / GDT 乱 | 描述符结构体没 packed（GCC16 位域布局） | 见坑 53：三个 descriptor 都要 `__attribute__((packed))` |

更多细节见仓库根的 `shell.md`（PowerShell→bash 的固化写法与避坑）。

### 0.0.5 恢复后第一件事 + 下一步任务

1. 先确认 0.0.3 全绿（尤其 **S13 的 `test_ps`** 还没在完整 15 套里跑过）
2. 然后按顺序做（每次只做一个，跑全 15 套 + push 再进下一个）：
   - `/bin/uname`、`/bin/true`、`/bin/false`（各 ~10 行，最快）
   - `/bin/ps` 增强：进程名 `comm`、`-a`/`-x` 过滤
   - `/bin/kill` 支持 `%job` 写法（需 sh 作业表配合）
   - 真 `fg`/`bg`：sh 监视模式 + `tcsetpgrp` + 后台读终端 `SIGTTIN`（最大项）
   - 架构方向：**宏内核**（微内核方向已于 2026-11-28 放弃，见根 `RESUME-TASK.md` §6.2.18）
3. 提交与推送规则：**当前仓库为 `BSD45_libexec_fulll_userland`；若用户给了新地址，
   先 `git remote set-url origin <新地址>` 再 push；未给地址时按用户指令决定是否推送。**

## 0.1 取代码

```bash
git clone git@github.com:moccuishle/BSD45_rootfs_overlay.git
cd BSD45_rootfs_overlay
# 源码已合并进仓库：kernel/ 是完整 4.5BSD sys 树，libc/arch/bsd/ 是 /bin/sh 需要的
# 4.5BSD 用户态源码；原来的 bsd45/src/（整棵 4.4BSD 解包树）已删除。
# 构建不依赖任何 tarball，clone 完直接 build_all.ps1 / build_all.sh。
```

### 0.2 Linux（推荐，已实测）

```bash
sudo apt install build-essential gcc-multilib libc6-dev-i386 binutils \
                 genisoimage qemu-system-x86 bison flex python3
cd bsd45
bash build_all.sh          # userland -> /bin/sh -> /test -> kernel -> ISO
bash verify.sh             # 启动 + 检查内核 banner / ISO 根
bash tools/test_s3.sh      # 7 套：s3 s4 test_proc test_tty s7 s8 s9
```

产物：`bsd45/out/BSD45-ELF-x86.iso`（约 4.2 MB）、`bsd45/build/bsd45.elf`。

### 0.3 Windows（历史路径，PS 脚本保持可用）

```powershell
$env:MKISOFS='C:\Program Files (x86)\VMware\VMware Workstation\mkisofs.exe'
$env:QEMU='C:\msys64\ucrt64\bin\qemu-system-i386.exe'
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test_s9.ps1
```


> 目录调整：`kernel/libkern` → `kernel/lib`，`include/libkern` → `include/lib`，`kernel/other/kern` → `kernel/sys`。
> 脚本中的引擎名已统一为 4.5BSD（内核 banner 同步修改）。

### 0.4 关键路径速查

| 内容 | 位置 |
|---|---|
| 内核入口/启动 | `kernel/arch/i386/i386/locore.s`、`kernel/arch/i386/isa/machdep.c` |
| 内核运行时 glue | `kernel/other/glue.c`、`stubs.c`、`strings.c`、`mon.c` |
| 4.4BSD kern/* 核心源码 | `kernel/sys/`（原 `kernel/other/kern/`，S10 前整理） |
| 用户态/系统调用 | `kernel/other/usermode.c`、`usermode_asm.s` |
| VFS / 后端 | `kernel/fs/vfs.c`、`kernel/fs/isofs.c`、tmpfs 在 `vfs.c` 内 |
| 进程/fd/管道 | `kernel/other/usermode.c`（`struct uproc`、`proc_sched`、`fd_*`） |
| libc 子集 | `libc/src/`（`libc_sys.c` 是 syscall 粘合层） |
| 用户工具源码 | `libc/src/{echo,ls,cat,mkdir,printf,test}.c` |
| 4.5BSD sh | `libc/arch/bsd/bin/sh`（构建时拷到 `build/shgen`） |
| 链接脚本 | `boot/kernel.ld`（Linux ELF）；Windows 走 PE + defsym |
| 测试驱动 | `tools/run_qemu_test.sh`（串口 TCP + `/dev/tcp`） |

### 0.5 已知坑（补）

44. **符号下划线**：mingw 自动给 C 符号加 `_`，Linux ELF 不加，而 4.4BSD
    汇编按 `_name` 引用 → Linux 构建统一加 `-fleading-underscore`。
45. **64 位除法**：加了下划线后 gcc 要 `___divdi3`，libgcc 只有 `__divdi3`
    → `build_sh.sh` 链接 4.4BSD `kernel/lib` 的 quad 例程。
46. **测试脚本的 `|`**：`run_qemu_test.sh` 用 `|` 分隔多条输入，真实管道要写
    `\|`（如 `echo hi \| cat`）。
47. **串口读取**：后台 `cat` 要用 `stdbuf -o0`，否则最后一行
    `[user exit N]` 会丢。

### 0.6 Windows 上只有 ucrt64（x86_64）GCC 时怎么建（2026-11-14 实测）

本机没有 `C:\msys64\mingw32`，只有 `ucrt64` 的 GCC 16.2.0。`gcc -m32` 依然
能出 32 位 COFF（gas/ld/objcopy 都是多目标的），所以整条 Windows 构建链照跑，
但有下面这些差别（都已在脚本里处理，别再手工改回去）：

48. **`-B` 目录要跟着工具链走**：原来写死 `-B C:/msys64/mingw32/bin/`，那个
    目录不存在时 `as.exe` 找不到，gcc 会**静默返回 1（无任何报错）**。现在
    `tools-find.ps1` 里 `Find-CcBin` 按 `mingw32 → ucrt64 → mingw64` 找，
    `Get-CcBinArg` 生成 `-B` 参数，四个 build 脚本都用它。
49. **没有 32 位 libgcc**：`-print-libgcc-file-name` 给的是 x86_64 的，链进
    i386 PE 会报 "i386:x86-64 architecture ... incompatible"。现在用户态自己
    提供：`libc/src/quad32.c`（`__divdi3/__udivdi3/__moddi3/__umoddi3/__muldi3`，
    纯 32 位实现，避免递归）和 `libc/src/chkstk_ms.s`（`___chkstk_ms` 栈探测），
    由 `build_sh.ps1` 通过 `$libgcc` 数组链进去。
50. **host 工具 genassym 必须是 32 位程序**（它打印的是 i386 的 offset），但
    没有 32 位 CRT/libc。做法：`-m32 -ffreestanding -c` 编译，用 `ld -m i386pe
    -e _main` 直接链，运行时只要 `printf/exit` → 用 `dlltool -m i386 -f "--32"
    -d tools/host32/msvcrt.def -l build/host32/libmsvcrt32.a` 造一个 32 位
    msvcrt 导入库；`tools/host32/hostcrt.c` 提供 `__main` 空桩。
    **注意 `-f "--32"` 不能省**，否则 dlltool 用 64 位汇编器生成 x86-64 对象。
51. **`--disable-reloc-section`**：新 binutils（2.4x）给 PE 生成基址重定位表时
    会对 8 位 fixup 报 `8-bit reloc in dll`；内核按固定地址加载，重定位表没用
    （后面还会 `objcopy -R .reloc`），所以内核链接加了这个开关。
52. **`icu.s` 要自己 include `icu.h` + `isa.h`**：4.5BSD 里这两个宏是 locore.s
    包含 icu.s 之前就该有的，工程这棵树里没有 → icu.s 顶部补
    `#include "icu.h"` / `#include "isa.h"`（已提交在文件里）。
53. **GCC 16 的位域布局会让 GDT/IDT 描述符变形（本次最大的坑）**：
    `machine/segments.h` 里 `region_descriptor`、`segment_descriptor`、
    `gate_descriptor` 必须 6/8/8 字节且字段紧排，否则：
    - `region_descriptor` 8 字节 → `lgdt` 读到的 base 变成 `0x8b000000`
      （base 被放在偏移 4，lgdt 从偏移 2 取），第一次 `mov %eax,%ds` 就 #GP；
    - `segment_descriptor` 12 字节 → `union descriptor` 变 12 字节 →
      `ssdtosd()` 按 12 字节步长写 GDT，硬件按 8 字节读，全表错位。
    修复：三个结构体都加 `__attribute__((packed))`，并在
    `build_kernel.ps1` 的 cflags 里保持 `-mno-ms-bitfields`。
    自检办法（应全部通过）：
    ```
    _Static_assert(sizeof(struct region_descriptor) == 6, "rd");
    _Static_assert(sizeof(struct segment_descriptor) == 8, "sd");
    _Static_assert(sizeof(union descriptor) == 8, "ud");
    _Static_assert(sizeof(struct gate_descriptor) == 8, "gd");
    ```
54. **4.5BSD 的 sys 树已经合并进 `kernel/`**（2026-11-14）：以前 `kernel/include/sys`、
    `kernel/include/machine`、`kernel/arch/i386/i386`、`kernel/vm` 只有
    `config.mk`，4.5BSD 头文件/arch 源要临时从 `src\BSD45` 拷（`tools/populate-src.ps1`）。
    现在这些文件全部在 `kernel/` 里随仓库走；`/bin/sh` 需要的 4.5BSD 用户态
    源码（`bin/sh`、`lib/libc`、`usr.bin/printf`、`include`、`sys/sys`、
    `sys/i386/include`、`sys/net`、`sys/hp300/include`）在 `libc/arch/bsd/`。
    原来的 `bsd45/src/`（151 MB 整棵解包树）已删除，`ensure-src` / `reorganize`
    这些一次性脚本也一并移除；`tools/populate-src.ps1` 留着当“重新解包时怎么
    归位”的参考（不再被构建调用）。
55. **`/test` 必须单独编**：`build_all.ps1` 里 `build_sh.ps1 -Target test`
    那一步漏了 S5/S6 会全 FAIL（`/test` 不存在），端到端跑一遍
    `build_all.ps1` 再跑验收。
56. **回收来的页必须清零再交给用户**（S10.1 踩的坑）：`page_alloc()` 现在会
    从 free list 复用页，但 `break()`（`sbrk`）映射堆页时原来**没有 zero_page**。
    复用到的页可能还是上一任的页表（里面全是 PTE），于是 sh 的 malloc 把
    `0x2c0067` 当成链表指针 → `TRAP 12 (stack fault)`，`echo /bin/*` 必崩，
    而单独跑 `echo /bin/*` 又不崩（要先 fork/exec 过才会有页被回收）。
    结论：**任何 `page_alloc()` 之后、交给用户之前都要 `zero_page()`**
    （`load_elf`/`map_stack`/`build_args`/`break()` 都已做）。
57. **释放用户地址空间要在切 CR3 之后**：`proc_exit` 里先
    `movl IdlePTD, %cr3`（**物理地址**，不是 `kernel_pmap->pm_pdir` 那个虚拟
    地址），再 `free_address_space()`；内核栈 PDE（0x3f6..0x3fa）是共享的，
    切换后栈仍然有效。反过来先切再读 `cur->pd` 之外的东西就会踩空。
58. **vnode 池用 LIFO 复用 + 不要重复 put**：`vn_put` 到 0 才入 free list，
    入队前检查 `vnode_used[i]`，避免同一个 vnode 被两次 put 两次入队（那会
    让两个活跃 vnode 指向同一槽位）。当前实现回到“线性扫描找空槽”也能满足
    S10.1，但 LIFO 版本更接近真实 LRU 缓存。
59. **VMware 串口管道别用阻塞读**：`serial0.fileType = "pipe"` +
    `NamedPipeClientStream.Read()` 会因为 VMware 的同步句柄把 PowerShell 挂死
    （`BeginRead` 反复挂起也救不了）。要在 VMware 里做无键盘验证，最省事的
    是编译一个带 `-DS10_AUTOTEST` 的内核（`mon.c` 里 `monitor()` 打印 banner
    后直接调 `cmd_vstress()`），`serial0.fileType = "file"` 抓输出即可。

60. **Windows 上用 MSYS2 的 xorriso 打 ISO 要转 POSIX 路径**：`usr/bin/xorriso`
    是 MSYS2 程序，`-o C:\...\out.iso` 会被当成 `<cwd>/C:\...\out.iso`，报
    `Cannot determine attributes of source file ... : No such file or directory`。
    `build_iso.ps1` 现在按 `$MKISO -match '\\usr\\bin\\'` 判断，是则把 ISO 目录和
    `-o` 都转成 `/c/...`；VMware 的原生 `mkisofs.exe` 仍用 Windows 路径。

Windows 侧 2026-11-14 的实测结果：`build_all.ps1` 约 103 秒；
`tools/test_s3|s4|proc|tty|s7|s8|s9.ps1` **7 套全 PASS**；
S10.1 落地后 `tools/test_s10.ps1` 也 PASS（8 套全绿）。
VMware Workstation 17 里同一份 ISO 启动到 `bsd45>`，`vstress` 输出
`vnode used 0→0`、`pages alloc 100 / freed 100 / inuse 0`。

## 开发约定（2026-11-14 起）

- **`bsd45/libexec/`**：所有用户态程序源码（带 `main()` 的工具：`ls/cat/echo/mkdir/rm/cp/mv/wc/grep/kill/sleep`，
  以及 `hello.c`/`init.c`/`sh.c`/`t_libc.c` 这类独立程序）。
- **`bsd45/libc/src/`**：只放 libc 依赖实现（`libc_sys.c`、`dtoa.c`、`ctype_table.c`、
  `errlst.c`、`quad32.c`、`histedit_stub.c`、`crt0.s`、`jmp.s`、`chkstk_ms.s`）和
  `hostgen/`（构建期 host 工具：`mknodes.c`/`hostshim.h`/`hoststub.c`）。
- 新增工具时：源码放 `libexec/`，在 `build_sh.ps1`/`build_sh.sh` 的工具列表里加名字，
  并在 `build_iso.ps1`/`build_iso.sh` 里把产物拷成 `BIN/<NAME>.ELF`。
## S10 任务（下次把仓库拉下来就从这里开始）

目标：把"能跑"变成"像个系统"。四个方向按依赖顺序做，每步都要能过现有 7 套
验收 + 新增验收。

### S10.1 页 / vnode 回收与缓存 ✅（2026-11-14 完成）

原状（存档）：`struct vnode` 池固定（96 个），`vn_put` 只减引用不回收；物理页
`page_alloc()` 只分配不释放；tmpfs 文件数据页永不归还；反复
`open/close/exec/cat` 后池子耗尽。

**实现**（`kernel/other/usermode.c`、`kernel/fs/vfs.c`、`kernel/other/mon.c`）：

1. **物理页回收**：`page_alloc()` 先弹 LIFO free list，空了才推进 bump 指针；
   新增 `page_free(pa)`（空闲页首字存 next 指针，经 `SCRATCH_PAGE` 访问）、
   `vfs_page_unmap(va)`（清 `kernel_pmap` 的 PTE + `tlbflush`）和计数器
   `page_nalloc/page_nfree/page_ninuse`。
2. **地址空间回收**：`proc_exit()` 先切 CR3 到 `IdlePTD`，再
   `free_address_space(pd)` —— 逐个用户页 → 页表页 → 页目录页全部归还
   （只扫 PDE 0..0x3f5，内核高半区不动）。
3. **vnode 缓存**：`vnode_pool[96]` + LIFO free list；`vn_put` 到 0 才入队
   （带重复 put 保护）；`vfs_stats()` 暴露 vnode/页计数。
4. **tmpfs 页回收**：数据页改成**首次写才分配**（`tmpfs_getdata`）；
   `truncate`/`remove`/`rmdir` 调 `tmpfs_putdata()` 先解映射再归还；
   `>` 覆盖时旧页真正回到池子里。
5. **监控命令**：`vstat`（vnode/pages 计数）、`vstress`（内核里跑
   100× ISO 读 + 100× mkdir/rmdir + 100× 覆写，前后各打一次 vstat）。
6. **`/test` 增加 S10 段**：用户态 50× open/read/close、50× mkdir/rmdir、
   50× 覆写同一文件，全部 `bad=0`。

**验收**：`tools/test_s10.ps1` / `test_s10.sh`：

```
vstat[before]: vnode total 96 used 0 free 96
vstat[before]: pages alloc 0 freed 0 inuse 0
vstress: iso read 100x ok=100
vstress: tmpfs mkdir/rmdir 100x ok=100
vstress: tmpfs rewrite 100x ok=100
vstat[after]: vnode total 96 used 0 free 96
vstat[after]: pages alloc 100 freed 100 inuse 0
s10: iso open/read/close 50x bad=0
s10: tmpfs mkdir/rmdir 50x bad=0
s10: tmpfs rewrite 50x bad=0
[user exit 0]
```

判据：vnode `used` 回到基线、页 `inuse` 回到基线、`alloc > 0 且 freed > 0`
（证明真的回收过），且无 TRAP。QEMU 与 VMware 双平台都通过。

### S10.2 信号投递 + 作业控制 ✅（2026-11-14 完成）

**实现**（`kernel/other/usermode.c`、`libc/src/libc_sys.c`、`libexec/kill.c`、`libexec/sleep.c`）：

1. **进程结构**：`p_sig`/`p_sigmask`/`p_sigignore`/`p_sigcatch`/`p_sighand[NSIG]`、
   `p_pgrp`、`p_stopsig`、`p_termsig`、`p_cont`、`wait_options`；新增状态 `P_STOPPED`。
2. **投递点**：`syscall_dispatch()` 返回前对 `cur` 调 `sig_deliver()`；`proc_sched()`
   在 `resume_user()` 之前先切到目标进程的 CR3 再投递。处理函数用“用户栈上的
   sigframe + 8 字节 trampoline（`movl $103,%eax; int $0x80; hlt`）”实现，
   `sigreturn(103)` 校验 `SIGFRAME_MAGIC` 后恢复寄存器/栈/掩码。
3. **系统调用**：`kill(37)`（pid>0 / 0=pgrp / -1 / <-1 pgrp）、`sigaction(46)`、
   `sigprocmask(48)`、`sigreturn(103)`、`getpgrp(81)`、`setpgid(82)`、
   `TIOCGPGRP/TIOCSPGRP`（控制台前台进程组）。
4. **默认动作**：终止（`WIFSIGNALED`，`p_termsig` 记录信号）、
   `SIGSTOP/SIGTSTP/TTIN/TTOU` 停止（`P_STOPPED` + `WUNTRACED` 上报
   `((sig<<8)|0x7f)`）、`SIGCHLD/SIGCONT/URG/WINCH/INFO` 忽略；
   `wait4` 支持 `WNOHANG`/`WUNTRACED` 与 `WIFSIGNALED/WIFSTOPPED`。
5. **控制台**：`^C/^Z/^\` 分别生成 `SIGINT/SIGTSTP/SIGQUIT` 发给
   `console_fg_pgrp`，`read` 返回 `-EINTR`（`st=130` 由此而来）。
6. **用户态**：libc 增加 `signal/sigaction/sigprocmask/raise`；新增 `/bin/kill`
   （`-sig`、`-l`）与 `/bin/sleep`（无时钟，忙等 `getpid()` 近似）。

**验收**：`tools/test_sig.ps1` / `test_sig.sh`：

```
sig: handler ran / sig: handler count=1     <- 处理函数 + sigreturn
sig: ignore ok                              <- SIG_IGN
sig: blocked count=0 / unblocked count=1    <- sigprocmask
sig: term signaled=1 sig=15                 <- 默认动作 + WIFSIGNALED
sig: ignore-child exited=1 code=7
shell: Ctrl-C on /bin/cat -> st=130
shell: "sleep 5 &" + jobs -> [1] 2  sleep 5 ; kill -l 列出信号
```

**已知限制（留给 S10.2 续 / S11）**：`jobs` 能列后台任务、`/bin/kill` 能发信号，
但 4.5BSD sh 只有 `bg/fg/jobs` 内建、**没有 kill 内建**，所以 `kill %1` 这种
作业名写法要改成 `kill <pid>`；真正的 `fg`/`bg` 还要求 sh 进入监视模式
（`set -m` + `tcsetpgrp` + `SIGTTIN` 停止后台读终端），协作式内核目前做不到
后台任务读终端时的正确抢占（后台 `cat` 会和 shell 抢输入）。


现状：没有 `kill(2)`、没有信号；`proc_sched` 是协作式；`wait4` 只有
`WIFEXITED`。

要做：

1. **进程结构**：`struct uproc` 增加 `p_sig`（待投递位图）、`p_sigmask`、
   `p_sigignore`、`p_sigcatch[NSIG]`、`p_pgrp`、`p_session`、`p_flag`
   （`SSTOP`/`SWTED`）。
2. **投递点**：`resume_user` 返回用户态之前检查 `p_sig & ~p_sigmask`；
   有则把 `ucontext`（寄存器 + 栈）压到用户栈，PC 设为 handler，
   `%eax` 设为 sig；handler `ret` 到 trampoline 调 `sigreturn`。
3. **系统调用**：`kill(2)`、`signal(3)`（`sigaction` 简化版）、
   `sigprocmask`、`sigreturn`、`wait4` 支持 `WIFSTOPPED`/`WIFSIGNALED`。
4. **终端**：`pccon.c` 的 `^C`/`^Z`/`^\\` 生成 `SIGINT`/`SIGTSTP`/`SIGQUIT`
   给前台进程组；`tcsetpgrp` 可后做。
5. **sh**：4.4BSD sh 的 `jobs`/`fg`/`bg`/`kill` 已编译进来（`jobs.c`），
   接上 `kill(2)`、`wait4` STOP 状态后应可直接用。
6. **验收**：`tools/test_sig.sh`：`sleep 100 &`、`jobs`、`kill %1`、
   `cat` 后 Ctrl-C、`echo $?` 130、`fg` 恢复。

### S10.3 `/bin/rm`、`/bin/cp` 等工具

现状：VFS 有 `create/remove/mkdir/rmdir/truncate`，但 syscall 层缺
`unlink`/`rename`/`link`；用户态只有 echo/ls/cat/mkdir/printf/test。

要做：

1. **syscall**：`unlink(2)`、`rmdir(2)`、`rename(2)`、`link(2)`、`chmod(2)`
   （后两个可先返回 `ENOSYS`），全部走 `vnodeops`。
2. **工具**：`libc/src/rm.c`（`unlink`/`rmdir` + `-r` 递归用 `opendir`）、
   `cp.c`（`open(O_CREAT|O_TRUNC)` + 读循环）、`mv.c`（`rename` 失败再
   `cp`+`unlink`）、`wc.c`、`grep.c`（简单子串）。
3. **ISO 只读**：`rm`/`cp` 写到 `/tmp` 要能工作；对 ISO 上文件 `unlink`
   返回 `EROFS`。
4. **验收**：`cp /readme.txt /tmp/r; cat /tmp/r; rm /tmp/r; mkdir /tmp/x;
   rmdir /tmp/x; rm /nonexistent`（报错但不崩）。

### S10.4 真正的 mount 表与 `chdir`/相对路径

现状：`/tmp` 在 `vfs_init` 里硬编码挂 tmpfs；路径必须是绝对路径。

要做：

1. **mount 表**：`struct mount { vnode *m_root; char *m_path; mount *m_next; }`，
   `vfs_mount(path, fs)`/`vfs_unmount(path)`；`/` 与 `/tmp` 走同一套。
2. **cwd**：`struct uproc` 增 `p_cwd`（vnode 引用）；`chdir(2)`/`getcwd(2)`；
   `namei` 支持相对路径（起点 `p_cwd`）、`..` 到根钳制、`//` 折叠。
3. **sh**：`cd`、`pwd`、相对路径重定向（`> a`）随之可用；`execve` 的
   路径解析同样走 `namei`。
4. **验收**：`cd /tmp; pwd; echo hi > a; cat a; cd ..; pwd;
   cat readme.txt`（相对）、`cd /nope` 报错。


### S10.4 mount 表 + chdir/相对路径 ✅（2026-11-14 完成）

**实现**（`kernel/fs/vfs.c`、`kernel/other/usermode.c`、`libexec/*`）：

1. **路径绝对化只在系统调用层做一次**：新增 `vfs_absolutize(cwd, in, out, len)`
   （有界、单遍、处理 `.`/`..`、去重斜杠、去尾斜杠），`usermode.c` 里
   `path_arg()` 把 `open/stat/access/unlink/mkdir/rmdir/rename/chdir/execve`
   的路径参数统一转换。`vfs_namei()` / `vfs_lookup_parent()` 保持原样，
   只吃绝对路径 —— 因此 tmpfs 的 `dv == &tmp_root` 身份判断不受影响。
2. **cwd**：`struct uproc` 增加 `p_cwd[128]`，`proc_alloc` 置 `/`，
   `fork/vfork` 继承；`chdir(12)` 用 `vfs_namei` 校验目录后写入，
   `getcwd(326)` 拷出（`ERANGE`/`ENOTDIR` 已定义）。
3. `echo hi > a`、`ls`（`opendir(".")`）、`cd ..` 全部走相对路径。

**踩的坑（都是 S10.4 里的真 bug）**：

- 最初把绝对化塞进 `vfs_namei()`/`vfs_lookup_parent()` 内部 → 路径被二次改写，
  `dv == &tmp_root` 失效，**S8/S10.3 全红**（已回退，改为只在 syscall 层做）。
- `vfs_lookup_parent()` 用 `slash - path` 而 `slash` 指向绝对化缓冲 → 指针跨缓冲
  相减，`dir` 变垃圾（相对路径创建必 ENOENT）。
- `vfs_absolutize()` 先补 `/` 再给组件补 `/` → 生成 `"/tmp//a"`，
  `lookup_parent` 取到 `dir="/tmp/"`，`vfs_namei("/tmp/")` 空组件 ENOENT
  → 改成「按需补斜杠」+ 去尾斜杠。
- `fork` 没继承 `p_cwd` → 子进程 `ls`/`cat` 的 cwd 丢回 `/`。

**验收**：`tools/test_s10_4.ps1` / `test_s10_4.sh`：
`cd /tmp; pwd` → `/tmp`；`echo vfs-rel > a; cat a` → `vfs-rel`；
`cd /bin; ls` → `sh.elf`；无 TRAP。11 套验收全绿。

### S11（下一步，未开工）`/dev/null`、`/dev/zero`、`/dev/tty`

shell 脚本天天用 `> /dev/null`，先做这三个字符设备。不需要真 devfs，用特殊 fd 最省：

1. `kernel/other/usermode.c` 的 fd 类型（第 109-114 行现有 `FD_FILE 0`…
   `FD_CONSOLE 5`）后面加：
   ```c
   #define	FD_NULL	6
   #define	FD_ZERO	7
   #define	FD_TTY	8
   ```
2. `open()`（`case 5`，约第 1901 行）在 `vfs_namei` 之前判路径：
   `/dev/null` / `/dev/zero` / `/dev/tty` → 找一个空 fd 槽（照 `fd_alloc_vnode()`
   第 597 行的写法，`vp = 0`），设 `type`/`rw` 后直接返回，不走 VFS。
3. `read()`（`case 3`，控制台分支约第 1827 行 `if (cur->fds[fd].type == FD_CONSOLE)`）：
   `FD_NULL` → 0；`FD_ZERO` → `bzero(buf, len)` 返回 len；`FD_TTY` → 和控制台同路。
4. `write()`（`case 4`）：`FD_NULL` / `FD_ZERO` → 直接返回 `len`（丢弃）。
5. `fstat`：这三种返回 `0020666`（`S_IFCHR|0666`）。
6. 验收 `tools/test_dev.ps1` / `test_dev.sh`：
   ```
   echo hi > /dev/null ; echo st=$?     -> st=0
   cat /dev/null                        -> 无输出
   cat /dev/zero | wc                   -> 行/字/字节为 0 但 read 成功
   echo tty-ok > /dev/tty               -> 屏幕出现 tty-ok
   ```
   然后跑 `build/all_tests.sh`（12 套：S3…S10.4 + test_dev）。

**流程约定（2026-11-14）**：完成后**先只提交、不推送**，等新的仓库地址再
`git remote set-url origin <地址> && git push -u origin main`。

### S12（下一步，未开工）`/bin/df` + `mount`：把挂载表暴露给用户态

复用 S11 的「特殊 fd」套路，不新增系统调用号：

1. `kernel/fs/vfs.c` 加
   ```c
   int vfs_mount_list(char *buf, unsigned len);   /* 文本，NUL 结尾 */
   ```
   用现有的 mount 表逐项格式化，例如：
   ```
   /        iso9660  lba=24  size=2048  ro
   /tmp     tmpfs    lba=0   size=0     rw
   ```
   （ISO 的 `iso_root()/iso_root_size()` 已有；tmpfs 报 0。）
2. `kernel/other/usermode.c`：
   - fd 类型加 `#define FD_MOUNTS 8`
   - `open()` 的 `/dev/*` 拦截块里加 `/dev/mounts` → `FD_MOUNTS`
   - `read()` 复用 S11 的打包方式：第一次读时调 `vfs_mount_list()` 到静态缓冲，
     之后按 `off` 返回剩余部分，读完返回 0
3. 工具（`libexec/`）：
   - `df.c`：`cat /dev/mounts` 的等价物（open/read/write 循环）
   - `mount.c`：同样读 `/dev/mounts`，只是头部打印 `device on mount-point type`
4. `build_sh.ps1/.sh` 工具列表加 `df mount`；`build_iso.ps1` 装
   `BIN/DF.ELF`、`BIN/MOUNT.ELF`
5. 验收 `tools/test_mount.ps1` / `.sh`：
   ```
   df            -> 出现 "/" 与 "/tmp" 两行
   mount         -> 出现 iso9660 / tmpfs
   cat /dev/mounts | wc   -> 行数 == 2
   ```
   然后跑 14 套（13 + test_mount）。

**约定**：完成后 commit + push，仓库地址用用户当次给的（当前是
`BSD45_libexec_fulll_userland`，新地址以用户消息为准）。

## 0.7 暂停点与恢复指令（2026-11-14 07:20，重要，先读这节）

### 当前状态（暂停时）

| 项 | 值 |
|---|---|
| 仓库 | https://github.com/moccuishle/BSD45_libexec_fulll_userland |
| 已推送 | `b51b334`（S12b：`/dev/mounts` + `df` + `mount`），14 套验收全绿 |
| 工作区 | **有未提交改动**：S13（`/dev/ps` + `/bin/ps`）已实现并手工验证通过，但 15 套验收跑到一半被中断 |
| 未提交文件 | `kernel/other/usermode.c`、`build_sh.ps1`、`build_sh.sh`、`build_iso.ps1`、`libexec/ps.c`、`tools/test_ps.ps1`、`tools/test_ps.sh` |
| 未跟踪（本地辅助，已被 .gitignore 忽略） | `build/*.sh`、`build/*.py`（bash 驱动脚本） |

S13 手工验证结果（直接跑 QEMU 得到的）：

```
# ps
PID PPID PGRP STAT
1 0 1 BLOCK
2 1 2 RUN
```

### 恢复后按顺序做这几步

```bash
# 0) 所有命令都走 bash（见 work/shell.md）
export PATH=/c/Windows/System32/WindowsPowerShell/v1.0:/usr/bin:/c/msys64/usr/bin
cd /c/Users/Administrator/Desktop/work/bsd45

# 1) 先杀掉残留 QEMU！否则它占着 out/BSD45-ELF-x86.iso，
#    build_iso 会生成截断 ISO（2.3MB），所有启动测试全 "NO VERDICT"
powershell -NoProfile -Command \
  "Get-Process qemu-system-i386 -ErrorAction SilentlyContinue | Stop-Process -Force"
sleep 2

# 2) 重建（userland + sh + test + kernel + ISO）
powershell -NoProfile -ExecutionPolicy Bypass -File build_all.ps1

# 3) 15 套验收（S3 S4 S5 S6 S7 S8 S9 S10.1 S10.2 S10.3 S10.4 S11 tools S12 S13）
bash tools/all_tests.sh        # 15 套，脚本已随仓库提交

# 4) 全绿后提交并推送
git add -A && git commit -m "S13: /dev/ps + /bin/ps" && git push origin main
```

`tools/all_tests.sh` 已包含全部 15 套（会先杀掉残留 QEMU 并检查 ISO 大小）。

### 恢复时最容易踩的三个坑（本会话都真实发生过）

1. **ISO 被占用 → 截断 → 全线 NO VERDICT**
   `rm: Device or resource busy`（QEMU 或 VMware 挂着光驱）。现象：`build_iso` 报的
   ISO 只有 ~2.3 MB 而不是 4,591,616 字节；11 套以上验收同时 "NO VERDICT"。
   处理：杀 QEMU → `rm -f out/BSD45-ELF-x86.iso` → 重跑 `build_iso.ps1`。
2. **PowerShell 吃 bash 的引号/管道**
   带 `"` `|` `$` `()` 的 bash 逻辑**必须写成 `.sh` 文件**再执行，不要用
   `bash -c '...'`（会报 `syntax error near unexpected token` 或把 `|` 当管道）。
   详见 `work/shell.md`。
3. **从 bash 给 `.ps1` 传数组**
   `-Keys "a","b"` 会被当成一个字符串。用 `-Keys "a|b|c"`（`run_qemu_test.ps1`
   已支持 `|` 分隔）。

### 本会话已完成的里程碑（都已推送覆盖）

| 阶段 | 内容 | 提交 |
|---|---|---|
| S10.4 | 挂载表 + `chdir`/`getcwd` + 相对路径 | `416b246` |
| S11 | `/dev/null`、`/dev/zero`、`/dev/tty` | `d37bfa4` |
| tools | `/bin/head`、`/bin/tail` | `01a7100` + `5b7ac0f` |
| S12 | `/dev/mounts` 伪文件 + `/bin/df` | `474d640` |
| S12b | `/bin/mount`（`mount.c` include `df.c`，看 `basename(argv[0])`） | `b51b334` |

### 下一步候选（S13 收尾之后）

1. **`/bin/uname`、`/bin/true`、`/bin/false`**（各 ~10 行，最快）
2. **`/bin/ps` 增强**：加 `comm`（进程名）、`-a`/`-x` 过滤；再给 `kill` 支持 `%job`
3. **真 `fg`/`bg`**：sh 监视模式 + `tcsetpgrp` + 后台读终端时 `SIGTTIN`（工作量最大）

## 0.8 2026-11-15 会话：Windows 补齐工具链后 15 套全绿

环境：MSYS2 里补装了 `bison flex git xorriso diffutils`
（`pacman -S --needed bison flex git xorriso diffutils`）；编译器仍是
`C:\msys64\ucrt64\bin\gcc.exe`（16.2.0，`-m32`），QEMU 11.1.3 不变。
本机没有 VMware mkisofs，ISO 由 MSYS2 的 `/usr/bin/xorriso -as mkisofs` 生成。

**改动（都已进本次提交）**

1. `build_iso.ps1`：MSYS2 的 xorriso 是 POSIX 程序，直接传 `C:\...\isodir`
   会得到 `xorriso : FAILURE : Cannot determine attributes of source file
   '/c/.../C:\...\isodir'`（它把 cwd 和 Windows 路径拼在一起）。现在检测
   `$MKISO` 是否位于 `\usr\bin\`，是则把 `-o` 与 ISO 目录都转成 `/c/...`；
   VMware 的原生 `mkisofs.exe` 仍走 Windows 路径。
2. `tools/test_mount.ps1` / `.sh`：不再断言 `iso9660 lba=24` —— ISO 根目录
   LBA 取决于打包工具（VMware mkisofs=24，xorriso=19），改成只断言
   `iso9660 lba=` + `iso9660`/`tmpfs`/`ro`/`rw` 的形状。这是测试过严，
   不是内核回归。
3. `build_iso.sh`：Linux 侧原来只拷 sh/init/hello/test/ls/mkdir/cat/echo，
   漏了 `kill sleep rm cp mv wc grep head tail df mount ps`，导致 Linux 上
   跑 `tools/all_tests.sh` 时 S10.3/S12/S13/tools 必红；现已与
   `build_iso.ps1` 的清单对齐。

**验收（Windows + xorriso）**

```
test_s3 PASSED  test_s4 PASSED  test_proc PASSED  test_tty PASSED
test_s7 PASSED  test_s8 PASSED  test_s9 PASSED    test_s10 PASSED
test_sig PASSED test_s10_3 PASSED test_s10_4 PASSED test_dev PASSED
test_tools PASSED test_mount PASSED test_ps PASSED
---- suites failed: 0
ALL SUITES PASSED
```

ISO 4,591,616 字节（与 mkisofs 产物同尺寸），内核 `bsd45.elf` 396,388 字节。

**本会话环境备注（不进仓库）**：DSH 命令沙箱会拒绝
`Start-Process -RedirectStandardOutput`，所以 `build_kernel.ps1` 里跑
`genassym` 那一步会报 `Access is denied`。本地用 `build/_bk.ps1`
（同脚本，改成 `& $ga | Out-File -Encoding ascii`）绕过；
`tools/run_qemu_test.ps1` 用的是不带重定向的 `Start-Process`，不受影响。

## 0.9 2026-11-15 S14：`/bin/uname`、`/bin/true`、`/bin/false` ✅

**实现**

1. **内核** `syscall_dispatch()` 新增 `case 164`（4.5BSD `uname(2)`）：
   先 `user_ok(buf, sizeof(struct utsname))`，再直接填
   `BSD45` / `bsd45` / `1.0` / `4.4BSD-Lite2 i386 ELF kernel` / `i386`。
   结构体用内核自带的 `kernel/include/sys/utsname.h`（五个 256 字节字段，
   共 1280 字节；不在内核栈上放副本，直接写用户缓冲）。
2. **libc** `libc_sys.c`：`#define SYS_uname 164` + `uname()` 薄封装
   （负 errno → `-1`/`errno`，与其它 stub 一致）。
3. **工具** `libexec/uname.c`（`-a/-s/-n/-r/-v/-m`，可合并如 `-sn`，默认 `-s`）、
   `libexec/true.c`、`libexec/false.c`（退出码 0/1）。
4. **构建**：三个名字加进 `build_sh.ps1` / `build_sh.sh` 的工具表，
   `build_iso.ps1` / `build_iso.sh` 装成 `BIN/{UNAME,TRUE,FALSE}.ELF`。
5. **验收**：`tools/test_tools.ps1/.sh` 从 head/tail 扩成
   head/tail/uname/true/false；退出码用 `true && echo true-ok` /
   `false || echo false-ok` 验证（`$?` 会被 PowerShell 先展开，别用）。

**验收**（QEMU 串口实录）

```
# uname
BSD45
# uname -a
BSD45 bsd45 1.0 4.4BSD-Lite2 i386 ELF kernel i386
# uname -m
i386
# uname -s
BSD45
# uname -r
1.0
# uname -v
4.4BSD-Lite2 i386 ELF kernel
# true && echo T-ok
T-ok
# false || echo F-ok
F-ok
```

`tools/all_tests.sh`：15 套全绿（`test_tools` 已含上述断言）。

## 0.10 2026-11-15 S15：`/dev/ps` 增加 `COMM`（进程名） ✅

**实现**

1. `struct uproc` 增加 `char p_comm[16]`；新增 `proc_setcomm()`，取可执行文件
   的 basename（`/bin/ps` → `ps`）；`execve` 时更新，`fork`/`vfork` 继承，
   `start_prog()`（pid 1）也设置，并顺带把 pid 1 的 `p_cwd` 补成 `/`
   （它绕过 `proc_alloc`，之前 cwd 是空串）。
2. `/dev/ps` 每行变成 `PID PPID PGRP STAT COMM`；`/bin/ps` 表头同步。
3. `tools/test_ps.ps1/.sh` 断言 `PID PPID PGRP STAT COMM`、
   `1 0 1 BLOCK sh`、`2 1 2 RUN ps`。

**验收**

```
# ps
PID PPID PGRP STAT COMM
1 0 1 BLOCK sh
2 1 2 RUN ps
```

`tools/all_tests.sh` 15 套全绿。

**已知限制**：内核还没有 per-process 的 tty/session 信息，所以 `ps -a`/`-x`
的真实过滤语义（BSD：`-a` 看有 tty 的非会话首进程、`-x` 连无 tty 的一起看）
要等作业控制/会话模型落地后再补；当前 `ps` 列出全部进程。

## 0.11 2026-11-15 S16：真实时间（CMOS RTC）+ `/bin/date` ✅

原状：`gettimeofday()` 是 libc 里写死的 `784000000`（1994-11-04），内核没有
116 号系统调用，也没有 `date`。

**实现**

1. **内核** `usermode.c`：`rtc_epoch()` 直接读 CMOS RTC（0x70 索引 / 0x71 数据，
   经 `locore.s` 的 `rtcin()`）——等状态寄存器 A 的 UIP 位清零，按状态寄存器 B
   判断 BCD/二进制与 12/24 小时，再用 `rtc_days()` 把「1970-01-01 起的天数」
   换成秒。`gettimeofday(2)`（116）填 `struct timeval`（`tv_usec` 恒 0，
   没有亚秒时钟源），`timezone` 参数清零。
2. **libc**：`gettimeofday()` 改走 116 号系统调用，不再返回假时间；
   `time(3)` 因此也变成真实时间。
3. **工具** `libexec/date.c`：这个 libc 子集没有 `gmtime/strftime`，所以内置
   Hinnant 的 days→civil 算法，输出 `Sat Nov 14 17:43:29 UTC 2026`
   （QEMU/VMware 的 RTC 就是 UTC）。
4. **构建/验收**：`date` 进工具表与 `BIN/DATE.ELF`；`test_tools` 增加
   `date` 的正则断言（`Www Mmm dd hh:mm:ss UTC yyyy`）。

**验收**

```
# date
Sat Nov 14 17:43:29 UTC 2026
# echo date-st=$?
date-st=0
```

`tools/all_tests.sh` 15 套全绿（ISO 4,595,712 字节）。

**已知限制**：没有时钟中断，`tv_usec` 恒 0；`sleep(1)` 仍是忙等假实现，
真 sleep 要等时钟中断/超时队列（与 S10.2 的限制同源）。

## 0.12 2026-11-15 Linux（Ubuntu 26.04 + VMware）构建与验收 ✅

环境：Ubuntu 26.04 LTS VM（VMware，用户 `jialiang`，`192.168.209.128`，
vmnet8 NAT）：`gcc 15.2.0 + gcc-multilib + libc6-dev-i386`、
`genisoimage 1.1.11`、`qemu-system-i386 10.2.1`（无 KVM，纯 TCG）、
`bison 3.8.2`、`flex 2.6.4`、`python3 3.14`、`git 2.53`。

送代码用 `git archive HEAD | tar -x`：**不要**直接拷 Windows 工作区，CRLF 会让
`set -o pipefail` 报 `invalid option name`，全盘皆错。

`bash build_all.sh` 暴露了 4 个只存在于 `.sh` 路径的问题（Windows 一直走
`build_*.ps1`，这些分支从没被执行过）：

1. **头文件视图**：`build_sh.sh` 拷的是 `kernel/include/{sys,machine}`，而
   `machine/endian.h` 只在 `libc/arch/bsd/sys/i386/include` 里 → 86 个文件
   `fatal error: machine/endian.h`。改成与 `build_sh.ps1` 一致：
   `usr/include`、`usr/sys/sys`、`usr/sys/i386/include → machine/`、
   `usr/sys/net`，hp300 的 ansi/limits/float 兜底；最后再把项目自己的
   `ctype.h/stdarg.h/histedit.h` 盖回去（否则 4.5BSD 的 ctype.h 会盖掉
   4.3-Reno 的 `_ctype_` 表，`_N` 未定义）。
2. **生成 sh 源码**只拷 `*.c/*.h/*.y/*.l`，漏了 `nodetypes`、`nodes.c.pat`、
   `builtins.def` → `mknodes: Can't open nodetypes`、`nodes.h` 缺失，14 个文件
   失败；改成拷全部普通文件。
3. **libkern 头**：`build_kernel.sh` 把 `libkern.h` 放 `inc/lib/`，而 4.5BSD 的
   `<sys/systm.h>` 仍 `#include <libkern/libkern.h>` → 内核 10 个文件失败；
   补 `inc/libkern/libkern.h`。
4. **可执行位**：仓库里的 `.sh` 都是 100644（Windows 提交、`core.filemode=false`），
   Linux checkout 后 `tools/run_qemu_test.sh: Permission denied`，15 套全红；
   现在索引里改成 100755。

**内核真 bug（两端都受影响，Linux 先暴露）**：`load_elf()` 假设每个 PT_LOAD 的
`p_vaddr` 页对齐。`ld -m elf_i386` 给 `.data` 的 `p_vaddr` 是 `0x080550e0`
（页内偏移 0xe0），旧代码把 `img + p_offset` 直接拷到页首，整个 `.data`
错位 0xe0 字节（症状：`/test` 里 `cr2=0x20202028` 的 TRAP 12、
`arith` 里出现空格）；PE/objcopy 生成的段恰好页对齐，所以 Windows 从没踩到。
现在按 `p_vaddr & 0xfff` 算页内偏移，从「包含 p_vaddr 的页」开始映射，
拷贝终点用 `p_offset + p_filesz`（`p_filesz` 是长度，不是文件末偏移——
第一版这里写错，把多页 text 的尾巴清零，Windows 立刻 `usermode` 就 TRAP 12）。

修完两端都全绿：

| 平台 | 构建 | ISO | 15 套验收 |
|---|---|---|---|
| Windows（ucrt64 GCC 16.2 + xorriso） | `build_all.ps1` | 4,595,712 B | **ALL SUITES PASSED** |
| Ubuntu 26.04（GCC 15.2 + genisoimage，TCG） | `build_all.sh` | 4,257,792 B | **ALL SUITES PASSED** |

## 0.13 2026-11-15 S100.1 / S100.2：边界契约与微内核设计（文档落地）

原则：**“晚分叉”不等于“晚设计”**——先在单基线里把边界固定成可检查的契约，
再谈分叉。

**新增文档（仓库根 `docs/`）**

- 当时产出两份设计文档（`docs/kernel-boundary.md`、
  `docs/microkernel-design.md`）以及 `tools/check-boundary.sh` 边界检查脚本。
  **两份文档已随微内核方向删除**；`check-boundary.sh` 保留——它校验的是
  "FS/驱动不许直接碰进程表/调度器/页分配器"，对宏内核同样有意义。

**新增工具**

- `bsd45/tools/check-boundary.sh`：把边界变成可执行检查。
  硬规则（`cur/procs/proc_*/P_BLOCKED/page_alloc/page_free/zero_page/user_map/
  pmap_enter/pmap_remove` 出现在 `kernel/fs/*.c`、`kernel/driver/*.c`）有 1 处
  就 exit 1；层次债（`atapi_*`、`pmap_map`）只打印不阻断。
  当前输出：`hard rules: OK (0 violations)`、`known layering debt: 8`。

**下一步（S100.2a）**：把 tmpfs 从 `kernel/fs/vfs.c` 原样搬到
`kernel/fs/tmpfs.c`（纯搬移、`vnodeops` 不变），跑 15 套确认 0 行为变化，
之后再加 `-Variant` 开关。

## 0.14 2026-11-15 S100.2a：tmpfs 从 `vfs.c` 拆出（纯搬移）✅

**改动**

1. 新增 `kernel/fs/tmpfs.c`（352 行）：tmpfs 节点表、数据页惰性分配 /
   `pmap_map` 内核窗口、11 个 vnodeops 实现，全部从 `vfs.c` 原样搬出；对外只
   暴露 `tmpfs_init()` / `tmpfs_root()` / `tmpfs_is_root()` 和
   `tmpfs_vnodeops`。
2. `kernel/fs/vfs.c` 804 → 500 行：只剩 vnode 池、`namei()`、挂载表、
   `vfs_absolutize/rename/mount_list` 和 ISO9660 vnodeops（ISO 后端计划在
   S100.4 再搬）；顺手删掉已经没人用的 `extern int user_ok()` 声明。
3. `vn_alloc()` 由 `static` 改为公开（backend 需要为 lookup 结果分配 vnode），
   在 `vfs.h` 声明；`vfs.h` 同时声明 `tmpfs_*` 三个入口。
4. `vfs_init()` 不再直接初始化 `tmp_root`，改为 `tmpfs_init()` +
   `mount_add("/tmp", tmpfs_root())`；`namei()` 的 `/tmp` 特例和 `vn_put()`
   的「挂载根不回收」判断都改走 `tmpfs_is_root()`，`vfs.c` 里不再出现
   `tmp_root`。
5. 构建：`build_kernel.ps1/.sh` 的源文件表加 `tmpfs.c`。

**验收**：Windows 15 套全绿（按用户要求本轮只验 Windows，不跑 Ubuntu）。
`tools/check-boundary.sh`：硬规则 0 违规，层次债 7 条
（isofs 5 + tmpfs 的 `pmap_map` 1 + vfs 的 `atapi_read` 1）。

**下一步**：加 `-Variant mono|micro` 构建开关（先只认 `mono`，零行为变化），
然后 S100.3 落 KP-4/6/7 + `fs_server`。

## 0.15 2026-11-15 4.5BSD VFS/VM 缓存层盘点与路线（「真正的 cache」前置调查）

> 2026-11-16 更新：本节的**路线 B 已全部落地**，实现与验收见 **0.16**；
> 设计细节见 [`docs/cache-mmap.md`](../docs/cache-mmap.md)。原来的仓库根
> `恢复.md` 已合并进 `RESUME-TASK.md`（本文件 + 仓库根入口），不再单独维护。

结论：你列的 VFS/VM 文件**大多存在，但不能直接“加进构建”**。

**现状**

| 文件 | 真实状态 |
|---|---|
| `kernel/sys/vfs_bio.c` | **函数体被删空**：`bread()` 只剩 `/* Body deleted. */ return (EIO);`（vfs_bio.c:142-154），`bwrite/getblk/brelse/biodone` 同样；只有 `bufinit` 和 hash/队列管理是真的 |
| `kernel/sys/vfs_cache.c` | 完整 4.5BSD 实现，但用 `(dvp)->v_id`（:75）——我们的 `struct vnode` 没有 `v_id` |
| `vfs_subr.c`(1782) / `vfs_lookup.c`(645) / `vfs_vnops.c`(449) / `vfs_syscalls.c`(2417) / `vfs_cluster.c`(756) | 4.5BSD 完整源码，但编译不了：依赖 `ucred`/`mount`/`uio`/`VOP_*`/`vnode_if.h`/`sleep-wakeup`/`M_*` malloc/`bdevsw->d_strategy` |
| `vm_page.c`(710) / `vm_object.c`(1449) / `vm_fault.c`(1037) / `vm_pageout.c`(571) / `vnode_pager.c`(582) | 完整 4.5BSD 源码，依赖 4.5BSD 的 `vm_map`/`proc`/pager 注册/`vm_page` 队列；我们的内核只有 `pmap`，没有 VM 对象层 |
| `kernel/fs/ufs/ufs/`、`kernel/fs/ufs/ffs/` | **只有 `config.mk`，没有任何 .c/.h** |
| `kernel/sys/config.mk` | 这些文件名只是注释里的 “sources in this directory”，`obj-y` 全空，构建从未编译它们 |

当前构建只编 19 个目标（locore/pmap/machdep/cons/lib/usermode/embed/pccon/stubs/
glue/mon/atapi/isofs/tmpfs/vfs），4.5BSD 的 VFS/VM 一个都没进。

**为什么不能“加进构建就行”**：这些文件冲着 4.5BSD 内核核心写（`struct buf`、
带 `v_id` 的 `vnode`、`mount`、`ucred`、`uio`、`VOP_*`、`vnode_if.h`、`sleep/wakeup`、
`M_*` malloc、`bdevsw/strategy`）；我们的端口有意重写成了最小核心（自定义
`struct uproc`、协作式调度、`vfs.c` + `vnodeops`、同步 `atapi_read`）。

**两条路线**

- **路线 A：真·移植 4.5BSD 内核核心**——恢复 `vfs_bio.c` 删除体、生成 `vnode_if`、
  带起 `init_main/kern_synch/kern_malloc/kern_descrip/kern_exec/fork/sig`、
  `vfs_init/vfs_conf`、`bdevsw/strategy`、`ucred/mount`，替换 `usermode.c` 的自定义
  syscall 层；还要补 ATA 硬盘驱动（现在只有只读 CD），否则 FFS 无设备可挂。
  代价：长期不可启动，15 套暂时用不上。
- **路线 B：按 4.5BSD 职责划分，把真正的缓存算法写进当前端口**（推荐先做）：
  B1 `vfs_bio.c` buffer cache（hash + LRU + 脏页 + hit/miss）+ `vfs_cluster.c` 预读；
  B2 `vfs_cache.c` name cache（给 vnode 加 `v_id`）；B3 `vm_object/vm_page/vnode_pager`
  文件页缓存；B4 UFS/FFS 暂缓（缺驱动、缺源码）。

**建议**：先做 B1——让 ISO9660 的 `read`/`readdir` 走真正的 buffer cache，
验收为 17 套全绿 + `cat` 同一文件第二遍的 `atapi_read` 次数显著下降；
之后再 B2、B3。算法以后可原样搬进路线 A 的 4.5BSD 结构，不会白做。

## 0.16 2026-11-16 S17：缓存系统移植（B1+B2+B3）✅

**目标**：把 4.5BSD 的缓存职责（buffer cache → name cache → 文件页缓存/mmap）
按路线 B 写进当前端口，验收为 17 套全绿（原 15 套 + test_cache + test_mmap）。

**新增代码**

| 文件 | 作用 |
|---|---|
| `kernel/fs/vfs_bio.c` | 2K 块 buffer cache：hash(32)+LRU、`bread/breada/brelse/bwrite/bdwrite`、hit/miss/read/readahead 统计 |
| `kernel/fs/vfs_cache.c` | name cache：`(dv->v_id,name)->vnode`，64 项/32 桶，`v_id` 校验、create/remove/rename purge |
| `kernel/fs/pagecache.c` | 4K 文件页缓存（= vm_object+vm_page+vnode_pager）：128 页、hash+LRU+引用计数、`readraw` 填充、`evicts` 统计 |
| `libexec/vmstat.c` | `/bin/vmstat`：cat `/dev/vmstat` |
| `libexec/mmaptest.c` | mmap 三用例验收程序 |
| `tools/test_cache.{ps1,sh}` | 两遍 cat 的缓存命中/零读盘断言 |
| `tools/test_mmap.{ps1,sh}` | file/anon/shared+msync 断言 |

**改动**

1. `struct vnode` 增加 `v_id`（唯一、释放清零）与 `v_key`（只读后端的稳定页缓存键）；
   `vnodeops` 末尾增加 `readraw`（页缓存填充用的无缓存读）。
2. `vfs_namei()` 先查 name cache；`vfs_create/rename` 与新增的 `vfs_remove()`
   （unlink/rmdir 统一走它）负责 purge。
3. **目录 vnode 保留**（8 槽，只保留只读 ISO 目录）：`root->bin->file` 的目录项
   缓存可跨 exec 命中；普通文件与 tmpfs 目录仍随 close/rmdir 释放，S10.1 的
   vnode 基线不变。
4. ISO 的 `read()` = `pagecache_read()`，其 miss 调 `iso_readraw_op()` →
   buffer cache；目录扫描/PVD/readdir 全部改走 buffer cache。
5. tmpfs 的 `read()` 也走页缓存（键用 `v_id`），`write/truncate` 时 purge。
6. `usermode.c` 新增 `mmap(197)/munmap(73)/mprotect(74)/msync(65)`：
   立即填充、`MAP_PRIVATE` 复制、`MAP_SHARED` 映射页缓存页、
   `mmap_teardown()` 在 exit/exec 前解除共享 PTE 并放引用。
7. `/dev/vmstat` + `vfs_cache_report()`；monitor `vstat` 追加同样的缓存行。
8. 构建：`build_kernel.ps1/.sh` 加 3 个源文件；`build_sh.ps1/.sh` 与
   `build_iso.ps1/.sh` 加 `vmstat mmaptest`；`tools/all_tests.sh` 15 → 17 套。

**验收（Windows，QEMU）**

```
test_s3 ... test_ps       15 套全部 PASSED
test_cache  S17 caches: PASSED   (buf hits 413->507, disk reads 182->182,
                                  page hits 2->45, fills 89->89, name hits 4)
test_mmap   S17 mmap: PASSED     (file ok / anon ok / shared ok / mmap: ok)
---- suites failed: 0
ALL SUITES PASSED
```

**关键数字**：第二次 `cat /readme.txt` 期间 `buf reads` 完全不变（182→182），
`page fills` 不变（89→89），`page hits` 增加 43 —— 证明第二次完全由内存服务。

**踩坑**（详见 `../docs/cache-mmap.md` 第 9 节）：

1. 页缓存 LRU 若只收空闲项，128 页填满后 `pc_take()` 永远失败 → `exec` 报 EIO；
   现在 `refs==0` 的项（无论是否 used）都在 LRU 上。
2. `read()` 必须在 EOF 按 `st_size` 钳长度，否则 `cat` 会在零填充页上死循环。
3. ISO 页缓存键不能用 `v_id`（每次 open 新 vnode），必须用 LBA。
4. 共享映射页不能交给 `free_address_space()`，否则双重释放。
5. `/dev/vmstat` 不能照抄 `/dev/ps` 的静态快照，计数器要每次 read 重新渲染。
6. tmpfs 目录不能进目录 vnode 保留环：`mkdir/rmdir` 压力循环会把 8 个槽占满，
   S10.1 的 vnode 基线就会涨；只保留只读 ISO 目录。

**当前边界**：仍无 demand paging/COW、`mmap` 只支持 offset 0；B4（UFS/FFS）
按 0.15 继续暂缓（缺 ATA 硬盘驱动和 UFS 源码）。

## S100（已放弃，2026-11-28）微内核与宏内核的边界

> 这一节原先规划 S100.1–S100.4：边界盘点、设计对照、tmpfs 服务化实验与
> 宏/微度量。S400–S403 真的落地了三个用户态服务（`fs_server` /
> `devfs_server` / `netsrv`），随后在 2026-11-28 全部撤销：用户态服务、
> IPC 转发层、客户端库、测试、两个占位目录与四份设计文档一并删除，
> 主线只保留宏内核。唯一保留的成果是 S100.2a（tmpfs 拆成独立文件，
> 见 0.14）。撤销的原因与完整清单见仓库根 `RESUME-TASK.md` §6.2.18。

**不要再往这个方向开工。**

## 1. 任务目标（原始需求）

在 4.4BSD-Lite2 i386 ELF 内核上实现 Solaris 式的 VFS/root 文件系统、
`init(8)` 与 `execve`，把 4.4BSD libc/sh 交叉编译成 i386 ELF 用户程序放进
ISO，最终 ISO 启动后进入用户态运行 BSD 的 `/bin/sh`。

硬约束：
- 用 mingw32 **i386** GCC 编译；
- 产物是 UNIX ELF32（不是 PE），ISO 用 El Torito 引导；
- 不使用 SIMD（`-march=i386 -mno-sse -mno-mmx`，用户态同样适用）。

## 2. 完成度

| 阶段 | 内容 | 状态 |
|---|---|---|
| S0 | 用户态 ELF 交叉编译链（COFF→PE→objcopy） | ✅ |
| S1 | ring3、用户页目录/栈、ELF32 装载、`int 0x80` | ✅ |
| S2 | ramfs、fd 表、`execve`（含 argv/envp）、`/sbin/init` → `/bin/sh` | ✅ |
| S3 | 4.4BSD libc 子集 + **真正的 BSD `/bin/sh`** | ✅ 本次完成 |
| S4 | ATAPI(PIO) + ISO9660 只读 root；`/sbin/init`、`/bin/sh` 是光盘文件 | ✅ 本次完成（核心） |
| S5 | fork/vfork/wait4、进程表与调度、`/bin/echo` 外部命令 | ✅ 本次完成 |
| S6 | 控制台 tty：termios ioctl、行规程、`isatty` 为真、sh 交互提示符 | ✅ 本次完成 |
| S7 | 目录遍历（`getdirentries`/`opendir`/`readdir`、`ls`、通配符）+ pipe 管道 | ✅ 本次完成 |
| S8 | 内存 tmpfs：`/tmp` 可写、`>`/`>>` 重定向、`mkdir`/`ls /tmp` | ✅ 本次完成 |
| S9 | vnode/VFS：`vnodeops` + ISO9660/tmpfs 后端，syscall/exec 全部走 vnode | ✅ 本次完成 |

### S3 验收（`powershell -File tools\test_s3.ps1`）

```
bsd45> usermode
exec /sbin/init: entry 8048000 pd 200000
init: starting /bin/sh
execve /bin/sh: entry 8048000 esp bfbfffb8 pd 20e000 argc 3
hello
[user exit 3]
```

交互模式（monitor 输入 `shell`）：

```
bsd45> shell
execve /bin/sh: entry 8048000 esp bfbfffd8 pd 20e000 argc 1
echo hi there
hi there
echo $((2+3))
5
exit 7
[user exit 7]
```

即：ISO 里的 `/bin/sh` 已经是 **4.4BSD-Lite2 `usr/src/bin/sh`** 编译出来的
i386 ELF（175 KB），不是项目早期的最小内置 sh；`-c`、交互、`echo`、
算术展开 `$(( ))`、退出码传递都跑通。


### S4 验收（`powershell -File tools\test_s4.ps1`）

```
atapi: QEMU     QEMU DVD-ROM
atapi: capacity 2332 sectors
root: ISO9660 on ATAPI CD, root dir lba 24, 2048 bytes

bsd45> cd
ISO9660 root: lba 24, 2048 bytes
bsd45> usermode
exec /sbin/init: entry 8048000 pd 200000
init: starting /bin/sh
execve /bin/sh: from ISO9660 (175504 bytes, lba 353)
execve /bin/sh: entry 8048000 esp bfbfffb8 pd 239000 argc 3
hello
[user exit 3]
```

关键点：`/bin/sh` 不再来自内核数组，而是 **ISO9660 光盘上的 `/BIN/SH.ELF`**；
`execve` 里打印的 `from ISO9660 (… lba 353)` 就是证据。VMware 里同样通过
（`NECVMWar VMware IDE CDR10`）。

### S4 实现要点

- `kernel/atapi.c`：两条 IDE 通道 × 主/从 探测 ATAPI（IDENTIFY PACKET +
  word0 判定 CD-ROM），用 SCSI READ(12) 走 PIO 读 2048 字节扇区；
- `kernel/isofs.c`：读 LBA 16 的 PVD，校验 `CD001`，从 root 目录记录
  逐扇区走目录项；路径按“大写 + 无扩展名补 `.ELF`”映射
  （`/bin/sh` → `BIN/SH.ELF`，`/sbin/init` → `SBIN/INIT.ELF`）；
- `usermode.c`：`execve` 优先从 ISO9660 读文件（读到 `CD_IMAGE_VA 0xFE400000`
  的临时内核映射，按页 `pmap_map`），失败才回退到内嵌 ramfs；
- `build_iso.ps1`：把 `sh.elf/init.elf/hello.elf/test.elf` 放进 ISO 的
  `BIN/`、`SBIN/` 和根目录；内核里仍保留一份 ramfs 作为无盘启动的兜底；
- monitor 新增 `cd` 命令查看挂载信息。


### S5 验收（`powershell -File tools\test_proc.ps1`）

```
bsd45> test
exec /test: from ISO9660 (70588 bytes)
S:fork
C:fork
fork waited pid=2 code=5
S:vfork
execve /bin/echo: from ISO9660 (70412 bytes, lba 239)
vfork-exec-ok
P:vfork
vfork waited pid=3 code=0
done
[user exit 0]
```

sh 里跑外部命令（fork → exec → wait4 全部走通，`$?` 正确）：

```
bsd45> shell
execve /bin/sh: from ISO9660 (175504 bytes, lba 274)
/bin/echo one
execve /bin/echo: from ISO9660 (70412 bytes, lba 239)
one
/bin/echo two
two
echo st=$?
st=0
exit 0
[user exit 0]
```

### S5 实现要点

- `struct uproc` 进程表（8 槽）：pid/ppid、页目录、`brk` 水位、fd 表、
  保存的 `sysregs` 上下文、退出码、等待状态；
- `copy_address_space()`：新建 PD/PT，逐页复制用户地址空间（内核高半区共享）；
- `resume_user(pd, sysregs *)`（`usermode_asm.s`）：换 CR3、恢复通用寄存器、
  `iret` 回 ring3。系统调用栈是协程式复用的（每次 `int 0x80` 从 TSS esp0
  重新开始），不用保存内核栈；
- `fork` 复制地址空间、子进程 `eax=0`；`wait4` 若无僵尸子进程则阻塞父进程，
  子进程 `exit` 时写回状态并唤醒父进程（`reaped` 标记：被唤醒的父进程直接
  收走僵尸）；`vfork` = 复制地址空间 + 父进程阻塞到子进程 exec/exit；
- `getpid/getppid/kill` 按进程表返回；`stat/access` 现在也会查 ISO9660，
  这样 sh 的 `PATH` 查找能找到 `/bin/echo`。


### S6 验收（`powershell -File tools\test_tty.ps1`）

```
bsd45> test
tty: isatty=1/1/1 tcgetattr=0
tty: iflag=300 oflag=3 cflag=af80 lflag=58a
...
bsd45> shell
# echo hi-tty
hi-tty
# exit 0
[user exit 0]
```

VMware（`NECVMWar VMware IDE CDR10`）里同样能进 `# ` 交互，说明 tty 与
ring3 入口都不依赖 QEMU。

即：`isatty(0/1/2)` 全为真、`tcgetattr()` 返回规范模式的 termios
（`ICANON|ECHO|ISIG|IEXTEN`），交互 `/bin/sh` 也终于打出 `# ` 提示符。

### S6 实现要点

- 内核 `ioctl(54)`：`TIOCGETA/TIOCSETA/TIOCSETAW/TIOCSETAF`（fd 0..2）、
  `TIOCGWINSZ`（80x25）、`TIOCGPGRP/TIOCSPGRP`，其余仍返回 ENOTTY；
- 常量直接来自 4.4BSD 的 `sys/ttycom.h` / `sys/termios.h`（termios 44 字节）；
- 控制台行规程 `console_read()`：回显、退格、`^U` 删行、`^W` 删词、
  `^D` 空行 EOF、`^C` 丢弃当前行；
- libc 的 `tcgetattr/tcsetattr` 改走 `ioctl(TIOCGETA/TIOCSETA)`，于是
  4.4BSD 的 `isatty()`（其实现就是 tcgetattr 成功）返回真；
- `init` 在非 `-c` 模式下给 sh 加 `-i`，sh 才进入交互模式并打印 PS1（`# `）。


### S7 验收（`powershell -File tools\test_s7.ps1`）

```
bsd45> shell
# ls /
bin
boot.cat
boot.img
bsd45.elf
hello.elf
readme.txt
sbin
test.elf
# ls /bin
cat.elf
echo.elf
ls.elf
sh.elf
# echo /bin/*
/bin/cat.elf /bin/echo.elf /bin/ls.elf /bin/sh.elf
# echo hello-pipe | cat
hello-pipe
# exit 0
[user exit 0]
```

### S7 实现要点

- ISO9660 目录枚举：`iso_readdir()` 逐条读目录记录（记录不跨扇区），名字转小写、
  去掉 `;1` 版本号和尾部点；`iso_stat()` 现在能返回目录本身（`/`、`/bin`）；
- 内核 `getdirentries(fd, buf, n, basep)`：fd 表新增 `FD_DIR`，每次返回一条
  `struct dirent`（`d_type` 目录=4/文件=8），`lseek` 对目录改的是记录游标；
- `stat/fstat/access` 对目录返回 `S_IFDIR`（0755），`ls` 因此能判断参数是目录；
- libc 补齐 4.4BSD 的 `opendir/readdir/closedir/telldir/seekdir/rewinddir`、
  `glob/fnmatch`、`alphasort/mergesort` 与 `fstatfs` 桩；
- 新增 `/bin/ls`（自己的小实现）和 `/bin/cat`（管道的读者）；
- pipe：8 个 4 KB 环形缓冲，read/write 在空/满时挂起进程（和 wait4 同一套
  “保存 sysregs、由对端唤醒”的机制，唤醒时借用对端页目录把数据直接拷进
  用户缓冲），`close`/退出时维护读写端引用计数并唤醒对端；
- fd 0/1/2 的 `dup2`/`fcntl(F_DUPFD)` 现在优先查 fd 表，才落到控制台。


### S8 验收（`powershell -File tools\test_s8.ps1`）

```
bsd45> shell
# echo hello-tmpfs > /tmp/a
# cat /tmp/a
hello-tmpfs
# echo second >> /tmp/a
# cat /tmp/a
hello-tmpfs
second
# mkdir /tmp/d
# ls /tmp
a
d
# exit 0
[user exit 0]
```

### S8 实现要点

- 内核里一块**内存文件系统** `/tmp`：32 个槽、每文件一页（4 KB）、名字 1 层；
  每页用 `pmap_map` 映射到内核窗口 `0xFE600000`（物理地址不能当虚拟地址用）；
- `open` 支持 `O_CREAT/O_TRUNC/O_APPEND/O_RDONLY/O_WRONLY`，`read/write/lseek`
  走 tmpfs 节点，`stat/fstat` 报文件和目录，`unlink/mkdir/rmdir` 直接改表；
- `getdirentries` 增加 tmpfs 目录分支，`ls /tmp` 能列出来；
- **fd 0/1/2 正式登记为 `FD_CONSOLE`**：以前 read/write 直接特判，导致
  `fcntl(1, F_DUPFD, 10)`（sh 的 `savefd`）返回 EBADF，重定向静默失效；
- 新增 `/bin/mkdir`；`sh` 的 `>`/`>>` 现在走 open + fcntl + dup2。


### S9 验收（`powershell -File tools\test_s9.ps1`）

```
bsd45> shell
# ls /                 <- ISO9660 目录，经 vnode readdir
# cat /readme.txt      <- ISO9660 文件，经 vnode read
# echo vfs-ok > /tmp/a <- tmpfs，经 vnode create/write
# cat /tmp/a
vfs-ok
# mkdir /tmp/d
# ls /tmp
a
d
# echo hi-vfs | cat    <- 管道仍然工作
hi-vfs
```

### S9 实现要点

- 新增 `kernel/vfs.c` + `vfs.h`：`struct vnode` + `struct vnodeops`
  （lookup/getattr/read/write/readdir/create/remove/mkdir/rmdir/truncate）；
- 两个后端：`iso_vnodeops`（(lba,size) 存在 vnode 里，按扇区读）和
  `tmpfs_vnodeops`（一页一个文件）；`/tmp` 是挂在 ISO 根上的挂载点，
  `vfs_namei()` 里切换；
- 所有路径类系统调用（open/stat/fstat/access/unlink/mkdir/rmdir/
  getdirentries/lseek）以及 `execve`/`init` 装载都改成走 vnode，
  syscall 层不再判断“这是 ISO 还是 tmpfs”；
- fd 表里放 `struct vnode *vp`，`close`/`dup`/`dup2`/`fcntl(F_DUPFD)`/
  `fork` 都要维护 vnode 引用计数（96 个 vnode 的静态池）；
- 旧的 ramfs 直查被移除，`/sbin/init`、`/bin/sh` 从光盘经 VFS 装载。

## 3. S3 做了什么

### 3.1 内核（`kernel/usermode.c`）

- `execve(path, argv, envp)` 现在把 argv/envp **复制到新用户栈**：
  `argc, argv[], NULL, envp[], NULL, 字符串`，ESP 指向 `argc`；
- `brk`/`sbrk` 真正的堆：按页映射，`brk_mapped` 是**页对齐**的映射水位
  （早期版本用它未对齐的地址当起点，会重复映射同一页，malloc 立刻 #PF）；
- `struct stat` 按 4.4BSD i386 布局（96 字节）填充，`st_size` 在偏移 48
  （`off_t` 是 64 位）；早期按 128 字节乱写会踩坏调用者的栈；
- syscall 表补齐：`read/write/open/close/execve/exit/break/getpid/getppid/
  uid-gid/getpgrp/setpgid/access/signal 类/ioctl(ENOTTY)/umask/fcntl/
  stat(189)/fstat(188)/lseek(199)/getdtablesize/wait4(ECHILD)/dup2/
  mprotect/munmap/msync`；错误统一返回**负 errno**；
- 新增 `/test` ramfs 项与 `start_prog(path, script)`，monitor 里 `test`
  直接跑 libc 冒烟测试程序。

### 3.2 用户态 libc 子集（`userland/libc/`）

| 文件 | 作用 |
|---|---|
| `crt0.s` | `_start`：取 argc/argv/envp，设 `environ`，调 `main`，`exit` |
| `jmp.s` | `setjmp/longjmp`（i386，`jmp_buf` 同 4.4BSD 布局） |
| `libc_sys.c` | 全部 `int 0x80` syscall stub + errno + 无 tty/无信号/无目录的降级实现 |
| `dtoa.c` | 给 4.4BSD `vfprintf` 用的最小 `__dtoa`（sh 不打浮点） |
| `ctype.h` + `ctype_table.c` | 4.3BSD-Reno 的经典 `_ctype_` 表（4.4BSD 的 ctype 依赖 rune/locale，太重） |
| `histedit.h` + `histedit_stub.c` | 4.4BSD sh 强制链接 libedit；本内核无 termios，`el` 保持 NULL，退回 `read(2)` |
| `errlst.c` | 4.4BSD `sys_errlist`（`sys_nerr` 去掉 const 以匹配头文件） |

4.4BSD 源码里直接编的子集：`string/`（含 bcopy→memcpy/memmove、
memset→bzero、index→strchr 的多重定义编法）、`stdlib/`（malloc/calloc/
exit/atexit/getenv/qsort/strtol/rand/abort/getopt）、`stdio/`（整个
FILE/缓冲/printf 家族）、`gen/`（isatty/setmode/wait3/exec）。

`build_sh.ps1` 负责：生成 `token.def`/`builtins.c,h`/`nodes.c,h`/
`syntax.c,h`/`init.c`（4.4BSD 的 mk* 主机工具，用 `-include hostshim.h`
补 `__P`、补 `link()`），bison/flex 生成 `arith.c`/`arith_lex.c`，
最后 `ld -m i386pe -Ttext 0x08048000` + `objcopy -O elf32-i386`（外加
`libgcc.a` 提供 `__divdi3` 等）。

## 4. 已知限制（S4 及以后）

- 无 `fork/vfork/wait4`：**外部命令不能执行**，只有内建命令（`echo`,
  `cd` 退化, `exit`, `$(( ))` 等）可用；`vfork` 返回 ENOSYS；
- 无 tty：`isatty` 恒假、无行编辑/作业控制/`fc` 历史；
- 无目录：`opendir/readdir` 返回 NULL，`chdir` 恒 ENOENT，`getcwd` 恒 `/`；
- `ramfs` 仍是内核数组，ISO9660 里没有 `/bin/sh` 文件；
- 信号全为 no-op（`signal/sigaction` 返回 0），`kill` 返回 0。

## 5. 构建 / 运行 / 测试

```powershell
cd C:\Users\Administrator\Desktop\work\bsd45
$env:QEMU='C:\msys64\ucrt64\bin\qemu-system-i386.exe'
$env:MKISOFS='C:\Program Files (x86)\VMware\VMware Workstation\mkisofs.exe'

powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test_s3.ps1
```

`build_all.ps1` = 小用户态（hello/init）→ `build_sh.ps1`（真 sh 覆盖
`out\userland\sh.elf`）→ 内核 → ISO。加 `-NoSh` 可退回内置小 sh。

手动 QEMU：

```powershell
& "C:\msys64\ucrt64\bin\qemu-system-i386.exe" `
  -cdrom out\BSD45-ELF-x86.iso -boot d -m 64 -serial stdio
```

启动后在 monitor 里：
- `usermode`：`init -c 'echo hello; exit 3'` → sh -c 验收路径；
- `shell`：交互 `/bin/sh`；
- `test`：libc 冒烟测试（printf/malloc/string）。

`tools/run_qemu_test.ps1` 是 QEMU 驱动（TCP 串口，`server=on,wait=on`，
必须 wait=on 才能收到启动输出）。`tools/test_kbd.ps1` 是**纯键盘**回归
（`-serial none`，模拟 VMware 这种没有串口的机器）。

### VMware

打开 `vms\BSD45-ELF-x86.vmx`（cdrom 指向 `..\out\BSD45-ELF-x86.iso`）。
启动后**先点一下 VM 窗口**再敲键盘（VMware 要捕获输入）。启动行会打印
`Console: VGA 80x25, COM1 present` 或 `COM1 absent`——没有串口的 VM 显示
absent 是正常的。

> 旧版 ISO 在 VMware 里键盘没反应：VMware 对未映射的 I/O 端口返回 `0xff`，
> 而 `pccngetc()` 先读 COM1 的 LSR，`0xff & 1 == 1` 被当成"串口有数据"，
> 于是永远返回 `0xff`、从不轮询键盘。现在 `com_probe()` 会识别这种情况
> 并跳过 COM1，另外 `kbd_init()` 会清空 8042 输出缓冲并发送 `0xae`
> 使能键盘接口。想抓串口日志可在 vmx 里加：
>
> ```
> serial0.present = "TRUE"
> serial0.fileType = "file"
> serial0.fileName = "bsd45-serial.log"
> ```

## 6. 已踩过并修复的关键坑（别重犯）

1. `pmap_map()` 没有 pmap 参数：`pmap_map(virt, start, end, prot)`。
2. `locore.s` 的 `calltrap` 要 `pushl %esp` 给 `trap()` 传 trapframe 指针。
3. IDT 要 `idt[256]` 才放得下 `int 0x80`。
4. 用户 PTE 用 `PG_u`(0x4)，不是 `PG_U`(0x20)。
5. 用户页目录必须复制 PDE `0x3F6`（proc0 内核栈）。
6. ISO 末尾内核补零到 2048 对齐。
7. stage1 用 CD 的 2048 字节扇区计数。
8. 解包用 `System32\tar.exe`（MSYS2 GNU tar 处理 Windows `-C` 路径有坑）。
9. `$ErrorActionPreference` 在 build_iso 必须是 Continue。
10. GCC16 下 `region_descriptor` 6 字节 packed、`segment_descriptor` 8 字节。
11. **用户态也必须 `-march=i386 -mno-sse ...`**：默认 `-march=pentium4` 会生成
    `movd %eax,%xmm0`，内核没开 SSE，一进 ring3 就异常。
12. **PE 汇编符号带下划线**：`_start` 里 `call _start_c`；且 `static` 函数被
    内联掉会导致 asm 引用找不到符号（去掉 static 或加 `used`）。
13. **COFF 不支持 `.size`** 指令（ELF 才有）。
14. **`brk` 必须按页对齐推进**，否则同一页被反复映射，malloc 写新页 #PF。
15. **`struct stat` 是 96 字节不是 128**，越界写会踩坏调用者栈（症状：
    `__swhatbuf` 里写 NULL 指针）。
16. 4.4BSD `signal.h` 把 `sigemptyset/sigfillset/...` 定义成宏，libc 内部要
    真函数时必须先 `#undef`（且放在所有 `#include` 之后）。
17. 4.4BSD `arith.y` 用 `= {` 标动作、`yyerror` 里用 `yyerrok`，bison 不认，
    生成前要 sed 掉。
18. 复制 sh 源码时源树里的 `obj` 坏符号链接会让 `Copy-Item` 管道中断，
    要过滤 ReparsePoint。
19. `Expand-Archive` 解 Reno 源码会因非法文件名失败，用 `7za x`。
20. **VMware 无串口时键盘失灵**：未映射 I/O 端口读回 `0xff`，`pccngetc()`
    把 `0xff` 当串口数据，永远不读键盘。修复：`com_probe()` 判断 LSR 的
    `0xe0` 位（`0xff` 或非 `0x60` 视为无串口）+ `kbd_init()` 清 8042 缓冲并
    发 `0xae`；启动时打印 `Console: ... COM1 present/absent` 便于确认。

21. **ATAPI 的 PACKET 命令必须设置 byte count limit**（LBA mid/high，
    小端）。填 0 时 VMware 的 CDR10 会把 READ(10)/READ(12) 直接 ABRT
    （st 0x51 err 0x04），而 INQUIRY / READ CAPACITY 却正常，极具迷惑性。
    正确做法：按本次数据相长度填 `bytes & 0xff` / `(bytes >> 8) & 0xff`。
22. ATAPI 探测不要迷信 LBA mid/high 的 0x14/0xeb 签名（QEMU 读回 0），
    用 IDENTIFY PACKET 返回的 word0（bit15=1 且设备类型 5 = CD-ROM）判定。
23. 读 CD 的缓冲区不要放进内核静态数组：会撑大 ELF，stage1 把内核读到
    0x10000、stage2 在 0x90000，>512 KB 的内核会被覆盖。改用运行时
    `pmap_map` 的临时窗口。

24. **`user_map()` 必须显式接收页目录**：它内部用 `SCRATCH_PD` 编辑页表，
    fork/exec 之后 `SCRATCH_PD` 指向的可能已经是子进程的 PD，父进程 `brk`
    会把页表改到别处（症状：父进程堆页缺失、`#PF`）。
25. **内核镜像必须 < 512 KB**：stage1 把内核读到 `0x10000`，stage2 放在
    `0x90000`；内核超过 512 KB 会覆盖自己的尾部（内嵌镜像数组），表现为
    用户程序某几页是内核代码。S5 起 sh/echo/test 只放在 ISO9660 上，
    内核只嵌 `hello/init`，镜像回到 ~334 KB。
26. **僵尸进程要回收**：父进程阻塞在 `wait4` 时由子进程 `exit` 唤醒，这条
    路径必须把子进程标成 FREE（`reaped`），否则下一次 `wait4` 会拿到已经
    报过到旧 pid。
27. **vfork 子进程不要碰栈**：严格共享地址空间时，子进程自己的函数调用会
    踩父进程的栈帧（我们的测试程序就踩过）。本实现改为“复制 + 父进程阻塞”，
    是 vfork 的安全超集。

28. **tty 的 ioctl 号不要手写**：`TIOCGETA` 等是用 `_IOR/_IOW` 按
    `sizeof(struct termios)` 算出来的，termios 在 4.4BSD 里是 44 字节
    （含 `c_ispeed/c_ospeed`）；直接 include `sys/ttycom.h` 最稳。
29. **`isatty()` 就是 `tcgetattr()` 成功**：libc 的 isatty 只做这一件事，
    所以 tty 支持只要把 tcgetattr 接到 `TIOCGETA` 上即可。
30. **sh 的提示符要 `-i`**：4.4BSD sh 不会因为 stdin 是 tty 就自动进入
    交互模式（要 `iflag`），init 里显式传 `-i` 才会打印 PS1。

31. **切 CR3 之后不要再读内核数据**：`enter_user` 原来先 `mov cr3` 再
    `pushl __ucodesel`（从内存取选择子），VMware 下新页目录对那页的映射
    与 QEMU 不同，直接 #GP（被 4.4BSD 的向量表报成 TRAP 9）。现在改成
    **先压好整个 ring3 返回帧、最后再切 CR3 + iret**；`resume_user` 同理，
    它的 iret 帧全部从栈上取，不读内核全局。

32. **fd 0/1/2 不能写死成控制台**：`read` 里 `if (fd == 0)` 直接走
    控制台，会让 `dup2(pipefd, 0)` 失效。要先查 fd 表，只有未 dup 时才落
    到控制台。
33. **sh 的重定向用的是 `fcntl(F_DUPFD)`**（`copyfd()`），不是 `dup2`；
    fcntl 不实现，`|` 看起来“没报错但没接线”。
34. **进程退出必须关闭 fd**：管道写端引用计数不减，读者就永远等不到 EOF，
    表现为 `[no runnable process]` 死锁。
35. **`iso_name()` 会给无点名字补 `.ELF`**：`/bin` 这种目录名要先按
    `BIN.ELF` 找、失败再按 `BIN` 找，否则目录永远打不开。

36. **VMware 对 `resume_user` 的段寄存器顺序敏感**：先把 DS/ES/FS/GS
    设成用户选择子（DPL=3），再做栈上的 `push`，VMware 会报 TRAP 9 并停机
    （QEMU 不报）。改成**先压好 iret 帧和所有寄存器、最后才设段寄存器并切
    CR3** 就正常；`enter_user` 早已是这个顺序，`resume_user` 这次也统一了。

37. **物理页不能直接当虚拟地址用**：`page_alloc()` 返回的是物理地址，
    tmpfs 一开始直接 `bzero(pa, 4096)` 立刻 `#PF`；要用 `pmap_map` 映射到
    内核窗口（`0xFE600000`）再访问。
38. **fd 0/1/2 要在 fd 表里**：只做 `if (fd == 0)` 特判时，sh 的
    `fcntl(1, F_DUPFD, 10)` 会因为 `fds[1].used == 0` 返回 EBADF，`>`
    重定向不报错但不生效。给 0/1/2 建 `FD_CONSOLE` 条目即可。
39. **`start_prog` 也要初始化 console fd**：它 `bzero(procs)` 后手工填
    pid 1，绕过了 `proc_alloc`，第一版 S8 因此让 init/sh 的 printf 和
    read(0) 全失败。

40. **fd 关掉时要 `vn_put()`**：VFS 化之后 fd 持有 vnode 引用，最初
    `close()` 只清 `used`，vnode 池（96 个）很快耗尽，`ls /tmp` 直接
    打不开目录；`dup/dup2/fcntl/fork` 复制 fd 时也要 `vp->ref++`。
41. **tmpfs 根 vnode 没有 node**：`node = -1`，`getattr` 直接索引
    `tmpfs_nodes[-1]` 会把 `/tmp` 报成普通文件，`opendir` 因此失败
    （症状：`ls /tmp` 打印路径本身）。每个 op 要先判根节点。
42. **ISO 名字还是大写 + `.ELF`**：`/sbin/init` 在盘上是 `SBIN/INIT.ELF`，
    VFS 的 lookup 要在精确匹配失败后对无点名字再试一次 `name + ".elf"`。
43. **`execve` 也要走 vnode**：直接 `iso_lookup()+atapi_read()` 会让
    VFS 变成“只给 syscall 用的装饰”；现在 exec/init 都通过
    `vnodeops->read` 把镜像读进内核窗口。

## 8. 提交与恢复

- 每完成一个子任务：`bash tools/test_s3.sh ... test_s9.sh` 全绿 + 新增
  `test_s10*.sh`，然后 `git add -A && git commit`，推到
  `git@github.com:moccuishle/BSD45_rootfs_overlay.git`（main）。
- 恢复方式：clone 仓库 → 按第 0 节构建 → 从文档开头的 S10 节开始。
- 构建产物不进 git（`bsd45/build/`、`bsd45/out/` 已忽略）。
- 源码全部在仓库里：`bsd45/kernel/`（4.5BSD sys 树）+ `bsd45/libc/arch/bsd/`
  （4.5BSD 用户态源码）。`bsd45/src/` 已合并删除，构建不依赖任何 tarball
  （已实测：`src` 不存在时 `build_all.ps1` 104 秒 + 7 套验收全绿）。
- 引导代码按架构分目录：`boot/i386/`（stage1、stage2、kernel.ld 在用）、
  `boot/{arm,amd64,mips,riscv}/` 是占位目录（各带 `.keep`）。构建脚本里
  的路径已统一指向 `boot/i386/`。
- 整包备份/迁移可以用上层目录的 `4.5BSD-jialiang.tar.gz`（它包含完整的
  `bsd45/`），但**日常构建与恢复都不需要它**。

---

## 2026-11-16 决策

先做内核，外部的api 以后再做也可以。
