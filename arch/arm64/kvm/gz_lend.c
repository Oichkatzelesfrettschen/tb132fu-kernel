// SPDX-License-Identifier: GPL-2.0
/*
 * KVM on lent EL2. The kernel runs at EL1 under a hypervisor that exposes
 * SMCCC HVC 0xEF000E13(entry, args...): it runs entry at EL2 with the MMU off
 * and returns x0-x3. __gz_lend_shim is that entry; it installs KVM's non-VHE
 * hyp context for the duration of one hyp call.
 */
#include <linux/arm-smccc.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/pfn.h>
#include <linux/printk.h>
#include <asm/cacheflush.h>
#include <asm/fpsimd.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_hyp.h>
#include <asm/memory.h>
#include <asm/virt.h>
#include <asm/sysreg.h>
#include <asm/barrier.h>
#include <linux/smp.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/bitops.h>

#define GZ_LEND_ID	0xEF000E13UL
#define GZ_LEND_REJECT	0xffffffffUL
#define GZ_LEND_FAULT	0xdead0001UL

bool gz_lend_mode __ro_after_init;
EXPORT_SYMBOL_GPL(gz_lend_mode);

extern char __gz_lend_shim[];
int kvm_arm_gz_lend_late_init(void);

/* __hyp_do_panic reads save_pa and fault_pa at these offsets. */
struct gz_lend_cpu {
	struct user_fpsimd_state fp;
	unsigned long save_pa;		/* 0x210 */
	unsigned long fault_pa;		/* 0x218 */
	bool dead;
};

DEFINE_PER_CPU(struct gz_lend_cpu, gz_lend_cpu);
extern char __gz_lend_fault_run[];

static unsigned long gz_lend_shim_pa;
static u64 gz_lend_run_fn __read_mostly = ~0ULL;
static bool gz_lend_armed;

/*
 * An EL3 upcall that faults on a lent CPU never issues its return call, so
 * the secure world's caller stays suspended inside EL3. The default response
 * is a restart; 0 keeps the system up with that CPU's KVM context dead.
 */
static bool gz_lend_upcall_panic = true;
core_param(gz_lend_upcall_panic, gz_lend_upcall_panic, bool, 0644);

/*
 * gz_lend_el3_entry= names the EL2 address at which EL3 enters the lender
 * when the secure world issues one of its SiP calls into it. The entry
 * assumes the lender's own EL2 translation tables. 0 (the default) turns the
 * classification off.
 */
static unsigned long gz_lend_el3_entry __ro_after_init;

static int __init gz_lend_el3_entry_setup(char *s)
{
	return kstrtoul(s, 16, &gz_lend_el3_entry);
}
early_param("gz_lend_el3_entry", gz_lend_el3_entry_setup);

bool gz_lend_fault_is_el3_upcall(unsigned long elr)
{
	return gz_lend_el3_entry && elr == gz_lend_el3_entry;
}

/*
 * gz_lend_el3_decline=<ret>,<fn>,<fn_reply>,<reply> (hexadecimal) places one
 * page of code at gz_lend_el3_entry in KVM's EL2 address space. EL3 enters
 * it with the caller's function ID in x0. The code issues the SMC <ret> with
 * x1 = <fn_reply> when the low 16 bits of x0 equal <fn>, x1 = <reply>
 * otherwise, and x2-x4 = 0. It uses registers only: EL3 hands over a stack
 * that KVM's tables leave unmapped. The MMU-off windows of the lend keep
 * the fault report above.
 */
static u64 gz_lend_decline_lit[4];
static bool gz_lend_decline_set __ro_after_init;

static int __init gz_lend_el3_decline_setup(char *s)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(gz_lend_decline_lit); i++) {
		char *tok = strsep(&s, ",");

		if (!tok || kstrtou64(tok, 16, &gz_lend_decline_lit[i]))
			return -EINVAL;
	}
	gz_lend_decline_set = !s;
	return gz_lend_decline_set ? 0 : -EINVAL;
}
early_param("gz_lend_el3_decline", gz_lend_el3_decline_setup);

extern const char __gz_lend_decline[], __gz_lend_decline_lit[];

