/*
 * smp.c - i386 SMP support: per-CPU state, spinlocks, Local APIC, MP detection.
 *
 * This is phase 1 of SMP: the data structures and low-level primitives.
 * AP startup (INIT/SIPI trampoline) and per-CPU scheduling follow once this
 * layer is stable.
 */
#include <sys/param.h>
#include <sys/systm.h>

#include <machine/pcb.h>	/* S413: struct i386tss for the per-CPU TSS */

#include "sys/smp.h"
#include "sys/platform.h"
#include "thread/kthread.h"
#include "arch/i386/io.h"

extern void pmap_map(unsigned virt, unsigned start, unsigned end, int prot);
#define	VM_PROT_READ	0x1
#define	VM_PROT_WRITE	0x2
#define	LAPIC_VA	0xFEA00000u	/* free 4KB window: FE8 buf, FE9 pagecache */

/* EN: CPUID helper is used for topology; CMOS guesses were removed (S412). */

struct cpu_info cpus[MAXCPU];
int ncpu = 1;			/* BSP is CPU 0; grows as APs come up */
static int ap_tramp_installed = 0;	/* set once trampoline is copied+patched */

/*
 * S432: cpu_id() moved here from sys/smp.h, where it was a static __inline
 * that executed `str` directly.  asmble.md forbids assembly outside arch/,
 * so the instruction now lives in the i386 tree and the generic header only
 * declares the function.  Callers are unchanged.
 *
 * `str` has a 16-bit destination, so the upper half of the register GCC
 * picks is undefined afterwards.  Mask explicitly: relying on the compiler
 * to zero-extend gave an AP a garbage CPU id now and then (it then wrote the
 * BSP's TSS esp0 and both CPUs shared a kernel stack).
 * S432：cpu_id() 从 sys/smp.h 移到本文件（原先是内联 `str`）。asmble.md 禁止
 * arch/ 之外出现汇编，因此指令归 i386 目录，通用头只留声明，调用点不变。
 * `str` 是 16 位目标，寄存器高 16 位未定义，必须显式掩码；否则偶尔会算出
 * 垃圾 CPU 号（曾导致 AP 改写 BSP 的 TSS esp0，两核共用栈）。
 */
int
cpu_id(void)
{
	unsigned sel = 0;

	__asm __volatile("str %0" : "=r" (sel));
	sel &= 0xffffu;
	sel >>= 3;
	if (sel < CPU_TSS_SEL_BASE || sel >= CPU_TSS_SEL_BASE + MAXCPU)
		return (0);
	return ((int)(sel - CPU_TSS_SEL_BASE));
}

/* S412: IPI gates.  setidt() lives in machdep.c (K&R definition). */
extern void setidt(int idx, void *func, int typ, int dpl);
extern void smp_ipi_tlb(void), smp_ipi_resched(void), smp_ipi_stop(void);
static int smp_idt_done;

static void
smp_idt_init(void)
{

	if (smp_idt_done)
		return;
	setidt(IPI_TLB_SHOOTDOWN, (void *)smp_ipi_tlb, 14, 0);
	setidt(IPI_RESCHEDULE, (void *)smp_ipi_resched, 14, 0);
	setidt(IPI_STOP, (void *)smp_ipi_stop, 14, 0);
	smp_idt_done = 1;
}

/*
 * S412: AP stacks and the embedded raw trampoline.  The trampoline is
 * linked at physical 0x7000 (not at KERNBASE+...), because an AP starts in
 * real mode where absolute addresses are physical.  build_kernel.ps1 emits
 * ap_tramp_bin[]; the data block lives at offset 0x300 in that blob.
 */
#define	AP_TRAMP_DATA_OFF	0x300
#define	AP_TRAMP_GDTR		0x300	/* limit16 + base32 */
#define	AP_TRAMP_PD		0x306
#define	AP_TRAMP_STACK		0x30a
#define	AP_TRAMP_CPUID		0x30e
#define	AP_TRAMP_ENTRY		0x312
#define	AP_TRAMP_WIN		0xFEB00000u	/* writable alias of phys 0x7000 */

extern const unsigned char ap_tramp_bin[];
extern const unsigned int ap_tramp_bin_len;
extern int IdlePTD;

/* 8 CPUs x 16 KB, in .bss (VA is valid in every page directory). */
static unsigned char ap_stacks[MAXCPU][16384] __attribute__((aligned(16)));

/*
 * S413: one TSS per CPU.
 *
 * CPU 0's TSS has always lived at the start of proc0's u-area (that is what
 * the GPROC0_SEL descriptor points at, and what init386 fills in), so it is
 * registered here rather than duplicated.  An AP gets its own TSS in this
 * file, installed into GDT slot GPROC0_SEL + id before its first ltr.  The
 * hardware requires every CPU to have a TSS because that is where esp0 for
 * ring-3 -> ring-0 transitions comes from; sharing one TSS between CPUs means
 * an AP's syscall lands on the BSP's kernel stack.
 * S413：每个 CPU 一个 TSS。CPU0 的 TSS 一直在 proc0 u 区的开头（GPROC0_SEL
 * 描述符指向那里，init386 填的就是它），所以这里只登记不复制；AP 用自己的
 * TSS，在第一次 ltr 之前装进 GDT 的 GPROC0_SEL+id 槽。硬件要求每个 CPU 都有
 * TSS——ring3 进 ring0 的 esp0 就来自它；多核共用一个 TSS 会让 AP 的系统调用
 * 落在 BSP 的内核栈上。
 */
