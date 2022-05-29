// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDEI client test driver
 *
 * Copyright (C) 2022 Red Hat, Inc.
 *
 * Author(s): Gavin Shan <gshan@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/arm-smccc.h>
#include <linux/acpi.h>
#include <linux/arm_sdei.h>
#include <linux/kprobes.h>

#define SDEI_EVENT_NUM		SDEI_SW_SIGNALED_EVENT
#define SDEI_EVENT_PARAM	0xdabfdabf

static asmlinkage void (*sdei_firmware_call)(unsigned long function_id,
			unsigned long arg0, unsigned long arg1,
			unsigned long arg2, unsigned long arg3,
			unsigned long arg4, struct arm_smccc_res *res);

static void sdei_smccc_smc(unsigned long function_id,
			   unsigned long arg0, unsigned long arg1,
			   unsigned long arg2, unsigned long arg3,
			   unsigned long arg4, struct arm_smccc_res *res)
{
	arm_smccc_smc(function_id, arg0, arg1, arg2, arg3, arg4, 0, 0, res);
}
NOKPROBE_SYMBOL(sdei_smccc_smc);

static void sdei_smccc_hvc(unsigned long function_id,
			   unsigned long arg0, unsigned long arg1,
			   unsigned long arg2, unsigned long arg3,
			   unsigned long arg4, struct arm_smccc_res *res)
{
	arm_smccc_hvc(function_id, arg0, arg1, arg2, arg3, arg4, 0, 0, res);
}
NOKPROBE_SYMBOL(sdei_smccc_hvc);

static int sdei_to_linux_errno(unsigned long sdei_err)
{
	switch (sdei_err) {
	case SDEI_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case SDEI_INVALID_PARAMETERS:
		return -EINVAL;
	case SDEI_DENIED:
		return -EPERM;
	case SDEI_PENDING:
		return -EINPROGRESS;
	case SDEI_OUT_OF_RESOURCE:
		return -ENOMEM;
	}

	return 0;
}

static int invoke_sdei_fn(unsigned long function_id, unsigned long arg0,
			  unsigned long arg1, unsigned long arg2,
			  unsigned long arg3, unsigned long arg4,
			  u64 *result)
{
	struct arm_smccc_res res;
	int ret = 0;

	if (sdei_firmware_call) {
		sdei_firmware_call(function_id, arg0, arg1,
				   arg2, arg3, arg4, &res);
		ret = sdei_to_linux_errno(res.a0);
	} else {
		/*
		 * !sdei_firmware_call means we failed to probe or called
		 * sdei_mark_interface_broken(). -EIO is not an returned
		 * by sdei_to_linux_errno() and is used to suppress messages
		 * from this driver.
		 */
		ret = -EIO;
		res.a0 = SDEI_NOT_SUPPORTED;
	}

	if (result)
		*result = res.a0;

	return ret;
}
NOKPROBE_SYMBOL(invoke_sdei_fn);