asm(
"	.pushsection .rodata, \"a\"\n"
"	.balign	8\n"
"	.globl	__gz_lend_decline\n"
"__gz_lend_decline:\n"
"	and	w9, w0, #0xffff\n"
"	ldr	x0, 1f\n"
"	ldr	x10, 1f + 8\n"
"	ldr	x11, 1f + 16\n"
"	ldr	x1, 1f + 24\n"
"	cmp	x9, x10\n"
"	csel	x1, x11, x1, eq\n"
"	mov	x2, xzr\n"
"	mov	x3, xzr\n"
"	mov	x4, xzr\n"
"	smc	#0\n"
"0:	wfe\n"
"	b	0b\n"
"	.balign	8\n"
"	.globl	__gz_lend_decline_lit\n"
"__gz_lend_decline_lit:\n"
"1:	.quad	0, 0, 0, 0\n"
"	.popsection\n"
);

static int gz_lend_map_decline(void)
{
	size_t code = __gz_lend_decline_lit - __gz_lend_decline;
	unsigned long page;
	u64 pte;
	int ret;

	if (!gz_lend_decline_set || !gz_lend_el3_entry)
		return 0;
	page = get_zeroed_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	memcpy((void *)page, __gz_lend_decline, code);
	memcpy((void *)page + code, gz_lend_decline_lit,
	       sizeof(gz_lend_decline_lit));
	flush_icache_range(page, page + PAGE_SIZE);
	ret = create_hyp_fixed_exec_mapping(gz_lend_el3_entry,
					    __pa(page), &pte);
	if (ret) {
		free_page(page);
		pr_err("gz_lend: EL3 decline page not mapped: %d\n", ret);
		return ret;
	}
	pr_info("gz_lend: EL3 decline page mapped, entry %llx\n", pte);
	return 0;
}

static bool gz_lend_auto __initdata;
bool gz_lend_trace;

/* The boot CPU records EL1 entry in __boot_cpu_mode[0] before this runs. */
static int __init gz_lend_setup(char *s)
{
	if (s && (*s == '1' || *s == '2') &&
	    __boot_cpu_mode[0] == BOOT_CPU_MODE_EL1) {
		gz_lend_mode = true;
		gz_lend_auto = *s == '1';
	}
	return 0;
}
early_param("gz_lend", gz_lend_setup);

/*
 * Replaces the bare HVC of __kvm_call_hyp and of the hyp-stub calls. The
 * lender restores the FP/SIMD registers it saved at HVC entry, so the values
 * the hyp call left live are reloaded from the buffer the shim filled.
 */
u64 gz_lend_hyp_call(u64 a0, u64 a1, u64 a2, u64 a3)
{
	struct arm_smccc_res res;
	struct gz_lend_cpu *c;
	unsigned long flags;

	if (!gz_lend_armed)
		return HVC_STUB_ERR;

	if (gz_lend_trace)
		pr_info("gz_lend: cpu%d call %llx %llx %llx %llx\n",
			raw_smp_processor_id(), a0, a1, a2, a3);
	local_irq_save(flags);
	c = this_cpu_ptr(&gz_lend_cpu);
	if (unlikely(c->dead)) {
		local_irq_restore(flags);
		return a0 == gz_lend_run_fn ? ARM_EXCEPTION_HYP_GONE : 0;
	}
	arm_smccc_hvc(GZ_LEND_ID, gz_lend_shim_pa, a0, a1, a2, a3,
		      c->save_pa | (a0 == gz_lend_run_fn),
		      (unsigned long)&c->fp, &res);
	if (unlikely((res.a0 & ~3UL) == 0xdead0000UL)) {
		/* KVM's EL2 context on this CPU is gone. */
		c->dead = true;
		local_irq_restore(flags);
		pr_err("gz_lend: EL2 fault in call %llx(%llx, %llx, %llx): far=%lx esr=%lx elr=%lx\n",
		       a0, a1, a2, a3, res.a1, res.a2, res.a3);
		if (gz_lend_fault_is_el3_upcall(res.a3)) {
			pr_emerg("gz_lend: cpu%d took a secure-world upcall into the lender while EL2 was lent\n",
				 raw_smp_processor_id());
			if (gz_lend_upcall_panic)
				panic("gz_lend: unanswered EL3 upcall");
		}
		return a0 == gz_lend_run_fn ? ARM_EXCEPTION_HYP_GONE : 0;
	}
	if (res.a1 == 1)
		fpsimd_load_state(&c->fp);
	local_irq_restore(flags);
	if (gz_lend_trace)
		pr_info("gz_lend: cpu%d ret %lx fp %lx run %d cptr %lx hcr %lx\n",
			raw_smp_processor_id(), res.a0, res.a1,
			a0 == gz_lend_run_fn, res.a2, res.a3);

	WARN_ONCE(res.a0 == GZ_LEND_REJECT && res.a1 == 0 && a0 < HVC_STUB_HCALL_NR,
		  "gz_lend: hypervisor rejected the lend call\n");
	return res.a0;
}

