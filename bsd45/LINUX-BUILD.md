# Linux / Bash 构建

`bsd45/*.sh` 是 `*.ps1` 的等价实现，功能一致：用户态 → 4.5BSD `/bin/sh` 与全部
工具 → 内核 → El Torito ISO（含 `BSD45.JLFS` 安装镜像）。已在真实 Ubuntu
（VMware 虚拟机）上完整构建并启动验证。

## 环境

```bash
sudo apt install build-essential gcc-multilib libc6-dev-i386 binutils \
                 genisoimage byacc flex python3
# 可选：在 Linux 上走与 Windows 完全相同的 PE 路线
sudo apt install gcc-mingw-w64-i686 binutils-mingw-w64-i686
# 可选：自己启动验证
sudo apt install qemu-system-x86
```

| 项目 | 验证过的版本 |
|---|---|
| 发行版 | Ubuntu 26.04 LTS Server (VMware, NAT) |
| gcc | 15.2.0（含 gcc-multilib） |
| binutils / ld | 2.46 |
| byacc | 2.0 20241231 |
| flex | 2.6.4 |
| genisoimage | 1.1.11 |
| python3 | 3.x（只有 mingw 路线用得到 `tools/pe_bss_fix.py`） |

## 构建

```bash
cd bsd45
bash build_all.sh                 # userland -> /bin/sh + tools -> /test -> kernel -> ISO
bash build_all.sh --no-sh         # 保留 libexec/sh.c 那个极简 shell
bash build_all.sh -D S10_AUTOTEST -u   # 等价于 ps1 的 -Define / -Unicode

bash build_kernel.sh              # 单独构建内核（--no-link / --only usermode.o）
bash build_sh.sh                  # /bin/sh + 全部 libexec 工具
bash build_sh.sh -t test          # /test
bash build_iso.sh                 # 打包 out/BSD45-ELF-x86.iso
bash verify.sh                    # QEMU 启动 + banner 检查
```

编译期开关用环境变量：`ARCH`、`KERNEL_UNICODE=1`、`KERNEL_DEFINE=A,B`、
`BSD45_TOOLCHAIN=native|mingw`。

产物：`out/BSD45-ELF-x86.iso`（16,939,008 字节）、`build/bsd45.elf`
（160,080 字节）、`out/userland/*.elf`。

## 两套工具链，同一份产物

`tools-find.sh` 探测并统一导出 `CC/LD/OBJCOPY/OBJDUMP/HOSTCC/LEADUS/CROSSBIN`，
其余脚本不再直接写 `gcc`/`ld`：

| 路线 | 编译 | 链接 | 适用 |
|---|---|---|---|
| `native`（Linux 默认） | `gcc -m32` 直接出 ELF32 | `ld -m elf_i386 -T boot/i386/kernel.ld` | 装 gcc-multilib 的 Linux |
| `mingw` | `gcc -m32` 出 COFF | `ld -m i386pe` 出 PE，`objcopy -O elf32-i386` 转 ELF | Windows/MSYS2，或装了 mingw-w64 的 Linux |

默认 `auto`：先用一个真实的 `gcc -m32` 探针（必须产出 `elf32-i386` 且 `ld -m
elf_i386` 能链），成功走 `native`，否则退到 `mingw`。两条路线产出的 ELF 布局相同
（`.text` 0xFE101000），`stage2` 按 `PT_LOAD` 搬运，所以 ISO 都能直接启动。

`native` 路线内核更小（160 KB vs 204 KB）：把零初始化静态数组放进 `.bss` 是 ELF
链接器的默认行为，PE 路线需要 `tools/pe_bss_fix.py` 显式改写 `.data$*` 才等效。

## 为 Linux 做的适配（只改 `.sh`，`.ps1` 不变）

1. **符号下划线**：Windows/mingw 会给 C 符号自动加 `_`，ELF 不会，而 4.4BSD
   的汇编（`locore.s`、`icu.s`）和本项目的 `usermode_asm.s`、`crt0.s` 都按
   `_name` 引用。`native` 路线统一加 `-fleading-underscore`（`LEADUS`），
   `mingw` 路线留空 —— 两条路线的汇编符号就都是 `_name`。
2. **入口符号**：`libc/src/hello.c` 的入口由 `_start` 改名 `start`
   （加下划线后正好是 `_start`，两种平台一致）。
3. **64 位除法**：`native` 加 `-fleading-underscore` 后 gcc 引用 `___divdi3`，
   而 libgcc 只提供 `__divdi3`；`build_sh.sh` 改为链接 4.4BSD `kernel/lib` 的
   quad 例程（`divdi3.c`/`qdivrem.c` 等 19 个文件）。`mingw` 路线沿用 ps1 的
   `libc/src/quad32.c` + `chkstk_ms.s`。
4. **genassym / 宿主生成器**：`genassym`、`mknodes`、`mksyntax`、`mkinit` 用
   `HOSTCC` 编译（本机可执行的程序），但 `genassym` 必须带 `-m32`——它输出的
   是目标机的结构体偏移。mingw 路线上这就是 ps1 里 dlltool + msvcrt32 那套
   自举；用 i686 工具链时直接链 32 位运行时，不再需要。
5. **串口测试**：`tools/run_qemu_test.sh` 原用 `|` 分隔按键，与 shell 管道
   冲突；现支持 `\|` 转义（测试脚本里的 `echo hi \| cat` 即真实管道）。
6. **串口读取**：后台读取器改用 `stdbuf -o0`，避免最后一行 `[user exit N]`
   滞留在缓冲区。

## yacc / lex

`/bin/sh` 的解析器由 `arith.y` / `arith_lex.l` 生成。优先级：

1. `tools/hostbin/{yacc,flex}` —— 由 `tools/build_bsdtools.sh` 从
   `4.4BSD-Lite2.tar.gz` 的 `usr/src/usr.bin/yacc` + `contrib/flex-2.5.2` 重建，
   与 Windows 侧 `tools/build_bsdtools.ps1` 装到同一目录；
2. 发行版的 `byacc` + `flex`（byacc 与第 1 项同源，行为一致）；
3. 都没有才退回 `bison -y`，并按 ps1 的注释改写语法（`= {` → `{`，删掉
   `yyerrok`/`yyclearin`）。

用 byacc 就不需要那两处改写：`= {` 是它的动作共享语法，`yyerrok`/`yyclearin`
在它里面是全局宏。flex 2.5.2 的 `-o` 必须紧贴文件名（`-oarith_lex.c`）。

## 验证结果（Ubuntu 26.04，QEMU 8 vCPU / 3 GB）

- `bash build_all.sh`：0 个编译失败，产出 ISO 16,939,008 字节
- QEMU 启动该 ISO：`smpon` 4 核上线、`shell` 正常、`ls /bin` 列出全部工具
- `pthread`：`created 4 threads`、`counter=4000 join=ok`、`detach=5000` → **PASS**
- `pthmain`（S425 回归）：main `pthread_exit` 后两个 worker 都跑完 → **PASS**
- `pthpre`：`ABABAB…` 抢占交错 → **PASS**
- `Triple fault` 0、`kernel TRAP` 0、`address space busy` 0

## 与 Windows 版的关系

Windows 上仍用 `*.ps1`（`gcc -m32` COFF → `ld -m i386pe` PE →
`objcopy -O elf32-i386`）；Linux 上默认 `gcc -m32` 直接产出 ELF，`build_*.sh` 用
`ld -m elf_i386` + `boot/kernel.ld` 链接。设 `BSD45_TOOLCHAIN=mingw` 可以强制
Bash 走 PE 路线，用来逐字节对照 Windows 的产物。
