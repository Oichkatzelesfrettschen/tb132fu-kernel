// SPDX-License-Identifier: GPL-2.0
/*
 * GenieZone gzvm host-interface probe.
 *
 * gzvm, the GenieZone VM-management interface used by crosvm, is reached
 * through HVC fast calls in the SMCCC owner-59 range starting at function
 * 0x1000; function 12 (GZVM_FUNC_PROBE) returns 0 when the hypervisor
 * implements that interface. The probe issues four read-only HVCs once at
 * boot:
 *   uid  - SMCCC vendor-hypervisor Call UID query;
 *   ctl  - an owner-59 fast64 function no gzvm revision assigns (0x10ff),
 *          the negative control for the reply to an unknown call;
 *   hvc  - GZVM_FUNC_PROBE carrying driver version 16.0.0 in x1..x3, the
 *          ACK android15-6.6 driver's arguments;
 *   v0   - GZVM_FUNC_PROBE with zero arguments, the older driver's form.
 *
 * Each result is logged and becomes the name of a misc device, so an
 * unprivileged shell reads it from /sys/class/misc.
 */

#include <linux/arm-smccc.h>
#include <linux/compiler.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <asm/insn.h>
#include <asm/ptrace.h>
#include <asm/traps.h>

#define GZVM_PROBE_SMC_ENTITY_MTK	59
#define GZVM_PROBE_FUNCID_START		0x1000
#define GZVM_PROBE_FUNC_PROBE		12
#define GZVM_PROBE_FUNC_UNASSIGNED	0xff

#define GZVM_PROBE_HCALL_ID(func)					\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_64,	\
			   GZVM_PROBE_SMC_ENTITY_MTK,			\
			   GZVM_PROBE_FUNCID_START + (func))

/*
 * SMCCC owner 6 is the Vendor Specific Hypervisor Service range; function
 * 0xff01 in every service range is the Call UID query.
 */
#define GZVM_PROBE_OWNER_VENDOR_HYP	6
#define GZVM_PROBE_HVC_VENDOR_HYP_UID					\
	ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, ARM_SMCCC_SMC_32,	\
			   GZVM_PROBE_OWNER_VENDOR_HYP, 0xff01)

/* GZVM_DRV_MAJOR_VERSION and GZVM_DRV_MINOR_VERSION of the ACK driver. */
#define GZVM_PROBE_DRV_MAJOR		16
#define GZVM_PROBE_DRV_MINOR		0

/* A64 encoding of "hvc #0", the instruction __arm_smccc_hvc issues. */
#define GZVM_PROBE_HVC0_INSN		0xd4000002U

#define GZVM_PROBE_NAME_LEN		64

static bool gzvm_probe_in_flight;
static bool gzvm_probe_trapped_undef;

/*
 * HVC executed at EL1 is UNDEFINED when EL2 sets HCR_EL2.HCD or injects an
 * undefined-instruction exception for an unhandled call. The hook skips
 * the HVC so a hypervisor that refuses the call yields a recorded result
 * rather than a kernel oops. It acts only while a probe call is in flight.
 */
static int gzvm_probe_undef(struct pt_regs *regs, u32 instr)
{
	if (!READ_ONCE(gzvm_probe_in_flight))
		return 1;

	WRITE_ONCE(gzvm_probe_trapped_undef, true);
	regs->regs[0] = SMCCC_RET_NOT_SUPPORTED;
	regs->regs[1] = 0;
	regs->regs[2] = 0;
	regs->regs[3] = 0;
	arm64_skip_faulting_instruction(regs, AARCH64_INSN_SIZE);
	return 0;
}

static struct undef_hook gzvm_probe_hook = {
	.instr_mask	= 0xffffffffU,
	.instr_val	= GZVM_PROBE_HVC0_INSN,
	.pstate_mask	= PSR_MODE_MASK,
	.pstate_val	= PSR_MODE_EL1h,
	.fn		= gzvm_probe_undef,
};

struct gzvm_probe_call {
	const char *tag;
	unsigned long fid;
	unsigned long a1, a2, a3;
	bool uid_form;
	bool undef;
	struct arm_smccc_res res;
	char name[GZVM_PROBE_NAME_LEN];
	struct miscdevice dev;
};

