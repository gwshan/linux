// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2026 ARM Ltd.
 */

#include <linux/arm-rmi-cmds.h>
#include <linux/kvm_host.h>

#include <asm/kvm_emulate.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_pgtable.h>
#include <asm/virt.h>

u32 kvm_rmm_ipa_limit(void)
{
	return u64_get_bits(rmi_feat_reg(0), RMI_FEATURE_REGISTER_0_S2SZ);
}

void kvm_destroy_realm(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	size_t pgd_size = kvm_pgtable_stage2_pgd_size(kvm->arch.mmu.vtcr);

	if (!kvm_realm_is_created(kvm))
		goto out_free_sro;

	kvm_set_realm_state(kvm, REALM_STATE_DYING);

	if (realm->rd_phys) {
		phys_addr_t rd_phys = realm->rd_phys;

		if (WARN_ON(rmi_realm_terminate(rd_phys, realm->sro)))
			goto out_free_sro;

		if (WARN_ON(rmi_realm_destroy(rd_phys, realm->sro)))
			goto out_free_sro;
		rmi_free_delegated_page(rd_phys);
		realm->rd_phys = 0;
	}

	if (WARN_ON(rmi_undelegate_range(kvm->arch.mmu.pgd_phys,
					 pgd_size)))
		goto out_free_sro;

	kvm_set_realm_state(kvm, REALM_STATE_DEAD);

out_free_sro:
	kfree(realm->sro);
	realm->sro = NULL;
}

int kvm_init_realm(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;

	realm->ia_bits = VTCR_EL2_IPA(kvm->arch.mmu.vtcr);
	realm->sro = kmalloc_obj(*realm->sro);
	if (!realm->sro)
		return -ENOMEM;

	return 0;
}
