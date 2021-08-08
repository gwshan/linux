// SPDX-License-Identifier: GPL-2.0-only
/*
 * Asynchronous page fault support.
 *
 * Copyright (C) 2021 Red Hat, Inc.
 *
 * Author(s): Gavin Shan <gshan@redhat.com>
 */

#include <linux/arm-smccc.h>
#include <linux/kvm_host.h>
#include <asm/kvm_emulate.h>
#include <kvm/arm_hypercalls.h>
#include <kvm/arm_vgic.h>
#include <asm/kvm_sdei.h>

static inline int read_cache(struct kvm_vcpu *vcpu, u32 offset, u32 *val)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;

	return kvm_read_guest_offset_cached(kvm, &apf->cache,
					    val, offset, sizeof(*val));
}

static inline int write_cache(struct kvm_vcpu *vcpu, u32 offset, u32 val)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;

	return kvm_write_guest_offset_cached(kvm, &apf->cache,
					     &val, offset, sizeof(val));
}

void kvm_arch_async_pf_create_vcpu(struct kvm_vcpu *vcpu)
{
	vcpu->arch.apf = kzalloc(sizeof(*(vcpu->arch.apf)), GFP_KERNEL);
}

bool kvm_arch_async_not_present_allowed(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;
	struct kvm_sdei_vcpu *vsdei = vcpu->arch.sdei;
	u32 reason, token;
	int ret;

	if (!apf || !(apf->control_block & KVM_ASYNC_PF_ENABLED))
		return false;

	if (apf->send_user_only && vcpu_mode_priv(vcpu))
		return false;

	if (!irqchip_in_kernel(vcpu->kvm))
		return false;

	if (!vsdei || vsdei->critical_event || vsdei->normal_event)
		return false;

	/* Pending page fault, which isn't acknowledged by guest */
	ret = read_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, reason),
			 &reason);
	if (ret) {
		kvm_err("%s: Error %d to read reason (%d-%d)\n",
			__func__, ret, kvm->userspace_pid, vcpu->vcpu_idx);
		return false;
	}

	ret = read_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, token),
			 &token);
	if (ret) {
		kvm_err("%s: Error %d to read token %d-%d\n",
			__func__, ret, kvm->userspace_pid, vcpu->vcpu_idx);
		return false;
	}

	if (reason || token)
		return false;

	return true;
}

bool kvm_arch_setup_async_pf(struct kvm_vcpu *vcpu,
			     u32 esr, gpa_t gpa, gfn_t gfn)
{
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;
	struct kvm_arch_async_pf arch;
	unsigned long hva = kvm_vcpu_gfn_to_hva(vcpu, gfn);

	arch.token = (apf->id++ << 12) | vcpu->vcpu_id;
	arch.gfn = gfn;
	arch.esr = esr;

	return kvm_setup_async_pf(vcpu, gpa, hva, &arch);
}

/*
 * It's guaranteed that no pending asynchronous page fault when this is
 * called. It means all previous issued asynchronous page faults have
 * been acknowledged.
 */
bool kvm_arch_async_page_not_present(struct kvm_vcpu *vcpu,
				     struct kvm_async_pf *work)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;
	int ret;

	kvm_async_pf_add_slot(vcpu, work->arch.gfn);

	ret = write_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, token),
			  work->arch.token);
	if (ret) {
		kvm_err("%s: Error %d to write token (%d-%d %08x)\n",
			__func__, ret, kvm->userspace_pid,
			vcpu->vcpu_idx, work->arch.token);
		goto fail;
	}

	ret = write_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, reason),
			  KVM_PV_REASON_PAGE_NOT_PRESENT);
	if (ret) {
		kvm_err("%s: Error %d to write reason (%d-%d %08x)\n",
			__func__, ret, kvm->userspace_pid,
			vcpu->vcpu_idx, work->arch.token);
		goto fail;
	}

	apf->notpresent_pending = true;
	apf->notpresent_token = work->arch.token;

	return !kvm_sdei_inject(vcpu, apf->sdei_event_num, true);

fail:
	write_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, token), 0);
	write_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, reason), 0);
	kvm_async_pf_remove_slot(vcpu, work->arch.gfn);
	return false;
}

void kvm_arch_async_page_present_queued(struct kvm_vcpu *vcpu)
{
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;

	kvm_make_request(KVM_REQ_ASYNC_PF, vcpu);
	if (apf && !apf->pageready_pending)
		kvm_vcpu_kick(vcpu);
}