static struct i386tss	ap_tss[MAXCPU] __attribute__((aligned(16)));
static struct i386tss	*tss_slot[MAXCPU];

extern struct user *proc0paddr;
extern void smp_gdt_set_tss(int id, struct i386tss *t);

struct i386tss *
cpu_tss(int id)
{

	if (id < 0 || id >= MAXCPU)
		return (0);
	if (tss_slot[id] == 0 && id == 0 && proc0paddr != 0) {
		/*
		 * pcb (and pcb_tss, its first member) starts at offset 0 of
		 * struct user, which is why GPROC0_SEL's descriptor base is
		 * kstack.  Registered lazily so it works no matter whether
		 * SMP was enabled at boot.
		 */
		tss_slot[0] = (struct i386tss *)proc0paddr;
	}
	return (tss_slot[id]);
}

unsigned
cpu_tss_sel(int id)
{

	if (id < 0 || id >= MAXCPU)
		id = 0;
	return (((unsigned)(CPU_TSS_SEL_BASE + id)) << 3);
}

/*
 * EN: Give CPU `id` its own TSS pointing at `stack_top` and make the CPU load
 * it.  Called from ap_start() after the kernel GDT is in place.
 */
int
cpu_tss_install(int id, unsigned stack_top)
{
	struct i386tss *t;
	unsigned sel;

	if (id <= 0 || id >= MAXCPU)
		return (-1);
	t = &ap_tss[id];
	{
		char *p = (char *)t;
		int n = (int)sizeof(*t);

		while (n-- > 0)
			*p++ = 0;
	}
	t->tss_ss0 = 0x10;			/* kernel data selector */
	t->tss_esp0 = (int)stack_top;
	tss_slot[id] = t;

	smp_gdt_set_tss(id, t);
	sel = cpu_tss_sel(id);
	__asm __volatile("ltr %0" : : "r" ((unsigned short)sel));
	return (0);
}

extern unsigned vfs_page_alloc(void);

/* Kernel GDT/IDT pseudo-descriptors, loaded by every AP after paging. */
struct gdtr_pack {
	unsigned short	limit;
	unsigned	base;
} __attribute__((packed));
extern struct gdtr_pack r_gdt, r_idt;
extern void lgdt(void *), lidt(void *);

/*
 * S412: AP page directory.
 *
 * IdlePTD's low identity map is not usable here: its PTEs are read-only and
 * pmap_pte() cannot reach them, so an AP that enables paging at 0x7000
 * triple-faults.  Build a private AP page directory with a writable
 * 0..4 MB identity map plus copies of the kernel windows, and point the
 * trampoline at it.  All APs share it (they run kernel code only).
 */
#define	AP_PD_WIN	0xFEB01000u
#define	AP_PT_WIN	0xFEB02000u
static unsigned ap_pd_pa;

static void
ap_pd_setup(void)
{
	unsigned *kpd = (unsigned *)((unsigned)KERNBASE + (unsigned)IdlePTD);
	unsigned *pd, *pt, pt_pa;
	int i;

	ap_pd_pa = vfs_page_alloc();
	pt_pa = vfs_page_alloc();
	pmap_map(AP_PD_WIN, ap_pd_pa, ap_pd_pa + 0x1000,
	    VM_PROT_READ | VM_PROT_WRITE);
	pmap_map(AP_PT_WIN, pt_pa, pt_pa + 0x1000,
	    VM_PROT_READ | VM_PROT_WRITE);
	pd = (unsigned *)AP_PD_WIN;
	pt = (unsigned *)AP_PT_WIN;
	for (i = 0; i < 1024; i++) {
		pd[i] = 0;
		pt[i] = ((unsigned)i << 12) | 0x003;	/* V|RW, supervisor */
	}
	pd[0] = pt_pa | 0x003;
	/* Kernel windows: proc table, stacks, image, CD, caches, tables. */
	for (i = 0x3ec; i < 1024; i++)
		pd[i] = kpd[i];
	/* Its own recursive PTmap (PDRPDROFF = 0x3f7), not IdlePTD's. */
	pd[0x3f7] = ap_pd_pa | 0x003;
	printf("smp: AP page dir %x (low PT %x), tramp win %x\n",
	    ap_pd_pa, pt_pa, AP_TRAMP_WIN);
}


/* S412: topology from CPUID, never from CMOS guesses. */
int	cpu_logical = 1;
int	cpu_cores = 1;
int	cpu_smt = 1;
int	cpu_htt = 0;
char	cpu_vendor[13];

/*
 * Execute CPUID with an explicit subleaf.  EBX is a fixed register on i386,
 * so it must be listed as an output; the kernel is built -fno-pic, which
 * makes that legal.
 */
