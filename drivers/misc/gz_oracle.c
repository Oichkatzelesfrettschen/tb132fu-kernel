// SPDX-License-Identifier: GPL-2.0
/*
 * Reader for the EL2 register oracle of a patched GenieZone image.
 *
 * The oracle answers SMCCC fast call 0xEF000E12 issued with HVC from EL1 and
 * returns four EL2 system registers per selector. An unpatched hypervisor
 * returns 0xffffffff in x0 without the signature. The call is issued only
 * when the kernel was entered at EL1: at EL2 an HVC from the kernel targets
 * the kernel's own vectors.
 */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <asm/virt.h>

#define GZ_ORACLE_ID	0xEF000E12UL
#define GZ_ORACLE_NSEL	4

struct gz_oracle_read {
	struct arm_smccc_res res[GZ_ORACLE_NSEL];
};

static void gz_oracle_call(void *info)
{
	struct gz_oracle_read *r = info;
	int sel;

	for (sel = 0; sel < GZ_ORACLE_NSEL; sel++)
		arm_smccc_hvc(GZ_ORACLE_ID, sel, 0, 0, 0, 0, 0, 0, &r->res[sel]);
}

static int gz_oracle_show(struct seq_file *m, void *v)
{
	struct gz_oracle_read r;
	int cpu, sel;

	if (is_hyp_mode_available() || is_kernel_in_hyp_mode()) {
		seq_puts(m, "kernel entered at EL2: no hypervisor below it\n");
		return 0;
	}

	for_each_online_cpu(cpu) {
		memset(&r, 0, sizeof(r));
		if (smp_call_function_single(cpu, gz_oracle_call, &r, 1))
			continue;
		for (sel = 0; sel < GZ_ORACLE_NSEL; sel++)
			seq_printf(m, "cpu%d sel%d %016lx %016lx %016lx %016lx\n",
				   cpu, sel, r.res[sel].a0, r.res[sel].a1,
				   r.res[sel].a2, r.res[sel].a3);
	}
	return 0;
}

static int __init gz_oracle_init(void)
{
	proc_create_single("gz_oracle", 0400, NULL, gz_oracle_show);
	return 0;
}
late_initcall(gz_oracle_init);
