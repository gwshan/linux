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

static void event_complete(struct kvm_vcpu *vcpu, bool resume)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	struct kvm_sdei_event_context *ctxt = &vsdei->ctxt;
	unsigned long pstate, resume_addr = smccc_get_arg(vcpu, 1);
	unsigned int num, i;

	num = find_next_bit(&vsdei->running, KVM_NR_SDEI_EVENTS, 0);
	if (num >= KVM_NR_SDEI_EVENTS)
		return;

	/* Restore registers: x0 -> x17 */
	for (i = 0; i < ARRAY_SIZE(ctxt->regs); i++)
		vcpu_set_reg(vcpu, i, ctxt->regs[i]);

	/*
	 * The registers are modified accordingly if the execution resumes
	 * from the specified address.
	 *
	 * SPSR_EL1: PSTATE of the interrupted context
	 * ELR_EL1:  PC of the interrupted context
	 * PSTATE:   cleared nRW bit, but D/A/I/F bits are set
	 * PC:       the resume address
	 */
	if (resume) {
		if (has_vhe()) {
			write_sysreg_el1(ctxt->pstate, SYS_SPSR);
			write_sysreg_s(ctxt->pc, SYS_ELR_EL12);
		} else {
			__vcpu_sys_reg(vcpu, SPSR_EL1) = ctxt->pstate;
			__vcpu_sys_reg(vcpu, ELR_EL1) = ctxt->pc;
		}

		pstate = ctxt->pstate;
		pstate &= ~(PSR_MODE32_BIT | PSR_MODE_MASK);
		pstate |= (PSR_D_BIT | PSR_A_BIT | PSR_I_BIT |
			   PSR_F_BIT | PSR_MODE_EL1h);
		*vcpu_cpsr(vcpu) = pstate;
		*vcpu_pc(vcpu) = resume_addr;
	} else {
		*vcpu_cpsr(vcpu) = ctxt->pstate;
		*vcpu_pc(vcpu) = ctxt->pc;
	}

	/* Update event state */
	clear_bit(num, &vsdei->running);
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

static unsigned long event_status(struct kvm_vcpu *vcpu)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	unsigned int num = smccc_get_arg(vcpu, 1);
	unsigned long ret = 0;

	if (num >= KVM_NR_SDEI_EVENTS)
		return SDEI_INVALID_PARAMETERS;

	if (test_bit(num, &vsdei->registered))
		ret |= (1UL << SDEI_EVENT_STATUS_REGISTERED);
	if (test_bit(num, &vsdei->enabled))
		ret |= (1UL << SDEI_EVENT_STATUS_ENABLED);
	if (test_bit(num, &vsdei->running))
		ret |= (1UL << SDEI_EVENT_STATUS_RUNNING);

	return ret;
}

static unsigned long event_info(struct kvm_vcpu *vcpu)
{
	unsigned int num = smccc_get_arg(vcpu, 1);
	unsigned int info = smccc_get_arg(vcpu, 2);
	unsigned long ret = 0;

	if (num >= KVM_NR_SDEI_EVENTS)
		return SDEI_INVALID_PARAMETERS;

	/*
	 * All supported events are private and have normal priority.
	 * Besides, all supported events can be signaled by software
	 */
	switch (info) {
	case SDEI_EVENT_INFO_EV_TYPE:
		ret = SDEI_EVENT_TYPE_PRIVATE;
		break;
	case SDEI_EVENT_INFO_EV_SIGNALED:
		ret = 1;
		break;
	case SDEI_EVENT_INFO_EV_PRIORITY:
		ret = SDEI_EVENT_PRIORITY_NORMAL;
		break;
	case SDEI_EVENT_INFO_EV_ROUTING_MODE:
	case SDEI_EVENT_INFO_EV_ROUTING_AFF:
	default:
		ret = SDEI_INVALID_PARAMETERS;
	}

	return ret;
}

static unsigned long pe_mask(struct kvm_vcpu *vcpu, bool mask)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;

	if (mask) {
		vcpu->arch.flags |= KVM_ARM64_SDEI_MASKED;
	} else {
		vcpu->arch.flags &= ~KVM_ARM64_SDEI_MASKED;
		if (vsdei->pending)
			kvm_make_request(KVM_REQ_SDEI, vcpu);
	}

	return SDEI_SUCCESS;
}