/*
 * The shim stores the live FP/SIMD registers through the hyp mapping of the
 * per-CPU buffer; 4.19 maps only kvm_host_cpu_state of the per-CPU area.
 */
int gz_lend_map_hyp(int cpu)
{
	struct gz_lend_cpu *c = &per_cpu(gz_lend_cpu, cpu);

	if (!gz_lend_mode)
		return 0;
	BUILD_BUG_ON(offsetof(struct gz_lend_cpu, save_pa) != 0x210);
	BUILD_BUG_ON(offsetof(struct gz_lend_cpu, fault_pa) != 0x218);
	if (cpu == cpumask_first(cpu_possible_mask)) {
		int ret = gz_lend_map_decline();

		if (ret)
			return ret;
	}
	return create_hyp_mappings(c, c + 1, PAGE_HYP);
}

/*
 * CPUs on which a guest may be entered through the lend; a debugging aid
 * that confines vCPU threads to a subset.
 */
static unsigned long gz_lend_vcpu_cpus = ~0UL;
core_param(gz_lend_vcpu_cpus, gz_lend_vcpu_cpus, ulong, 0644);

bool gz_lend_vcpu_cpu_ok(void)
{
	return gz_lend_vcpu_cpus & BIT(raw_smp_processor_id());
}

/* Moves the calling vCPU thread onto the permitted CPUs; may sleep. */
void gz_lend_pin_vcpu_thread(void)
{
	cpumask_t mask;

	cpumask_clear(&mask);
	*cpumask_bits(&mask) = gz_lend_vcpu_cpus;
	cpumask_and(&mask, &mask, cpu_online_mask);
	if (cpumask_empty(&mask) || cpumask_subset(&current->cpus_allowed, &mask))
		return;
	set_cpus_allowed_ptr(current, &mask);
}

/*
 * The host runs on the physical timer in lend mode. Whatever ran before may
 * have left the EL1 virtual timer enabled with an expired compare value, and
 * KVM enables that timer's interrupt on every CPU for its guests.
 */
static void gz_lend_quiesce_vtimer(void *unused)
{
	write_sysreg(0, cntv_ctl_el0);
	isb();
}

/* Allocates the per-CPU save pages and removes them from the data caches. */
int gz_lend_prepare(void)
{
	int cpu;

	if (!gz_lend_mode)
		return -ENODEV;
	if (gz_lend_armed)
		return 0;

	for_each_possible_cpu(cpu) {
		struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);

		if (!page)
			return -ENOMEM;
		__flush_dcache_area(page_address(page), PAGE_SIZE);
		per_cpu(gz_lend_cpu, cpu).save_pa = page_to_phys(page);
		per_cpu(gz_lend_cpu, cpu).fault_pa = __pa_symbol(__gz_lend_fault_run);
	}
	on_each_cpu(gz_lend_quiesce_vtimer, NULL, 1);
	gz_lend_shim_pa = __pa_symbol(__gz_lend_shim);
	gz_lend_run_fn = (u64)kvm_ksym_ref(__kvm_vcpu_run_nvhe);
	gz_lend_armed = true;
	return 0;
}

/* Starts the deferred KVM initialisation on lent EL2. */
int gz_lend_arm(void)
{
	static bool done;
	int ret;

	ret = gz_lend_prepare();
	if (ret)
		return ret;
	if (done)
		return -EBUSY;
	done = true;

	ret = kvm_arm_gz_lend_late_init();
	pr_info("gz_lend: armed, shim at %lx, kvm init %d\n", gz_lend_shim_pa, ret);
	return ret;
}

/*
 * gz_lend=1 arms the lend by itself 20 s after the initcalls, which is
 * before Android marks the boot slot successful, so a failure here still
 * counts against the slot's retry counter. gz_lend=2 leaves arming to a
 * write of "arm" to /proc/gz_lend.
 */
static void gz_lend_auto_arm(struct work_struct *work)
{
	gz_lend_arm();
}
static DECLARE_DELAYED_WORK(gz_lend_auto_work, gz_lend_auto_arm);

static int __init gz_lend_auto_init(void)
{
	if (gz_lend_mode && gz_lend_auto)
		schedule_delayed_work(&gz_lend_auto_work, 20 * HZ);
	return 0;
}
late_initcall(gz_lend_auto_init);