static void
cpuid(unsigned leaf, unsigned sub, unsigned *a, unsigned *b, unsigned *c,
    unsigned *d)
{

	__asm __volatile("cpuid"
	    : "=a" (*a), "=b" (*b), "=c" (*c), "=d" (*d)
	    : "a" (leaf), "c" (sub));
}

/*
 * S412: CPUID topology.
 *
 * Leaf 1 gives the logical processor count (EBX[23:16]) and the HTT feature
 * bit (EDX[28]).  Leaf 0xB (or 0x1F on newer parts) describes the topology
 * levels: subleaf 0 is the SMT level (type 1), subleaf 1 the core level
 * (type 2), and EBX[15:0] is the number of logical processors at that level.
 * If leaf 0xB is absent we fall back to leaf 4's "cores-1" field.
 */
int
cpu_topology_detect(void)
{
	unsigned maxleaf, a, b, c, d, smt, logical, cores;

	cpuid(0, 0, &maxleaf, &b, &c, &d);
	cpu_vendor[0] = (char)(b & 0xff);
	cpu_vendor[1] = (char)((b >> 8) & 0xff);
	cpu_vendor[2] = (char)((b >> 16) & 0xff);
	cpu_vendor[3] = (char)((b >> 24) & 0xff);
	cpu_vendor[4] = (char)(d & 0xff);
	cpu_vendor[5] = (char)((d >> 8) & 0xff);
	cpu_vendor[6] = (char)((d >> 16) & 0xff);
	cpu_vendor[7] = (char)((d >> 24) & 0xff);
	cpu_vendor[8] = (char)(c & 0xff);
	cpu_vendor[9] = (char)((c >> 8) & 0xff);
	cpu_vendor[10] = (char)((c >> 16) & 0xff);
	cpu_vendor[11] = (char)((c >> 24) & 0xff);
	cpu_vendor[12] = 0;

	cpuid(1, 0, &a, &b, &c, &d);
	cpu_htt = (int)((d >> 28) & 1);
	logical = (b >> 16) & 0xff;
	if (logical == 0)
		logical = 1;

	smt = 1;
	cores = logical;
	if (maxleaf >= 0x0b) {
		unsigned a0, b0, c0, d0, a1, b1, c1, d1;

		cpuid(0x0b, 0, &a0, &b0, &c0, &d0);
		cpuid(0x0b, 1, &a1, &b1, &c1, &d1);
		if (((c0 >> 8) & 0xff) == 1 && (b0 & 0xffff) != 0) {
			smt = b0 & 0xffff;
			if (((c1 >> 8) & 0xff) == 2 && (b1 & 0xffff) != 0)
				logical = b1 & 0xffff;
			cores = (smt != 0) ? logical / smt : logical;
		}
	} else if (maxleaf >= 4) {
		/* No topology leaf: leaf 4 EAX[31:26] = cores - 1. */
		cpuid(4, 0, &a, &b, &c, &d);
		cores = ((a >> 26) & 0x3f) + 1;
		if (cores > logical)
			cores = logical;
	}

	if (cores < 1)
		cores = 1;
	if (logical < cores)
		logical = cores;
	smt = logical / cores;
	if (smt < 1)
		smt = 1;
	cpu_logical = (int)logical;
	cpu_cores = (int)cores;
	cpu_smt = (int)smt;
	return (cpu_logical);
}

/*
 * S415: SMT topology helpers.  AP IDs are handed out consecutively, and
 * cpu_smt is the number of logical processors sharing one physical core
 * (from CPUID leaf 0xB/0x1F, detected above).  With that, the physical core
 * number is simply apic_id / cpu_smt and the sibling thread of a CPU is the
 * other slot in the same group.  These are placement hints only: the
 * scheduler still claims a TCB with an atomic op, so two CPUs can never end
 * up running the same thread even if a hint is stale.
 *
 * S415：SMT 拓扑辅助。APIC ID 连续分配，cpu_smt 是共享一个物理核的逻辑
 * 处理器数（由 CPUID leaf 0xB/0x1F 检测）。于是物理核号 = apic_id/cpu_smt，
 * 同核兄弟是同组里的另一个槽位。这些只是放置提示：线程仍用原子操作认领，
 * 所以即使提示过期，也不会有两个核跑同一线程。
 */
int
smt_phys_core(int apic_id)
{

	if (cpu_smt <= 1)
		return (apic_id);	/* no SMT: every logical CPU is its own core */
	return (apic_id / cpu_smt);
}

int
smt_sibling(int apic_id)
{

	if (cpu_smt <= 1)
		return (-1);		/* no sibling without SMT */
	return ((apic_id / cpu_smt) * cpu_smt +
	    ((apic_id + 1) % cpu_smt));
}

void
cpuinfo_print(void)
{
	unsigned a, b, c, d, family, model, step;

	cpu_topology_detect();
	cpuid(1, 0, &a, &b, &c, &d);
	step = a & 0xf;
	model = (a >> 4) & 0xf;
	family = (a >> 8) & 0xf;
	if (family == 0xf)
		family += (a >> 20) & 0xff;
	if (family == 0x6 || family == 0xf)
		model += ((a >> 16) & 0xf) << 4;
	printf("cpu: vendor %s family %d model %d step %d\n",
	    cpu_vendor[0] ? cpu_vendor : "unknown", (int)family, (int)model,
	    (int)step);
	printf("cpu: logical %d, cores %d, smt %d, htt %d\n",
	    cpu_logical, cpu_cores, cpu_smt, cpu_htt);
	printf("cpu: apic id %d, maxcpu %d\n", (int)lapic_id(), MAXCPU);
}