static unsigned long event_reset(struct kvm_vcpu *vcpu, bool private)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	unsigned int num;

	/*
	 * Nothing to do if we're going to reset the shared events,
	 * which are unsupported.
	 */
	if (!private)
		return SDEI_SUCCESS;

	for (num = 0; num < KVM_NR_SDEI_EVENTS; num++) {
		clear_bit(num, &vsdei->registered);
		clear_bit(num, &vsdei->enabled);
	}

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
	case SDEI_1_0_FN_SDEI_EVENT_COMPLETE:
		event_complete(vcpu, false);
		break;
	case SDEI_1_0_FN_SDEI_EVENT_COMPLETE_AND_RESUME:
		event_complete(vcpu, true);
		break;
	case SDEI_1_0_FN_SDEI_EVENT_UNREGISTER:
		ret = event_unregister(vcpu);
		break;
	case SDEI_1_0_FN_SDEI_EVENT_STATUS:
		ret = event_status(vcpu);
		break;
	case SDEI_1_0_FN_SDEI_EVENT_GET_INFO:
		ret = event_info(vcpu);
		break;
	case SDEI_1_0_FN_SDEI_PE_MASK:
		ret = pe_mask(vcpu, true);
		break;
	case SDEI_1_0_FN_SDEI_PE_UNMASK:
		ret = pe_mask(vcpu, false);
		break;
	case SDEI_1_0_FN_SDEI_PRIVATE_RESET:
		ret = event_reset(vcpu, true);
		break;
	case SDEI_1_0_FN_SDEI_SHARED_RESET:
		ret = event_reset(vcpu, false);
		break;
	default:
		ret = SDEI_NOT_SUPPORTED;
	}

out:
	if (has_result)
		smccc_set_retval(vcpu, ret, 0, 0, 0);

	return 1;
}

int kvm_sdei_inject_event(struct kvm_vcpu *vcpu,
			  unsigned int num,
			  bool immediate)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;

	if (!vsdei)
		return -EPERM;

	if (num >= KVM_NR_SDEI_EVENTS || !test_bit(num, &vsdei->registered))
		return -ENOENT;

	/*
	 * The event may be expected to be delivered immediately. There
	 * are several cases we can't do this:
	 *
	 * (1) The PE has been masked from any events.
	 * (2) The event isn't enabled yet.
	 * (3) There are any pending or running events.
	 */
	if (immediate &&
	    ((vcpu->arch.flags & KVM_ARM64_SDEI_MASKED) ||
	    !test_bit(num, &vsdei->enabled) ||
	    vsdei->pending || vsdei->running))
		return -EBUSY;

	set_bit(num, &vsdei->pending);
	if (!(vcpu->arch.flags & KVM_ARM64_SDEI_MASKED) &&
	    test_bit(num, &vsdei->enabled))
		kvm_make_request(KVM_REQ_SDEI, vcpu);

	return 0;
}

int kvm_sdei_cancel_event(struct kvm_vcpu *vcpu, unsigned int num)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;

	if (!vsdei)
		return -EPERM;

	if (num >= KVM_NR_SDEI_EVENTS || !test_bit(num, &vsdei->registered))
		return -ENOENT;

	if (test_bit(num, &vsdei->running))
		return -EBUSY;

	clear_bit(num, &vsdei->pending);

	return 0;
}

void kvm_sdei_deliver_event(struct kvm_vcpu *vcpu)
{
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	struct kvm_sdei_event_context *ctxt = &vsdei->ctxt;
	unsigned int num, i;
	unsigned long pstate;

	if (!vsdei || (vcpu->arch.flags & KVM_ARM64_SDEI_MASKED))
		return;

	/*
	 * All supported events have normal priority. So the currently
	 * running event can't be preempted by any one else.
	 */
	if (vsdei->running)
		return;

	/* Select next pending event to be delivered */
	num = 0;
	while (num < KVM_NR_SDEI_EVENTS) {
		num = find_next_bit(&vsdei->pending, KVM_NR_SDEI_EVENTS, num);
		if (test_bit(num, &vsdei->enabled))
			break;
	}

	if (num >= KVM_NR_SDEI_EVENTS)
		return;

	/*
	 * Save the interrupted context. We might have pending request
	 * to adjust PC. Lets adjust it now so that the resume address
	 * is correct when COMPLETE or COMPLETE_AND_RESUME hypercall
	 * is handled.
	 */
	__kvm_adjust_pc(vcpu);
	ctxt->pc = *vcpu_pc(vcpu);
	ctxt->pstate = *vcpu_cpsr(vcpu);
	for (i = 0; i < ARRAY_SIZE(ctxt->regs); i++)
		ctxt->regs[i] = vcpu_get_reg(vcpu, i);

	/*
	 * Inject event. The following registers are modified according
	 * to the specification.
	 *
	 * x0: event number
	 * x1: argument specified when the event is registered
	 * x2: PC of the interrupted context
	 * x3: PSTATE of the interrupted context
	 * PC: event handler
	 * PSTATE: Cleared nRW bit, but D/A/I/F bits are set
	 */
	for (i = 0; i < ARRAY_SIZE(ctxt->regs); i++)
		vcpu_set_reg(vcpu, i, 0);

	vcpu_set_reg(vcpu, 0, num);
	vcpu_set_reg(vcpu, 1, vsdei->handlers[num].ep_arg);
	vcpu_set_reg(vcpu, 2, ctxt->pc);
	vcpu_set_reg(vcpu, 3, ctxt->pstate);

	pstate = ctxt->pstate;
	pstate &= ~(PSR_MODE32_BIT | PSR_MODE_MASK);
	pstate |= (PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT | PSR_MODE_EL1h);

	*vcpu_cpsr(vcpu) = pstate;
	*vcpu_pc(vcpu) = vsdei->handlers[num].ep_addr;

	/* Update event states */
	clear_bit(num, &vsdei->pending);
	set_bit(num, &vsdei->running);
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
