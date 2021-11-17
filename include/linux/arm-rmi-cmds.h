/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#ifndef __LINUX_ARM_RMI_CMDS_H_
#define __LINUX_ARM_RMI_CMDS_H_

#include <linux/arm-smccc-rmi.h>
#include <linux/bug.h>
#include <linux/processor.h>
#include <linux/types.h>


/*
 * rmi_smccc_invoke: Invoke the RMI call and return the results
 * @regs: Input parameters filled in. Updated with the ouptput results
 * after the call.
 */
static inline void rmi_smccc_invoke(struct arm_smccc_1_2_regs *regs)
{
	struct arm_smccc_1_2_regs args = *regs;
	unsigned long status;

	while (1) {
		arm_smccc_1_2_invoke(&args, regs);
		status = RMI_RETURN_STATUS(regs->a0);
		if (status != RMI_BUSY && status != RMI_BLOCKED)
			break;
		cpu_relax();
	}
}

unsigned long rmi_feat_reg(unsigned long index);

#endif
