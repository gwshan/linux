/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM_KVM_PARA_H
#define _ASM_ARM_KVM_PARA_H

#include <uapi/asm/kvm_para.h>
#include <linux/of.h>
#include <asm/hypervisor.h>

static inline bool kvm_check_and_clear_guest_paused(void)
{
	return false;
}

static inline unsigned int kvm_arch_para_features(void)
{
	unsigned int features = 0;

	if (kvm_arm_hyp_service_available(ARM_SMCCC_KVM_FUNC_ASYNC_PF))
		features |= (1 << KVM_FEATURE_ASYNC_PF);

	return features;
}

static inline unsigned int kvm_arch_para_hints(void)
{
	return 0;
}

static inline bool kvm_para_available(void)
{
	if (IS_ENABLED(CONFIG_KVM_GUEST))
		return true;

	return false;
}

#endif /* _ASM_ARM_KVM_PARA_H */
