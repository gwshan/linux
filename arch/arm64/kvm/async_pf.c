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

void kvm_arch_async_pf_destroy_vcpu(struct kvm_vcpu *vcpu)
{
	kfree(vcpu->arch.apf);
	vcpu->arch.apf = NULL;
}
