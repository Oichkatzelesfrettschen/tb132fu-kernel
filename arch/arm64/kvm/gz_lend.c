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

struct gz_lend_cpu {
	struct user_fpsimd_state fp;
	unsigned long save_pa;
};

static DEFINE_PER_CPU(struct gz_lend_cpu, gz_lend_cpu);

static unsigned long gz_lend_shim_pa;
static u64 gz_lend_run_fn __read_mostly = ~0ULL;
static bool gz_lend_armed;
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
	arm_smccc_hvc(GZ_LEND_ID, gz_lend_shim_pa, a0, a1, a2, a3,
		      c->save_pa | (a0 == gz_lend_run_fn),
		      (unsigned long)&c->fp, &res);
	if (unlikely(res.a0 == GZ_LEND_FAULT)) {
		local_irq_restore(flags);
		pr_err("gz_lend: EL2 fault in call %llx(%llx, %llx, %llx): far=%lx esr=%lx elr=%lx\n",
		       a0, a1, a2, a3, res.a1, res.a2, res.a3);
		return 0;
	}
	if (res.a1 == 1)
		fpsimd_load_state(&c->fp);
	local_irq_restore(flags);
	if (gz_lend_trace)
		pr_info("gz_lend: cpu%d ret %lx fp %lx run %d\n",
			raw_smp_processor_id(), res.a0, res.a1,
			a0 == gz_lend_run_fn);

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
	return create_hyp_mappings(&c->fp, &c->fp + 1, PAGE_HYP);
}

/*
 * CPUs on which a guest may be entered through the lend. Guest runs on the
 * Cortex-A78 cores of the first target stop that CPU, so the default covers
 * the Cortex-A55 cores; gz_lend.vcpu_cpus= widens it.
 */
static unsigned long gz_lend_vcpu_cpus = 0x0f;
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