static int sdei_test_handler(u32 num, struct pt_regs *regs, void *arg)
{
	u64 ctxt[18];
	int i;

	pr_info("=========== SDEI Event (CPU#%d) ===========\n",
		smp_processor_id());
	pr_info("Event:   %016lx   Parameter:   %016lx\n"
		"PC:      %016lx   PSTATE:      %016lx   SP:   %016lx\n",
		(unsigned long)num, (unsigned long)arg,
		(unsigned long)regs->pc, (unsigned long)regs->pstate,
		(unsigned long)regs->sp);

	pr_info("Regs:    %016llx %016llx %016llx %016llx\n",
		regs->regs[0], regs->regs[1], regs->regs[2], regs->regs[3]);
	pr_info("         %016llx %016llx %016llx %016llx\n",
		regs->regs[4], regs->regs[5], regs->regs[6], regs->regs[7]);
	pr_info("         %016llx %016llx %016llx %016llx\n",
		regs->regs[8], regs->regs[9], regs->regs[10], regs->regs[11]);
	pr_info("         %016llx %016llx %016llx %016llx\n",
		regs->regs[12], regs->regs[13], regs->regs[14], regs->regs[15]);
	pr_info("         %016llx %016llx %016llx %016llx\n",
		regs->regs[16], regs->regs[17], regs->regs[18], regs->regs[19]);
	pr_info("         %016llx %016llx %016llx %016llx\n",
		regs->regs[20], regs->regs[21], regs->regs[22], regs->regs[23]);
	pr_info("         %016llx %016llx %016llx %016llx\n",
		regs->regs[24], regs->regs[25], regs->regs[26], regs->regs[27]);
	pr_info("         %016llx %016llx %016llx\n",
		regs->regs[28], regs->regs[29], regs->regs[30]);

	for (i = 0; i < ARRAY_SIZE(ctxt); i++) {
		invoke_sdei_fn(SDEI_1_0_FN_SDEI_EVENT_CONTEXT,
			       i, 0, 0, 0, 0, &ctxt[i]);
	}

	pr_info("Context: %016llx %016llx %016llx %016llx\n",
		ctxt[0], ctxt[1], ctxt[2], ctxt[3]);
	pr_info("         %016llx %016llx %016llx %016llx\n",
		ctxt[4], ctxt[5], ctxt[6], ctxt[7]);
	pr_info("         %016llx %016llx %016llx %016llx\n",
		ctxt[8], ctxt[9], ctxt[10], ctxt[11]);
	pr_info("         %016llx %016llx %016llx %016llx\n",
		ctxt[12], ctxt[13], ctxt[14], ctxt[15]);
	pr_info("         %016llx %016llx\n",
		ctxt[16], ctxt[17]);

	pr_info("\n");

	return 0;
}

static int __init check_version(void)
{
	u64 version;
	int ret;

	ret = invoke_sdei_fn(SDEI_1_0_FN_SDEI_VERSION, 0, 0, 0, 0, 0, &version);
	if (ret) {
		pr_warn("%s: Error %d to get version\n", __func__, ret);
		return ret;
	}

	pr_info("SDEI TEST: Version %d.%d, Vendor 0x%x\n",
		(int)SDEI_VERSION_MAJOR(version),
		(int)SDEI_VERSION_MINOR(version),
		(int)SDEI_VERSION_VENDOR(version));

	return 0;
}

static int __init sdei_test_init(void)
{
	int ret;

	/* Callback */
	if (acpi_disabled) {
		pr_warn("%s: ACPI disabled\n", __func__);
		return -EPERM;
	}

	sdei_firmware_call = acpi_psci_use_hvc() ?
			     &sdei_smccc_hvc : &sdei_smccc_smc;

	/* Check version */
	ret = check_version();
	if (ret)
		return ret;

	/* Register event */
	ret = sdei_event_register(SDEI_EVENT_NUM, sdei_test_handler,
				  (void *)SDEI_EVENT_PARAM);
	if (ret) {
		pr_warn("%s: Error %d to register event 0x%x\n",
			__func__, ret, SDEI_EVENT_NUM);
		return -EIO;
	}

	/* Enable event */
	pr_info("%s: SDEI event (0x%x) registered\n",
		__func__, SDEI_EVENT_NUM);
	ret = sdei_event_enable(SDEI_EVENT_NUM);
	if (ret) {
		sdei_event_unregister(SDEI_EVENT_NUM);
		pr_warn("%s: Error %d to enable event\n", __func__, ret);
		return ret;
	}

	pr_info("%s: SDEI event (0x%x) enabled\n",
		__func__, SDEI_EVENT_NUM);
	return 0;
}

static void __exit sdei_test_exit(void)
{
	sdei_event_unregister(SDEI_EVENT_NUM);
}

module_init(sdei_test_init);
module_exit(sdei_test_exit);