bool kvm_arch_can_dequeue_async_page_present(struct kvm_vcpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;
	struct kvm_async_pf *work;
	u32 reason, token;
	int ret;

	if (!apf || !(apf->control_block & KVM_ASYNC_PF_ENABLED))
		return true;

	if (apf->pageready_pending)
		goto fail;

	ret = read_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, reason),
			 &reason);
	if (ret) {
		kvm_err("%s: Error %d to read reason (%d-%d)\n",
			__func__, ret, kvm->userspace_pid, vcpu->vcpu_idx);
		goto fail;
	}

	ret = read_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, token),
			 &token);
	if (ret) {
		kvm_err("%s: Error %d to read token (%d-%d)\n",
			__func__, ret, kvm->userspace_pid, vcpu->vcpu_idx);
		goto fail;
	}

	/*
	 * There might be pending page-not-present notification (SDEI)
	 * to be delivered. However, the corresponding work has been
	 * completed. For this case, we need to cancel the notification
	 * early to avoid the overhead because of the injected SDEI
	 * and interrupt.
	 */
	if (apf->notpresent_pending) {
		spin_lock(&vcpu->async_pf.lock);
		work = list_first_entry_or_null(&vcpu->async_pf.done,
						typeof(*work), link);
		spin_unlock(&vcpu->async_pf.lock);
		if (!work)
			goto fail;

		if (reason == KVM_PV_REASON_PAGE_NOT_PRESENT &&
		    work->arch.token == apf->notpresent_token &&
		    token == apf->notpresent_token) {
			kvm_make_request(KVM_REQ_ASYNC_PF, vcpu);
			return true;
		}
	}

	if (reason || token)
		goto fail;

	return true;

fail:
	kvm_make_request(KVM_REQ_ASYNC_PF, vcpu);
	return false;
}

void kvm_arch_async_page_ready(struct kvm_vcpu *vcpu,
			       struct kvm_async_pf *work)
{
	struct kvm_memory_slot *memslot;
	unsigned int esr = work->arch.esr;
	phys_addr_t gpa = work->cr2_or_gpa;
	gfn_t gfn = gpa >> PAGE_SHIFT;
	unsigned long hva;
	bool write_fault, writable;
	int idx;

	/*
	 * We shouldn't issue prefault for special work to wake up
	 * all pending tasks because the associated token (address)
	 * is invalid.
	 */
	if (work->wakeup_all)
		return;

	/*
	 * The gpa was validated before the work is started. However, the
	 * memory slots might be changed since then. So we need to redo the
	 * validatation here.
	 */
	idx = srcu_read_lock(&vcpu->kvm->srcu);

	if (esr_dabt_is_s1ptw(esr))
		write_fault = true;
	else if (ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW)
		write_fault = false;
	else
		write_fault = esr_dabt_is_wnr(esr);

	memslot = gfn_to_memslot(vcpu->kvm, gfn);
	hva = gfn_to_hva_memslot_prot(memslot, gfn, &writable);
	if (kvm_is_error_hva(hva) || (write_fault && !writable))
		goto out;

	kvm_handle_user_mem_abort(vcpu, memslot, gpa, hva, esr, true);

out:
	srcu_read_unlock(&vcpu->kvm->srcu, idx);
}

/*
 * It's guaranteed that no pending asynchronous page fault when this is
 * called. It means all previous issued asynchronous page faults have
 * been acknowledged.
 */
void kvm_arch_async_page_present(struct kvm_vcpu *vcpu,
				 struct kvm_async_pf *work)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;
	int ret;

	/*
	 * The work could be completed prior to page-not-present notification
	 * delivery. In this case, what we need to do is just to cancel the
	 * page-not-present notification to avoid unnecessary overhead.
	 */
	if (work->wakeup_all) {
		work->arch.token = ~0;
	} else {
		kvm_async_pf_remove_slot(vcpu, work->arch.gfn);

		if (apf->notpresent_pending &&
		    apf->notpresent_token == work->arch.token &&
		    !kvm_sdei_cancel(vcpu, apf->sdei_event_num)) {
			apf->notpresent_pending = false;
			apf->notpresent_token = 0;
			goto done;
		}
	}

	ret = write_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, token),
			  work->arch.token);
	if (ret) {
		kvm_err("%s: Error %d to write token (%d-%d %08x)\n",
			__func__, ret, kvm->userspace_pid,
			vcpu->vcpu_idx, work->arch.token);
		goto done;
	}

	ret = write_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, reason),
			  KVM_PV_REASON_PAGE_READY);
	if (ret) {
		kvm_err("%s: Error %d to write reason (%d-%d %08x)\n",
			__func__, ret, kvm->userspace_pid,
			vcpu->vcpu_idx, work->arch.token);
		goto done;
	}

	apf->pageready_pending = true;
	kvm_vgic_inject_irq(vcpu->kvm, vcpu->vcpu_idx,
			    apf->irq, true, NULL);
	return;