/*
 * S200: SMP master switch.  The kernel is validated as single-CPU by 17 test
 * suites; bringing APs online changes timing and touches shared page tables.
 * Default 0 keeps every existing test byte-for-byte stable.  Set to 1 (via
 * the 'smpon' monitor command, or SMP_ENABLE at build time) to run the full
 * MP startup protocol.  A conservative boot always completes on the BSP.
 */
#ifndef SMP_ENABLE
#define SMP_ENABLE 0
#endif
int smp_enabled = SMP_ENABLE;

/* S200: IPI/TLB-shootdown statistics (observable via the 'smpinfo' command). */
unsigned ipi_tlb_shootdowns;
unsigned ipi_reschedules;
unsigned smp_tlb_flushes;

/* Local APIC default MMIO physical base; remapped via MSR if needed. */
#define	LAPIC_PA	0xFEE00000u
static unsigned lapic_va = 0;	/* 0 until lapic_init maps it */

/* Local APIC register offsets */
#define	LAPIC_ID	0x20
#define	LAPIC_VER	0x30
#define	LAPIC_TPR	0x80
#define	LAPIC_EOI	0xB0
#define	LAPIC_LDR	0xD0
#define	LAPIC_DFR	0xE0
#define	LAPIC_SPIV	0xF0
#define	LAPIC_ICR_LO	0x300
#define	LAPIC_ICR_HI	0x310
#define	LAPIC_LVT_TIMER	0x320
#define	LAPIC_TIMER_INIT	0x380
#define	LAPIC_TIMER_CUR	0x390
#define	LAPIC_TIMER_DIV	0x3E0

static inline unsigned
lapic_read(unsigned off)
{

	return (lapic_va ? *(volatile unsigned *)(lapic_va + off) : 0);
}

static inline void
lapic_write(unsigned off, unsigned val)
{

	if (lapic_va)
		*(volatile unsigned *)(lapic_va + off) = val;
}

/* EN: Spinlock init. */
void
spin_init(struct spinlock *lk)
{

	if (lk != 0) {
		lk->locked = 0;
		lk->holder = -1;
	}
}

/* EN: Acquire a spinlock with a pause loop. */
void
spin_lock(struct spinlock *lk)
{
	int prev;
	unsigned spins = 0;
	static volatile int spin_stuck;	/* probe: report a wedged lock once */

	if (lk == 0)
		return;
	for (;;) {
		prev = 0;
		__asm __volatile("xchgl %0,%1"
		    : "=r" (prev) : "m" (lk->locked), "0" (1) : "memory");
		if (prev == 0) {
			preempt_disable();	/* hold lock => no preemption */
			lk->holder = cpu_id();
			return;			/* acquired */
		}
		/*
		 * PROBE (temporary): a lock that is never released wedges every
		 * CPU that needs it - with interrupts off, so the machine goes
		 * silent and nothing gets logged.  Say who holds it instead of
		 * spinning forever in the dark.  The counter is per call, so a
		 * normal contended lock never reaches it.
		 * 探针：永不释放的锁会把每个需要它的核卡死（且关中断），整机静默、
		 * 什么日志都没有。这里把持有者打出来，而不是在黑暗里无限自旋。
		 */
		if (++spins == 20000000) {
			/*
			 * Never print twice: if the wedged lock *is* the console
			 * lock, the printf below would recurse into this same
			 * loop, and a second print would recurse again forever.
			 * The flag is set before the call so the inner entry
			 * spins instead of recursing.
			 */
			if (!spin_stuck) {
				spin_stuck = 1;
				printf("spin: lock %p stuck, held by cpu %d, "
				    "cpu %d waiting\n", lk, lk->holder, cpu_id());
			}
			spins = 0;
		}
		__asm __volatile("pause");	/* spin hint */
	}
}

/* EN: Release a spinlock. */
void
spin_unlock(struct spinlock *lk)
{

	if (lk != 0) {
		lk->holder = -1;
		lk->locked = 0;
		preempt_enable();	/* released lock => preemption allowed */
	}
}

/* EN: Return the local APIC ID of this CPU. */
unsigned
lapic_id(void)
{

	return (lapic_read(LAPIC_ID) >> 24);
}

/* EN: Map a hardware APIC id back to our CPU index (-1 when not online). */
int
smp_cpu_by_apicid(unsigned apicid)
{
	int i;

	for (i = 0; i < MAXCPU; i++)
		if (cpus[i].ci_active && cpus[i].ci_apicid == apicid)
			return (i);
	return (-1);
}

/*
 * EN: Send a fixed-delivery IPI to CPU `target` (an index into cpus[], not a
 * hardware APIC id - the two only coincide while the firmware numbers the
 * CPUs 0..n-1, which is the case on QEMU/VMware but is not guaranteed).
 */