static struct gzvm_probe_call gzvm_probe_calls[] = {
	{ .tag = "uid", .fid = GZVM_PROBE_HVC_VENDOR_HYP_UID,
	  .uid_form = true },
	{ .tag = "ctl",
	  .fid = GZVM_PROBE_HCALL_ID(GZVM_PROBE_FUNC_UNASSIGNED) },
	{ .tag = "hvc", .fid = GZVM_PROBE_HCALL_ID(GZVM_PROBE_FUNC_PROBE),
	  .a1 = GZVM_PROBE_DRV_MAJOR, .a2 = GZVM_PROBE_DRV_MINOR },
	{ .tag = "v0", .fid = GZVM_PROBE_HCALL_ID(GZVM_PROBE_FUNC_PROBE) },
};

static void gzvm_probe_issue(struct gzvm_probe_call *c)
{
	pr_info("gzvm-probe: %s fid=0x%08lx a1=0x%lx a2=0x%lx a3=0x%lx issuing\n",
		c->tag, c->fid, c->a1, c->a2, c->a3);

	preempt_disable();
	WRITE_ONCE(gzvm_probe_trapped_undef, false);
	WRITE_ONCE(gzvm_probe_in_flight, true);
	arm_smccc_hvc(c->fid, c->a1, c->a2, c->a3, 0, 0, 0, 0, &c->res);
	WRITE_ONCE(gzvm_probe_in_flight, false);
	c->undef = READ_ONCE(gzvm_probe_trapped_undef);
	preempt_enable();

	pr_info("gzvm-probe: %s fid=0x%08lx undef=%d x0=0x%lx x1=0x%lx x2=0x%lx x3=0x%lx\n",
		c->tag, c->fid, c->undef, c->res.a0, c->res.a1, c->res.a2,
		c->res.a3);
}

static void gzvm_probe_name(struct gzvm_probe_call *c)
{
	const struct arm_smccc_res *r = &c->res;

	if (c->undef)
		snprintf(c->name, sizeof(c->name), "gzvm_probe_%s_undef",
			 c->tag);
	else if (c->uid_form)
		snprintf(c->name, sizeof(c->name),
			 "gzvm_probe_%s_%08x_%08x_%08x_%08x", c->tag,
			 (u32)r->a0, (u32)r->a1, (u32)r->a2, (u32)r->a3);
	else if (r->a0 == 0)
		snprintf(c->name, sizeof(c->name), "gzvm_probe_%s_ok", c->tag);
	else
		snprintf(c->name, sizeof(c->name), "gzvm_probe_%s_x0_%08x",
			 c->tag, (u32)r->a0);
}

static const struct file_operations gzvm_probe_fops = {
	.owner = THIS_MODULE,
};

static int __init gzvm_probe_init(void)
{
	size_t i;
	int ret;

	BUILD_BUG_ON(GZVM_PROBE_HCALL_ID(GZVM_PROBE_FUNC_PROBE) != 0xfb00100cU);
	BUILD_BUG_ON(GZVM_PROBE_HVC_VENDOR_HYP_UID != 0x8600ff01U);

	register_undef_hook(&gzvm_probe_hook);
	for (i = 0; i < ARRAY_SIZE(gzvm_probe_calls); i++)
		gzvm_probe_issue(&gzvm_probe_calls[i]);
	unregister_undef_hook(&gzvm_probe_hook);

	for (i = 0; i < ARRAY_SIZE(gzvm_probe_calls); i++) {
		struct gzvm_probe_call *c = &gzvm_probe_calls[i];

		gzvm_probe_name(c);
		pr_info("gzvm-probe: result %s\n", c->name);

		c->dev.minor = MISC_DYNAMIC_MINOR;
		c->dev.name = c->name;
		c->dev.fops = &gzvm_probe_fops;
		c->dev.mode = 0400;
		ret = misc_register(&c->dev);
		if (ret)
			pr_err("gzvm-probe: %s device registration failed: %d\n",
			       c->tag, ret);
	}
	return 0;
}
late_initcall(gzvm_probe_init);