done:
	write_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, reason), 0);
	write_cache(vcpu, offsetof(struct kvm_vcpu_pv_apf_data, token), 0);
}

static void kvm_arch_async_sdei_notifier(struct kvm_vcpu *vcpu,
					 unsigned long num,
					 unsigned int state)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;

	if (!apf)
		return;

	if (num != apf->sdei_event_num) {
		kvm_err("%s: Invalid event number (%d-%d %lx-%llx)\n",
			__func__, kvm->userspace_pid, vcpu->vcpu_idx,
			num, apf->sdei_event_num);
		return;
	}

	switch (state) {
	case KVM_SDEI_NOTIFY_DELIVERED:
		if (!apf->notpresent_pending)
			break;

		apf->notpresent_token = 0;
		apf->notpresent_pending = false;
		break;
	case KVM_SDEI_NOTIFY_COMPLETED:
		break;
	default:
		kvm_err("%s: Invalid state (%d-%d %lx-%d)\n",
			__func__, kvm->userspace_pid, vcpu->vcpu_idx,
			num, state);
	}
}

static long kvm_arch_async_enable(struct kvm_vcpu *vcpu, u64 data)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;
	gpa_t gpa = (data & ~0x3FUL);
	bool enabled, enable;
	int ret;

	if (!apf || !irqchip_in_kernel(kvm))
		return SMCCC_RET_NOT_SUPPORTED;

	/* Bail if the state transition isn't allowed */
	enabled = !!(apf->control_block & KVM_ASYNC_PF_ENABLED);
	enable = !!(data & KVM_ASYNC_PF_ENABLED);
	if (enable == enabled) {
		kvm_debug("%s: Async PF has been %s on (%d-%d %llx-%llx)\n",
			  __func__, enabled ? "enabled" : "disabled",
			  kvm->userspace_pid, vcpu->vcpu_idx,
			  apf->control_block, data);
		return SMCCC_RET_NOT_REQUIRED;
	}

	/* To disable the functinality */
	if (!enable) {
		kvm_clear_async_pf_completion_queue(vcpu);
		apf->control_block = data;
		return SMCCC_RET_SUCCESS;
	}

	/*
	 * The SDEI event and IRQ number should have been given
	 * prior to enablement.
	 */
	if (!apf->sdei_event_num || !apf->irq) {
		kvm_err("%s: Invalid SDEI event or IRQ (%d-%d %llx-%d)\n",
			__func__, kvm->userspace_pid, vcpu->vcpu_idx,
			apf->sdei_event_num, apf->irq);
		return SMCCC_RET_INVALID_PARAMETER;
	}

	/* Register SDEI event notifier */
	ret = kvm_sdei_register_notifier(kvm, apf->sdei_event_num,
					 kvm_arch_async_sdei_notifier);
	if (ret) {
		kvm_err("%s: Error %d registering SDEI notifier (%d-%d %llx)\n",
			__func__, ret, kvm->userspace_pid, vcpu->vcpu_idx,
			apf->sdei_event_num);
		return SMCCC_RET_NOT_SUPPORTED;
	}

	/* Initialize cache shared by host and guest */
	ret = kvm_gfn_to_hva_cache_init(kvm, &apf->cache, gpa,
			offsetofend(struct kvm_vcpu_pv_apf_data, token));
	if (ret) {
		kvm_err("%s: Error %d initializing cache (%d-%d)\n",
			__func__, ret, kvm->userspace_pid, vcpu->vcpu_idx);
		return SMCCC_RET_NOT_SUPPORTED;
	}

	/* Flush the token table */
	kvm_async_pf_reset_slot(vcpu);
	apf->send_user_only = !(data & KVM_ASYNC_PF_SEND_ALWAYS);
	kvm_async_pf_wakeup_all(vcpu);
	apf->control_block = data;

	return SMCCC_RET_SUCCESS;
}