void
lapic_send_ipi(int target, int vector)
{
	unsigned apic;

	apic = (target >= 0 && target < MAXCPU) ? cpus[target].ci_apicid :
	    (unsigned)target;
	lapic_write(LAPIC_ICR_HI, (unsigned)(apic << 24));
	/* Fixed delivery, edge triggered: the low byte is just the vector.
	 * 0x500 is delivery mode 5 (INIT), which would reset the target. */
	lapic_write(LAPIC_ICR_LO, (unsigned)vector);
}

/* EN: Signal end-of-interrupt to the local APIC. */
void
lapic_eoi(void)
{

	lapic_write(LAPIC_EOI, 0);
}

/* EN: Program the timer for a short delay used between IPIs. */
static void
ap_delay(void)
{
	volatile int i;

	for (i = 0; i < 200000; i++)
		;
}

/* EN: Send INIT IPI to CPU index `target` (translated to its APIC id). */
static void
ap_send_init(int target)
{
	unsigned apic = (target >= 0 && target < MAXCPU) ?
	    cpus[target].ci_apicid : (unsigned)target;

	lapic_write(LAPIC_ICR_HI, (unsigned)(apic << 24));
	lapic_write(LAPIC_ICR_LO, 0x500);	/* INIT, level-triggered */
	ap_delay();
	lapic_write(LAPIC_ICR_LO, 0);		/* de-assert */
	ap_delay();
}

/* EN: Send STARTUP (SIPI) IPI: vector = trampoline_pa >> 12 (0x7000 -> 0x07). */
static void
ap_send_sipi(int target)
{
	int vec = CPU_TRAMPOLINE_PA >> 12;
	unsigned apic = (target >= 0 && target < MAXCPU) ?
	    cpus[target].ci_apicid : (unsigned)target;

	lapic_write(LAPIC_ICR_HI, (unsigned)(apic << 24));
	lapic_write(LAPIC_ICR_LO, 0x600 | vec);	/* SIPI */
	ap_delay();
}

/*
 * S200: Map the Local APIC MMIO page into a fixed, reserved kernel VA window.
 *
 * The old implementation left the APIC unmapped because calling pmap_map()
 * advanced virtual_avail and corrupted the page-table layout that user exec
 * depends on.  We avoid pmap entirely here: vtopte(va) gives the PTE for a
 * kernel virtual address through the recursive page-table window, so we can
 * install one uncacheable mapping by hand without touching any allocator
 * state.  LAPIC_VA (0xFE800000) sits in the reserved kernel window above the
 * uring region, so no other code maps it.
 */
/*
 * PTmap is the recursive page-table window: PTmap[(va >> 12)] is the PTE
 * that maps virtual page `va`.  We declare it as raw 32-bit entries so we do
 * not have to pull in the pmap.h/machine headers (which need types like
 * boolean_t / simple_lock_data_t that this translation unit lacks).
 *
 * i386 PTE bits used here:
 *   bit 0    P    present
 *   bit 1    RW   writable
 *   bit 5    PCD  cache disable (mandatory for APIC MMIO)
 *   bits 31:12    physical page frame number
 */
extern unsigned int PTmap[];
#define S200_PTE_P	0x001
#define S200_PTE_RW	0x002
#define S200_PTE_PCD	0x020

static void
lapic_map(void)
{
	volatile unsigned int *pte;

	pte = &PTmap[LAPIC_VA >> PGSHIFT];
	*pte = ((LAPIC_PA >> PGSHIFT) << 12) |
	    S200_PTE_P | S200_PTE_RW | S200_PTE_PCD;
	/* invalidate the TLB entry for this VA */
	__asm __volatile("invlpg (%0)" : : "r" (LAPIC_VA) : "memory");
	lapic_va = LAPIC_VA;
}

/*
 * EN: Initialize per-CPU state and try to start APs.  If no Local APIC is
 * present (lapic readback invalid), stay single-CPU -- this keeps QEMU with
 * a single CPU and the existing tests working.
 */
void
smp_init(void)
{
	int i, detected;

	for (i = 0; i < MAXCPU; i++) {
		cpus[i].ci_id = i;
		cpus[i].ci_active = (i == 0);
		cpus[i].ci_curthread = 0;
		cpus[i].ci_ticks = 0;
	}
	ncpu = 1;
	/* S200: only map/enable the APIC when SMP is explicitly requested.
	 * Default builds stay single-CPU with zero behavioural change. */
	if (!smp_enabled) {
		printf("smp: per-CPU ready, 1 CPU (SMP disabled)\n");
		return;
	}
	lapic_map();
	smp_idt_init();
	detected = mp_detect();
	printf("smp: per-CPU ready, %d CPU(s) detected, lapic %s\n",
	    detected, lapic_va ? "mapped" : "absent");
	if (detected > 1)
		smp_start_aps(detected);
}

/*
 * Copy the raw AP trampoline to 0x7000 and patch the fields that are common
 * to every AP: GDT physical base, kernel page directory, and ap_start().
 * Per-AP stack/cpu-id are patched immediately before each SIPI.
 */
