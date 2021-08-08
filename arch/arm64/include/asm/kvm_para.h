/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM_KVM_PARA_H
#define _ASM_ARM_KVM_PARA_H

#include <uapi/asm/kvm_para.h>

static inline bool kvm_check_and_clear_guest_paused(void)
{
	return false;
}

static inline unsigned int kvm_arch_para_features(void)
{
	return 0;
}

static inline unsigned int kvm_arch_para_hints(void)
{
	return 0;
}

static inline bool kvm_para_available(void)
{
	return false;
}

#endif /* _ASM_ARM_KVM_PARA_H */
