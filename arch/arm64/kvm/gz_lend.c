// SPDX-License-Identifier: GPL-2.0
/*
 * KVM on lent EL2. The kernel runs at EL1 under a hypervisor that exposes
 * SMCCC HVC 0xEF000E13(entry, args...): it runs entry at EL2 with the MMU off
 * and returns x0-x3. __gz_lend_shim is that entry; it installs KVM's non-VHE
 * hyp context for the duration of one hyp call.
 */
#include <linux/arm-smccc.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <asm/cacheflush.h>
#include <asm/fpsimd.h>
#include <asm/memory.h>
#include <asm/virt.h>

#define GZ_LEND_ID	0xEF000E13UL
#define GZ_LEND_REJECT	0xffffffffUL

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
static bool gz_lend_armed;

/* The boot CPU records EL1 entry in __boot_cpu_mode[0] before this runs. */
static int __init gz_lend_setup(char *s)
{
	if (s && *s == '1' && __boot_cpu_mode[0] == BOOT_CPU_MODE_EL1)
		gz_lend_mode = true;
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

	local_irq_save(flags);
	c = this_cpu_ptr(&gz_lend_cpu);
	arm_smccc_hvc(GZ_LEND_ID, gz_lend_shim_pa, a0, a1, a2, a3,
		      c->save_pa, (unsigned long)&c->fp, &res);
	if (res.a1 == 1)
		fpsimd_load_state(&c->fp);
	local_irq_restore(flags);

	WARN_ONCE(res.a0 == GZ_LEND_REJECT && res.a1 == 0 && a0 < HVC_STUB_HCALL_NR,
		  "gz_lend: hypervisor rejected the lend call\n");
	return res.a0;
}

/*
 * Allocates the per-CPU save pages, removes them from the data caches and
 * starts the deferred KVM initialisation.
 */
int gz_lend_arm(void)
{
	int cpu, ret;

	if (!gz_lend_mode)
		return -ENODEV;
	if (gz_lend_armed)
		return -EBUSY;

	for_each_possible_cpu(cpu) {
		struct page *page = alloc_page(GFP_KERNEL | __GFP_ZERO);

		if (!page)
			return -ENOMEM;
		__flush_dcache_area(page_address(page), PAGE_SIZE);
		per_cpu(gz_lend_cpu, cpu).save_pa = page_to_phys(page);
	}
	gz_lend_shim_pa = __pa_symbol(__gz_lend_shim);
	gz_lend_armed = true;

	ret = kvm_arm_gz_lend_late_init();
	pr_info("gz_lend: armed, shim at %lx, kvm init %d\n", gz_lend_shim_pa, ret);
	return ret;
}