static void
ap_tramp_install(void)
{
	unsigned char *p;
	unsigned i, n = ap_tramp_bin_len;

	/*
	 * The low identity map is read-only and pmap_pte() does not resolve
	 * user-range VAs, so write the blob through a high kernel window that
	 * pmap_map() maps to physical 0x7000.  The AP still executes at low
	 * 0x7000: reading/executing the read-only PTE is fine.
	 */
	pmap_map((unsigned)AP_TRAMP_WIN, (unsigned)CPU_TRAMPOLINE_PA,
	    (unsigned)(CPU_TRAMPOLINE_PA + 0x1000),
	    VM_PROT_READ | VM_PROT_WRITE);
	p = (unsigned char *)AP_TRAMP_WIN;

	if (n > 0x1000)
		n = 0x1000;
	for (i = 0; i < n; i++)
		p[i] = ap_tramp_bin[i];
	ap_pd_setup();
	*(unsigned *)(p + AP_TRAMP_PD) = ap_pd_pa;
	*(unsigned *)(p + AP_TRAMP_ENTRY) = (unsigned)ap_start;
	ap_tramp_installed = 1;
	printf("smp: AP trampoline %d bytes at %x, pd %x\n", (int)n,
	    CPU_TRAMPOLINE_PA, ap_pd_pa);
}

/*
 * EN: Boot application processors 1..count-1 via the MP protocol.  The AP
 * trampoline is a raw 0x7000 blob embedded in the kernel image.
 */
void
smp_start_aps(int count)
{
	int ap, i;

	if (lapic_va == 0 || count <= 1)
		return;
	if (!ap_tramp_installed)
		ap_tramp_install();
	if (!ap_tramp_installed)
		return;
	printf("smp: starting %d AP(s) via INIT/SIPI\n", count - 1);
	for (ap = 1; ap < count && ap < MAXCPU; ap++) {
		/* per-AP stack and cpu id, then INIT-SIPI-SIPI */
		*(unsigned *)(AP_TRAMP_WIN + AP_TRAMP_STACK) =
		    (unsigned)(ap_stacks[ap] + sizeof(ap_stacks[ap]));
		*(unsigned *)(AP_TRAMP_WIN + AP_TRAMP_CPUID) = (unsigned)ap;
		ap_send_init(ap);
		ap_send_sipi(ap);
		ap_send_sipi(ap);
		for (i = 0; i < 20 && !cpus[ap].ci_active; i++)
			ap_delay();
		if (!cpus[ap].ci_active)
			printf("smp: AP %d did not respond\n", ap);
	}
}

/*
 * S200: scan for the MP floating-pointer structure ("_MP_" signature) in
 * the three legal BIOS regions: EBDA (via BDA word at 0x40e), the 0xF0000
 * BIOS segment, and the extended BIOS ROM.  Returns the CPU count encoded
 * in the MP config table header, or 0 if not found / unusable.
 *
 * The floating pointer is 16-byte aligned; its byte 11 is the "MP feature
 * information byte 2" which, when bit 7 is set, means the config table is
 * present.  We read the config table's header byte 8 (entry count) and infer
 * CPU count conservatively from the CMOS value as the authoritative source,
 * using the MP table only to confirm an MP-capable system.
 */
static int
mp_scan_floating(void)
{
	unsigned long base;
	int found = 0;

	/* Physical memory is mapped at KERNBASE (0xFE000000) in this port, so
	 * a physical address pa is readable at (pa + 0xFE000000). */
#define S200_KVA(pa)	((volatile unsigned char *)((unsigned)(pa) + 0xFE000000u))
	/* BDA word at physical 0x40e holds the EBDA segment; <<4 = byte address */
	{
		unsigned short ebda_seg = *(volatile unsigned short *)S200_KVA(0x040e);
		base = ((unsigned long)ebda_seg) << 4;
		if (base == 0 || base > 0x100000u)
			base = 0x9FC00u;	/* typical EBDA fallback */
	}
	/* Candidate regions: EBDA (1K), then 0xF0000 BIOS segment (64K). */
	{
		int region;
		for (region = 0; region < 2 && !found; region++) {
			unsigned long start = region == 0 ? base : 0xF0000u;
			unsigned long span = region == 0 ? 0x400 : 0x10000;
			unsigned long off;

			for (off = 0; off < span; off += 16) {
				volatile unsigned char *q = S200_KVA(start + off);
				if (q[0] == '_' && q[1] == 'M' && q[2] == '_' && q[3] == 'P') {
					found = 1;
					break;
				}
			}
		}
	}
#undef S200_KVA
	return (found);
}

/*
 * EN: Detect the number of schedulable logical CPUs from CPUID.  Returns the
 * count clamped to MAXCPU; cpus[i].ci_apicid is filled with the expected APIC
 * ID for now (QEMU/VMware number them 0..n-1; ACPI MADT parsing can replace
 * this later without touching the scheduler).
 */
