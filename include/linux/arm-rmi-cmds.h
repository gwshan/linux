/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 ARM Ltd.
 */

#ifndef __LINUX_ARM_RMI_CMDS_H_
#define __LINUX_ARM_RMI_CMDS_H_

#include <linux/arm-smccc-rmi.h>
#include <linux/bug.h>
#include <linux/gfp.h>
#include <linux/processor.h>
#include <linux/string.h>
#include <linux/types.h>


#define RMI_MAX_ADDR_LIST	256

struct rmi_sro_state {
	struct arm_smccc_1_2_regs regs;
	int addr_count;
	unsigned long addr_list[RMI_MAX_ADDR_LIST];
};

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

int rmi_delegate_range(phys_addr_t phys, unsigned long size,
		       phys_addr_t *out_phys);
int rmi_undelegate_range(phys_addr_t phys, unsigned long size);
int rmi_free_delegated_page(phys_addr_t phys);

static inline int rmi_delegate_page(phys_addr_t phys)
{
	return rmi_delegate_range(phys, PAGE_SIZE, NULL);
}

static inline int rmi_undelegate_page(phys_addr_t phys)
{
	return rmi_undelegate_range(phys, PAGE_SIZE);
}

bool is_rmi_available(void);

long rmi_sro_memxfer_execute(struct rmi_sro_state *sro, gfp_t gfp);
void rmi_sro_free(struct rmi_sro_state *sro);
long rmi_sro_execute(struct arm_smccc_1_2_regs *regs);

/*
 * Resetting the addr_count is sufficient to ignore the addr_list contents.
 */
#define rmi_sro_memxfer_cmd(sro, gfp, ...) ({				\
	struct rmi_sro_state *__sro = (sro);				\
	__sro->addr_count = 0;						\
	__sro->regs = (struct arm_smccc_1_2_regs){ __VA_ARGS__ };	\
	long __ret = rmi_sro_memxfer_execute(__sro, gfp);		\
	rmi_sro_free(__sro);						\
	__ret;								\
})

#endif