void kvm_arch_async_pf_hypercall(struct kvm_vcpu *vcpu, u64 *val)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;
	u32 func;
	u64 data;
	long ret = SMCCC_RET_SUCCESS;

	if (!apf) {
		val[0] = SMCCC_RET_NOT_SUPPORTED;
		return;
	}

	func = smccc_get_arg1(vcpu);
	switch (func) {
	case ARM_SMCCC_KVM_FUNC_ASYNC_PF_VERSION:
		val[1] = 0x010000; /* v1.0.0 */
		break;
	case ARM_SMCCC_KVM_FUNC_ASYNC_PF_SLOTS:
		val[1] = ASYNC_PF_PER_VCPU;
		break;
	case ARM_SMCCC_KVM_FUNC_ASYNC_PF_SDEI:
		val[1] = apf->sdei_event_num;
		break;
	case ARM_SMCCC_KVM_FUNC_ASYNC_PF_IRQ:
		val[1] = apf->irq;
		break;
	case ARM_SMCCC_KVM_FUNC_ASYNC_PF_ENABLE:
		data = (smccc_get_arg3(vcpu) << 32) | smccc_get_arg2(vcpu);
		ret = kvm_arch_async_enable(vcpu, data);
		break;
	case ARM_SMCCC_KVM_FUNC_ASYNC_PF_IRQ_ACK:
		if (!apf->pageready_pending)
			break;

		kvm_vgic_inject_irq(kvm, vcpu->vcpu_idx,
				    apf->irq, false, NULL);
		apf->pageready_pending = false;
		kvm_check_async_pf_completion(vcpu);
		break;
	default:
		ret = SMCCC_RET_NOT_SUPPORTED;
	}

	val[0] = ret;
}

long kvm_arch_async_pf_vm_ioctl(struct kvm *kvm, unsigned long arg)
{
	struct kvm_arm_async_pf_cmd cmd;
	unsigned int version = 0x010000; /* v1.0.0 */
	void __user *argp = (void __user *)arg;

	if (copy_from_user(&cmd, argp, sizeof(cmd)))
		return -EFAULT;

	if (cmd.cmd != KVM_ARM_ASYNC_PF_CMD_GET_VERSION)
		return -EINVAL;

	cmd.version = version;
	if (copy_to_user(argp, &cmd, sizeof(cmd)))
		return -EFAULT;

	return 0;
}

long kvm_arch_async_pf_vcpu_ioctl(struct kvm_vcpu *vcpu, unsigned long arg)
{
	struct kvm_arch_async_pf_control *apf = vcpu->arch.apf;
	struct kvm_arm_async_pf_cmd cmd;
	void __user *argp = (void __user *)arg;
	long ret = 0;

	if (!apf)
		return -EPERM;

	if (copy_from_user(&cmd, argp, sizeof(cmd)))
		return -EFAULT;

	switch (cmd.cmd) {
	case KVM_ARM_ASYNC_PF_CMD_GET_SDEI:
		cmd.sdei = apf->sdei_event_num;
		break;
	case KVM_ARM_ASYNC_PF_CMD_GET_IRQ:
		cmd.irq = apf->irq;
		break;
	case KVM_ARM_ASYNC_PF_CMD_GET_CONTROL:
		cmd.control = apf->control_block;
		break;
	case KVM_ARM_ASYNC_PF_CMD_SET_SDEI:
		apf->sdei_event_num = cmd.sdei;
		break;
	case KVM_ARM_ASYNC_PF_CMD_SET_IRQ:
		apf->irq = cmd.irq;
		break;
	case KVM_ARM_ASYNC_PF_CMD_SET_CONTROL:
		if (kvm_arch_async_enable(vcpu, cmd.control) !=
		    SMCCC_RET_SUCCESS)
			ret = -EIO;

		break;
	default:
		ret = -EINVAL;
	}

	if (!ret && copy_to_user(argp, &cmd, sizeof(cmd)))
		ret = -EFAULT;

	return ret;
}

void kvm_arch_async_pf_destroy_vcpu(struct kvm_vcpu *vcpu)
{
	kfree(vcpu->arch.apf);
	vcpu->arch.apf = NULL;
}
