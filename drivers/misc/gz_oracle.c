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
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <asm/memory.h>
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

	if (__boot_cpu_mode[0] != BOOT_CPU_MODE_EL1) {
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


/*
 * Lending probe: HVC 0xEF000E13(x1 = physical entry, x2..x5 = arguments) runs
 * the entry at EL2 with the MMU off inside a patched GenieZone and returns
 * x0..x3. The call is made only on a write to /proc/gz_lend, in IPI context
 * with interrupts masked, because a faulting callee stops that CPU at EL2.
 * Input: "<cpu> <hex pa | probe> [a b c d]".
 */
#define GZ_LEND_ID	0xEF000E13UL

extern char gz_lend_probe[];
int gz_lend_arm(void);

struct gz_lend_req {
	unsigned long pa, a[4];
	struct arm_smccc_res res;
};

static struct gz_lend_req gz_lend_last;
static int gz_lend_last_cpu = -1;
static DEFINE_MUTEX(gz_lend_lock);

static void gz_lend_call(void *info)
{
	struct gz_lend_req *r = info;

	arm_smccc_hvc(GZ_LEND_ID, r->pa, r->a[0], r->a[1], r->a[2], r->a[3],
		      0, 0, &r->res);
}

static ssize_t gz_lend_write(struct file *file, const char __user *ubuf,
			     size_t len, loff_t *ppos)
{
	char buf[128], tok[24];
	struct gz_lend_req r = { 0 };
	int cpu, n = 0, ret;

	if (__boot_cpu_mode[0] != BOOT_CPU_MODE_EL1)
		return -ENODEV;
	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	if (!strncmp(buf, "arm", 3)) {
		int err = gz_lend_arm();

		return err ? err : len;
	}
	if (sscanf(buf, "%d %23s %lx %lx %lx %lx%n", &cpu, tok, &r.a[0],
		   &r.a[1], &r.a[2], &r.a[3], &n) < 2)
		return -EINVAL;
	if (!strcmp(tok, "probe"))
		r.pa = __pa_symbol(gz_lend_probe);
	else if (kstrtoul(tok, 16, &r.pa))
		return -EINVAL;
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu))
		return -EINVAL;

	mutex_lock(&gz_lend_lock);
	pr_info("gz_lend: cpu%d pa=%lx args %lx %lx %lx %lx\n", cpu, r.pa,
		r.a[0], r.a[1], r.a[2], r.a[3]);
	ret = smp_call_function_single(cpu, gz_lend_call, &r, 1);
	pr_info("gz_lend: cpu%d ret=%d %016lx %016lx %016lx %016lx\n", cpu, ret,
		r.res.a0, r.res.a1, r.res.a2, r.res.a3);
	gz_lend_last = r;
	gz_lend_last_cpu = cpu;
	mutex_unlock(&gz_lend_lock);
	return len;
}

static int gz_lend_show(struct seq_file *m, void *v)
{
	seq_printf(m, "probe_pa %lx\n", (unsigned long)__pa_symbol(gz_lend_probe));
	seq_printf(m, "last cpu%d pa=%lx %016lx %016lx %016lx %016lx\n",
		   gz_lend_last_cpu, gz_lend_last.pa, gz_lend_last.res.a0,
		   gz_lend_last.res.a1, gz_lend_last.res.a2, gz_lend_last.res.a3);
	return 0;
}

static int gz_lend_open(struct inode *inode, struct file *file)
{
	return single_open(file, gz_lend_show, NULL);
}

static const struct file_operations gz_lend_fops = {
	.open = gz_lend_open,
	.read = seq_read,
	.write = gz_lend_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init gz_oracle_init(void)
{
	proc_create_single("gz_oracle", 0400, NULL, gz_oracle_show);
	proc_create("gz_lend", 0600, NULL, &gz_lend_fops);
	return 0;
}
late_initcall(gz_oracle_init);
