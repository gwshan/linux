// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDEI virtualization support.
 *
 * Copyright (C) 2022 Red Hat, Inc.
 *
 * Author(s): Gavin Shan <gshan@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/slab.h>
#include <kvm/arm_hypercalls.h>
#include <asm/kvm_sdei.h>

int kvm_sdei_call(struct kvm_vcpu *vcpu)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	u32 func = smccc_get_function(vcpu);
	bool has_result = true;
	unsigned long ret;

	/* No return value for COMPLETE or COMPLETE_AND_RESUME */
	if (func == SDEI_1_0_FN_SDEI_EVENT_COMPLETE ||
	    func == SDEI_1_0_FN_SDEI_EVENT_COMPLETE_AND_RESUME)
		has_result = false;

	if (!vsdei) {
		ret = SDEI_NOT_SUPPORTED;
		goto out;
	}

	switch (func) {
	default:
		ret = SDEI_NOT_SUPPORTED;
	}

out:
	if (has_result)
		smccc_set_retval(vcpu, ret, 0, 0, 0);

	return 1;
}

void kvm_sdei_create_vcpu(struct kvm_vcpu *vcpu)
{
	struct kvm_sdei_vcpu *vsdei;

	vcpu->arch.sdei = kzalloc(sizeof(*vsdei), GFP_KERNEL_ACCOUNT);
	if (vcpu->arch.sdei)
		vcpu->arch.flags |= KVM_ARM64_SDEI_MASKED;
}

void kvm_sdei_destroy_vcpu(struct kvm_vcpu *vcpu)
{
	vcpu->arch.flags &= ~KVM_ARM64_SDEI_MASKED;
	kfree(vcpu->arch.sdei);
	vcpu->arch.sdei = NULL;
}
