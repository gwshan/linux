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

static unsigned long event_register(struct kvm_vcpu *vcpu)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	unsigned int num = smccc_get_arg(vcpu, 1);
	unsigned long flags = smccc_get_arg(vcpu, 4);

	if (num >= KVM_NR_SDEI_EVENTS)
		return SDEI_INVALID_PARAMETERS;

	/* Reject if the reserved bits or relative mode are set */
	if (flags & ~0x1UL)
		return SDEI_INVALID_PARAMETERS;

	/*
	 * Reject if the event has been registered or pending for
	 * unregistration.
	 */
	if (test_bit(num, &vsdei->registered) ||
	    test_bit(num, &vsdei->running))
		return SDEI_DENIED;

	vsdei->handlers[num].ep_addr = smccc_get_arg(vcpu, 2);
	vsdei->handlers[num].ep_arg = smccc_get_arg(vcpu, 3);
	set_bit(num, &vsdei->registered);

	return SDEI_SUCCESS;
}

static unsigned long event_enable(struct kvm_vcpu *vcpu, bool enable)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	int num = smccc_get_arg(vcpu, 1);

	if (num >= KVM_NR_SDEI_EVENTS)
		return SDEI_INVALID_PARAMETERS;

	/*
	 * Reject if the event hasn't been registered or pending
	 * for unregistration.
	 */
	if (!test_bit(num, &vsdei->registered))
		return SDEI_DENIED;

	if (enable) {
		set_bit(num, &vsdei->enabled);
		if (!(vcpu->arch.flags & KVM_ARM64_SDEI_MASKED) &&
		    test_bit(num, &vsdei->pending))
			kvm_make_request(KVM_REQ_SDEI, vcpu);
	} else {
		clear_bit(num, &vsdei->enabled);
	}

	return SDEI_SUCCESS;
}

static unsigned long event_context(struct kvm_vcpu *vcpu)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	struct kvm_sdei_event_context *ctxt = &vsdei->ctxt;
	unsigned int param_id = smccc_get_arg(vcpu, 1);

	/* Reject if event handler isn't running */
	if (!vsdei->running)
		return SDEI_DENIED;

	/* Reject if the parameter ID is out of range */
	if (param_id >= ARRAY_SIZE(ctxt->regs))
		return SDEI_INVALID_PARAMETERS;

	return ctxt->regs[param_id];
}

static unsigned long event_unregister(struct kvm_vcpu *vcpu)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	unsigned int num = smccc_get_arg(vcpu, 1);

	if (num >= KVM_NR_SDEI_EVENTS)
		return SDEI_INVALID_PARAMETERS;

	/*
	 * Reject if the event isn't registered. It's allowed to
	 * unregister event which has been pending for that.
	 */
	if (!test_bit(num, &vsdei->registered)) {
		if (test_bit(num, &vsdei->running))
			return SDEI_PENDING;
		else
			return SDEI_DENIED;
	}

	/*
	 * The event is disabled automatically on unregistration, even
	 * pending for that.
	 */
	clear_bit(num, &vsdei->enabled);
	clear_bit(num, &vsdei->registered);

	/* Pending for unreigstration if the event handler is running */
	if (test_bit(num, &vsdei->running))
		return SDEI_PENDING;

	return SDEI_SUCCESS;
}

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
	case SDEI_1_0_FN_SDEI_EVENT_REGISTER:
		ret = event_register(vcpu);
		break;
	case SDEI_1_0_FN_SDEI_EVENT_ENABLE:
		ret = event_enable(vcpu, true);
		break;
	case SDEI_1_0_FN_SDEI_EVENT_DISABLE:
		ret = event_enable(vcpu, false);
		break;
	case SDEI_1_0_FN_SDEI_EVENT_CONTEXT:
		ret = event_context(vcpu);
		break;
	case SDEI_1_0_FN_SDEI_EVENT_UNREGISTER:
		ret = event_unregister(vcpu);
		break;
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