int
mp_detect(void)
{
	int mp, n, i;

	if (lapic_va == 0)
		return (1);		/* no Local APIC: single CPU */
	if ((lapic_read(LAPIC_VER) & 0xff) == 0)
		return (1);		/* version 0: APIC not usable */
	mp = mp_scan_floating();
	n = cpu_topology_detect();
	printf("smp: MP table %s, CPUID logical %d cores %d smt %d htt %d\n",
	    mp ? "found" : "absent", cpu_logical, cpu_cores, cpu_smt, cpu_htt);
	if (n > MAXCPU) {
		printf("smp: logical %d > MAXCPU %d, using first %d\n", n, MAXCPU,
		    MAXCPU);
		n = MAXCPU;
	}
	for (i = 0; i < n; i++)
		cpus[i].ci_apicid = (unsigned)i;
	return (n > 1 ? n : 1);
}

/*
 * EN: C entry point for an application processor.  Called from ap_tramp.s
 * after the AP has entered protected mode with paging and the shared
 * kernel page directory.  Sets up per-CPU state and enters the idle loop.
 */
void
ap_start(int id)
{
	unsigned stack_top;

	if (id <= 0 || id >= MAXCPU)
		return;
	stack_top = (unsigned)(ap_stacks[id] + sizeof(ap_stacks[id]));
	stack_top &= ~15u;
	cpus[id].ci_active = 1;
	cpus[id].ci_curthread = 0;
	cpus[id].ci_lapic = lapic_va;
	cpus[id].ci_apicid = lapic_id();	/* S413: real id, not the index */
	/*
	 * The trampoline used a temporary GDT and the AP still has the
	 * real-mode IDTR, so load the kernel's GDT (for future TSS/ring3)
	 * and IDT (for timer/IPI vectors) before touching the APIC.
	 */
	lgdt(&r_gdt);
	lidt(&r_idt);
	/*
	 * S413: the LDTR is per-CPU as well.  Every user-mode selector we
	 * build (ring-3 code/data) indexes the LDT, so an AP that never loads
	 * it dies on resume_user's `mov %edi,%ds` with vector 9 - the selector
	 * has TI=1 while its LDTR is null.
	 * S413：LDTR 也是每核的。我们构造的所有用户态选择子都指向 LDT，AP 不加载
	 * 它的话，resume_user 里的 `mov %edi,%ds` 会因 TI=1 而 LDTR 为空触发 9 号
	 * 异常。
	 */
	__asm __volatile("lldt %0" : : "r" ((unsigned short)0x18));	/* GLDT_SEL<<3 */
	/*
	 * S413: per-CPU TSS.  This must happen before any ring-3 entry can
	 * happen on this CPU, because the TSS supplies the ring-0 stack.
	 */
	cpu_tss_install(id, stack_top);
	lapic_init();
	lapic_timer_init();
	ncpu++;
	/* S413: give this CPU its own scheduler state and let it dispatch. */
	sched_percpu_init(id, stack_top);
	/*
	 * Do not printf() from an AP: the console has no cross-CPU lock and
	 * interleaved characters corrupt the BSP's monitor output.  The BSP's
	 * "smpon: N CPU(s) online" is the single summary line.
	 * 不从 AP 直接 printf：控制台没有跨核锁，字符会与 BSP 输出交错。
	 */
	__asm __volatile("sti");
	ap_sched();		/* never returns */
	for (;;)
		__asm __volatile("hlt");
}

/* EN: Local APIC init: enable APIC via spurious vector register. */
void
lapic_init(void)
{

	if (lapic_va == 0)
		return;
	lapic_write(LAPIC_DFR, 0xffffffff);	/* flat model */
	/* Flat logical destination: one bit per logical APIC ID in LDR[31:24]. */
	lapic_write(LAPIC_LDR, 1u << (24 + (lapic_id() & 0x7)));
	lapic_write(LAPIC_LVT_TIMER, 0x10000);	/* masked for now */
	lapic_write(LAPIC_SPIV, 0x100 | 0x3f);	/* enable + spurious vec 0x3f */
}

/* EN: Start the AP local timer (placeholder: one-shot, masked). */
void
lapic_timer_init(void)
{

	if (lapic_va == 0)
		return;
	lapic_write(LAPIC_TIMER_DIV, 0x3);	/* divide by 16 */
	lapic_write(LAPIC_TIMER_INIT, 0);	/* no counts yet */
}

/* EN: Parallel idle/balance hook: migrate threads from busy to idle CPU. */
void
smp_balance(void)
{

	if (ncpu <= 1)
		return;
	/* per-CPU run queues and IPI-driven migration are wired in once APs
	 * report online; the policy hook is in place. */
}

/* ------------------------------------------------------------------ */
/* S200: inter-processor operations and diagnostics                    */
/* ------------------------------------------------------------------ */

/*
 * smp_tlb_shootdown - invalidate a virtual address on all other CPUs.
 *
 * On x86 each CPU has its own TLB, so a pmap change made on one CPU must be
 * invalidated on the others.  We broadcast IPI_TLB_SHOOTDOWN; each receiver
 * reloads CR3 (a full inclusive flush - simple and correct for this port).
 * In a uniprocessor build this is a single local invlpg, zero IPI traffic.
 */
void
smp_tlb_shootdown(unsigned va)
{
	int me, i;

	__asm __volatile("invlpg (%0)" : : "r" (va) : "memory");
	smp_tlb_flushes++;		/* local flush happened regardless of CPU count */
	if (ncpu <= 1 || lapic_va == 0)
		return;
	me = cpu_id();			/* S413: TSS selector, not the APIC id */
	for (i = 0; i < MAXCPU; i++) {
		if (i == me || !cpus[i].ci_active)
			continue;
		ipi_tlb_shootdowns++;
		lapic_send_ipi(i, IPI_TLB_SHOOTDOWN);
	}
}

/*
 * smp_reschedule - nudge other CPUs to re-run the scheduler after a wakeup.
 */
void
smp_reschedule(void)
{
	int me, i;

	if (ncpu <= 1 || lapic_va == 0)
		return;
	me = cpu_id();
	for (i = 0; i < MAXCPU; i++) {
		if (i == me || !cpus[i].ci_active)
			continue;
		ipi_reschedules++;
		lapic_send_ipi(i, IPI_RESCHEDULE);
	}
}

/* EN: S413: wake one CPU (likely owner of a freshly runnable thread). */
void
smp_resched_cpu(int id)
{

	if (id < 0 || id >= MAXCPU || !cpus[id].ci_active)
		return;
	if (id == cpu_id() || lapic_va == 0)
		return;
	ipi_reschedules++;
	lapic_send_ipi(id, IPI_RESCHEDULE);
}

/*
 * smp_handle_ipi - called from the APIC interrupt vector table.  vector is
 * the interrupt number; IPI vectors are 0x40..0x4f.  A full CR3 reload is a
 * safe inclusive TLB flush for the shootdown case.
 */
void
smp_handle_ipi(int vector)
{
	unsigned cr3;
	int me = smp_cpu_by_apicid(lapic_id());

	if (me < 0)
		me = cpu_id();
	if (me >= 0 && me < MAXCPU)
		cpus[me].ci_ticks++;

	switch (vector) {
	case IPI_TLB_SHOOTDOWN:
		cr3 = cpu_read_cr3();
		cpu_load_cr3(cr3);	/* flush all non-global TLB entries */
		break;
	case IPI_RESCHEDULE:
		/*
		 * S413: another CPU made a thread runnable (or wants this CPU
		 * to look again).  Set this CPU's reschedule flag; the flag is
		 * consumed at the next safe point, never inside this handler -
		 * switching stacks from an IPI frame would strand the frame.
		 */
		smp_need_resched();
		break;
	case IPI_STOP:
		cpus[(me >= 0 && me < MAXCPU) ? me : 0].ci_active = 0;
		break;
	default:
		break;
	}
	lapic_eoi();
}

/*
 * smp_enable - runtime opt-in used by the monitor 'smpon' command.  Maps the
 * APIC, detects CPUs and (if >1) starts the APs.  Returns the CPU count.
 */
int
smp_enable(void)
{
	int detected;

	if (smp_enabled)
		return (ncpu);
	smp_enabled = 1;
	lapic_map();
	smp_idt_init();
	detected = mp_detect();
	printf("smp: enabling, %d CPU(s) detected, lapic %s\n",
	    detected, lapic_va ? "mapped" : "absent");
	if (detected > 1)
		smp_start_aps(detected);
	return (ncpu);
}

/*
 * smp_print - dump SMP status via the kernel printf (monitor 'smpinfo').
 */
void
smp_print(void)
{
	int i, active = 0;

	for (i = 0; i < MAXCPU; i++)
		if (cpus[i].ci_active)
			active++;
	printf("smp: enabled=%d active=%d ncpu=%d maxcpu=%d lapic=%s\n",
	    smp_enabled, active, ncpu, MAXCPU, lapic_va ? "mapped" : "absent");
	printf("smp: ipi_tlb=%u ipi_resched=%u tlb_flushes=%u\n",
	    ipi_tlb_shootdowns, ipi_reschedules, smp_tlb_flushes);
	for (i = 0; i < MAXCPU; i++)
		if (cpus[i].ci_active)
			printf("smp: cpu%d apic %d active ticks %u\n", i,
			    (int)cpus[i].ci_apicid, cpus[i].ci_ticks);
	/*
	 * S413: per-CPU scheduling view - switch counts plus the thread each
	 * CPU is running right now.  Different non-negative tids on different
	 * CPUs is the proof that user threads really run in parallel.
	 */
	for (i = 0; i < MAXCPU; i++)
		if (cpus[i].ci_active) {
			struct kthread *ct = (struct kthread *)cpus[i].ci_curthread;

			printf("smp: sched cpu%d switches %u running tid %d\n",
			    i, sched_cpu_switches(i),
			    ct != 0 ? ct->t_tid : -1);
		}
}

/*
 * ipi_selftest - exercise the IPI/TLB-shootdown path in a safe way.  With a
 * single active CPU this only performs the local invlpg (no IPI is sent), so
 * it is always safe to run and validates the call path and counters.
 */
int
ipi_selftest(void)
{
	unsigned before = smp_tlb_flushes;

	smp_tlb_shootdown(LAPIC_VA);
	smp_reschedule();
	return (smp_tlb_flushes == before + 1);
}
