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

static int get_start_level(struct realm *realm)
{
	return 4 - max(2, stage2_pgtable_levels(realm->ia_bits));
}

static int find_map_level(struct realm *realm,
			  unsigned long start,
			  unsigned long end)
{
	int level = KVM_PGTABLE_LAST_LEVEL;

	while (level > get_start_level(realm)) {
		unsigned long map_size = kvm_granule_size(level - 1);

		if (!IS_ALIGNED(start, map_size) ||
		    (start + map_size) > end)
			break;

		level--;
	}

	return level;
}

static int undelegate_range_desc(unsigned long desc)
{
	unsigned long size = rmi_addr_block_size_to_bytes(RMI_ADDR_RANGE_BLOCK_SIZE(desc));
	unsigned long count = RMI_ADDR_RANGE_COUNT(desc);
	unsigned long addr = RMI_ADDR_RANGE_ADDR(desc);
	unsigned long state = RMI_ADDR_RANGE_STATE(desc);

	if (state == RMI_OP_MEM_UNDELEGATED)
		return 0;

	if (size * count == 0)
		return 0;

	return rmi_undelegate_range(addr, size * count);
}

static phys_addr_t alloc_delegated_granule(struct kvm_mmu_memory_cache *mc)
{
	phys_addr_t phys;
	void *virt;

	if (mc) {
		virt = kvm_mmu_memory_cache_alloc(mc);
	} else {
		virt = (void *)__get_free_page(GFP_ATOMIC | __GFP_ZERO |
					       __GFP_ACCOUNT);
	}

	if (!virt)
		return PHYS_ADDR_MAX;

	phys = virt_to_phys(virt);
	if (rmi_delegate_page(phys)) {
		free_page((unsigned long)virt);
		return PHYS_ADDR_MAX;
	}

	return phys;
}

static phys_addr_t alloc_rtt(struct kvm_mmu_memory_cache *mc)
{
	phys_addr_t phys = alloc_delegated_granule(mc);

	if (phys != PHYS_ADDR_MAX)
		kvm_account_pgtable_pages(phys_to_virt(phys), 1);

	return phys;
}

static void free_rtt(phys_addr_t phys)
{
	kvm_account_pgtable_pages(phys_to_virt(phys), -1);
	if (rmi_free_delegated_page(phys))
		kvm_account_pgtable_pages(phys_to_virt(phys), 1);
}

static long realm_rtt_create(struct realm *realm,
			     unsigned long addr,
			     int level,
			     phys_addr_t phys)
{
	addr = ALIGN_DOWN(addr, kvm_granule_size(level - 1));
	return rmi_rtt_create(virt_to_phys(realm->rd), phys, addr, level);
}

static long realm_rtt_fold(struct realm *realm,
			   unsigned long addr,
			   int level,
			   phys_addr_t *rtt_granule)
{
	unsigned long out_rtt;
	long ret;

	addr = ALIGN_DOWN(addr, kvm_granule_size(level - 1));
	ret = rmi_rtt_fold(virt_to_phys(realm->rd), addr, level, &out_rtt);

	if (ret == RMI_SUCCESS && rtt_granule)
		*rtt_granule = out_rtt;

	return ret;
}

/*
 * realm_rtt_destroy - Destroy an RTT at @level for @addr.
 *
 * Returns - Result of the RMI_RTT_DESTROY call, and:
 * @rtt_granule:	RTT granule, if the RTT was destroyed.
 * @next_addr:		IPA corresponding to the next possible valid entry we
 *			can target
 */
static long realm_rtt_destroy(struct realm *realm, unsigned long addr,
			      int level, phys_addr_t *rtt_granule,
			      unsigned long *next_addr)
{
	unsigned long out_rtt;
	long ret;

	ret = rmi_rtt_destroy(virt_to_phys(realm->rd), addr, level,
			      &out_rtt, next_addr);

	if (ret == RMI_SUCCESS)
		*rtt_granule = out_rtt;

	return ret;
}

static int realm_create_rtt_levels(struct realm *realm,
				   unsigned long ipa,
				   int level,
				   int max_level,
				   struct kvm_mmu_memory_cache *mc)
{
	long ret = 0;
	phys_addr_t rtt = PHYS_ADDR_MAX;

	while (level++ < max_level) {
		if (rtt == PHYS_ADDR_MAX)
			rtt = alloc_rtt(mc);
		if (rtt == PHYS_ADDR_MAX)
			return -ENOMEM;

		ret = realm_rtt_create(realm, ipa, level, rtt);
		if (ret < 0)
			break;

		if (RMI_RESULT_STATUS(ret) == RMI_ERROR_RTT &&
		    RMI_RESULT_DATA_LEVEL(ret) == level - 1) {
			/* The RTT already exists, reuse the granule at the next level. */
			ret = 0;
			continue;
		}

		if (ret) {
			WARN(1, "Failed to create RTT at level %d: %ld\n",
			     level, ret);
			ret = -ENXIO;
			break;
		}
		rtt = PHYS_ADDR_MAX;
	}

	if (rtt != PHYS_ADDR_MAX)
		free_rtt(rtt);

	return ret;
}

static int realm_tear_down_rtt_level(struct realm *realm, int level,
				     unsigned long start, unsigned long end)
{
	ssize_t map_size;
	unsigned long addr, next_addr;

	if (WARN_ON(level > KVM_PGTABLE_LAST_LEVEL))
		return -EINVAL;

	map_size = kvm_granule_size(level - 1);
	if (WARN_ON(!IS_ALIGNED(start, map_size)))
		return -EINVAL;

	for (addr = start; addr < end; addr = next_addr) {
		phys_addr_t rtt_granule;
		long ret;
		unsigned long align_addr = ALIGN(addr, map_size);

		next_addr = ALIGN(addr + 1, map_size);

		if (next_addr > end || align_addr != addr) {
			/*
			 * The target range is smaller than what this level
			 * covers, recurse deeper.
			 */
			ret = realm_tear_down_rtt_level(realm,
							level + 1,
							addr,
							min(next_addr, end));
			if (ret)
				return ret;
			continue;
		}

		ret = realm_rtt_destroy(realm, addr, level,
					&rtt_granule, &next_addr);
		if (ret < 0)
			return ret;

		switch (RMI_RESULT_STATUS(ret)) {
		case RMI_SUCCESS:
			free_rtt(rtt_granule);
			break;
		case RMI_ERROR_RTT:
			if (next_addr > addr) {
				/* Missing RTT, skip */
				break;
			}
			/*
			 * We tear down the RTT range for the full IPA
			 * space, after everything is unmapped. Also we
			 * descend down only if we cannot tear down a
			 * top level RTT. Thus RMM must be able to walk
			 * to the requested level. e.g., a block mapping
			 * exists at L1 or L2.
			 */
			if (WARN_ON(RMI_RESULT_DATA_LEVEL(ret) != level))
				return -EBUSY;
			if (WARN_ON(level == KVM_PGTABLE_LAST_LEVEL))
				return -EBUSY;

			/*
			 * The table has active entries in it, recurse deeper
			 * and tear down the RTTs.
			 */
			next_addr = ALIGN(addr + 1, map_size);
			ret = realm_tear_down_rtt_level(realm,
							level + 1,
							addr,
							next_addr);
			if (ret)
				return ret;
			/*
			 * Now that the child RTTs are destroyed,
			 * retry at this level.
			 */
			next_addr = addr;
			break;
		default:
			WARN_ON(1);
			return -ENXIO;
		}
	}

	return 0;
}

static int realm_tear_down_rtt_range(struct realm *realm,
				     unsigned long start, unsigned long end)
{
	/*
	 * Root level RTTs can only be destroyed after the RD is destroyed. So
	 * tear down everything below the root level
	 */
	return realm_tear_down_rtt_level(realm, get_start_level(realm) + 1,
					 start, end);
}

/*
 * Returns 0 on successful fold, a negative value on error, a positive value if
 * we were not able to fold all tables at this level.
 */
static int realm_fold_rtt_level(struct realm *realm, int level,
				unsigned long start, unsigned long end)
{
	int not_folded = 0;
	ssize_t map_size;
	unsigned long addr, next_addr;

	if (WARN_ON(level > KVM_PGTABLE_LAST_LEVEL))
		return -EINVAL;

	map_size = kvm_granule_size(level - 1);

	for (addr = start; addr < end; addr = next_addr) {
		phys_addr_t rtt_granule;
		long ret;

		next_addr = ALIGN(addr + 1, map_size);

		ret = realm_rtt_fold(realm, addr, level, &rtt_granule);
		if (ret < 0)
			return ret;

		switch (RMI_RESULT_STATUS(ret)) {
		case RMI_SUCCESS:
			free_rtt(rtt_granule);
			break;
		case RMI_ERROR_RTT: {
			int err_level = RMI_RESULT_DATA_LEVEL(ret);

			if (err_level < level) {
				/* No RTT at this level; skip the missing ancestor. */
				if (WARN_ON(err_level < get_start_level(realm)))
					return -EINVAL;
				next_addr = ALIGN(addr + 1,
						  kvm_granule_size(err_level - 1));
				break;
			}
			if (level == KVM_PGTABLE_LAST_LEVEL) {
				not_folded++;
				break;
			}
			/* Recurse a level deeper */
			ret = realm_fold_rtt_level(realm,
						   level + 1,
						   addr,
						   min(next_addr, end));
			if (ret < 0)
				return ret;
			if (ret) {
				not_folded++;
				break;
			}
			/* Try once more after folding the child RTTs. */
			ret = realm_rtt_fold(realm, addr, level, &rtt_granule);
			if (ret < 0)
				return ret;
			if (RMI_RESULT_STATUS(ret) == RMI_SUCCESS) {
				free_rtt(rtt_granule);
				break;
			}
			if (RMI_RESULT_STATUS(ret) != RMI_ERROR_RTT) {
				WARN_ON(1);
				return -ENXIO;
			}
			not_folded++;
			break;
		}
		default:
			WARN_ON(1);
			return -ENXIO;
		}
	}

	return not_folded;
}

static int realm_destroy_rtts(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	unsigned int ia_bits = realm->ia_bits;

	lockdep_assert_held(&kvm->arch.config_lock);
	if (WARN_ON_ONCE(!realm->stage2_unmapped))
		return -EINVAL;

	return realm_tear_down_rtt_range(realm, 0, (1UL << ia_bits));
}

static void realm_unmap_stage2(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;

	lockdep_assert_held(&kvm->arch.config_lock);

	if (realm->stage2_unmapped)
		return;

	write_lock(&kvm->mmu_lock);
	kvm_stage2_unmap_range(&kvm->arch.mmu, 0,
			       BIT(realm->ia_bits - 1), true);
	write_unlock(&kvm->mmu_lock);

	realm->stage2_unmapped = true;
}

int kvm_realm_teardown_stage2(struct kvm *kvm)
{
	lockdep_assert_held(&kvm->arch.config_lock);

	realm_unmap_stage2(kvm);
	return 0;
}

static void realm_unmap_shared_range(struct kvm *kvm,
				     unsigned long start,
				     unsigned long end,
				     bool may_block)
{
	struct realm *realm = &kvm->arch.realm;
	unsigned long rd = virt_to_phys(realm->rd);
	unsigned long next_addr, addr;
	unsigned long shared_bit = BIT(realm->ia_bits - 1);

	if (start >= end)
		return;

	/*
	 * Callers may pass either an IPA range or its shared alias. Add the
	 * shared bit only for the former. Use addition so an exclusive end at
	 * shared_bit advances to the end of the IPA space.
	 */
	if (start < shared_bit) {
		start += shared_bit;
		end += shared_bit;
	}

	for (addr = start; addr < end; addr = next_addr) {
		long ret;

		ret = rmi_rtt_unprot_unmap(rd, addr, end, RMI_ADDR_TYPE_NONE,
					   0, &next_addr, NULL, NULL);
		if (WARN_ON(ret < 0))
			return;

		switch (RMI_RESULT_STATUS(ret)) {
		case RMI_SUCCESS:
			break;
		case RMI_ERROR_RTT: {
			int err_level = RMI_RESULT_DATA_LEVEL(ret);
			int level = find_map_level(realm, addr, end);

			if (WARN_ON(err_level > KVM_PGTABLE_LAST_LEVEL))
				return;

			if (err_level >= level) {
				/* Nothing present, so skip */
				next_addr = ALIGN(addr + 1,
						  kvm_granule_size(err_level));
				break;
			}

			ret = realm_create_rtt_levels(realm, addr, err_level,
						      level, NULL);
			if (WARN_ON(ret))
				return;
			/* Retry with the RTT levels in place */
			next_addr = addr;
			break;
		}
		default:
			WARN_ON(1);
			return;
		}

		if (may_block)
			cond_resched_rwlock_write(&kvm->mmu_lock);
	}

	realm_fold_rtt_level(realm, get_start_level(realm) + 1,
			     start, end);
}

static void realm_unmap_private_range(struct kvm *kvm,
				      unsigned long start,
				      unsigned long end,
				      bool may_block)
{
	struct realm *realm = &kvm->arch.realm;
	unsigned long rd = virt_to_phys(realm->rd);
	unsigned long next_addr, addr;
	long ret;

	for (addr = start; addr < end; addr = next_addr) {
		unsigned long out_range;
		unsigned long flags = RMI_ADDR_TYPE_SINGLE;
		/* TODO: Optimise using RMI_ADDR_TYPE_LIST */

		ret = rmi_rtt_data_unmap(rd, addr, end, flags, 0,
					 &next_addr, &out_range, NULL);
		if (WARN_ON(ret < 0))
			return;

		switch (RMI_RESULT_STATUS(ret)) {
		case RMI_SUCCESS:
			ret = undelegate_range_desc(out_range);
			if (WARN_ON(ret))
				return;
			break;
		case RMI_ERROR_RTT: {
			int err_level = RMI_RESULT_DATA_LEVEL(ret);
			int level = find_map_level(realm, addr, end);

			if (WARN_ON(err_level > KVM_PGTABLE_LAST_LEVEL))
				return;

			if (err_level >= level) {
				/* Nothing present, so skip this RTT entry. */
				next_addr = ALIGN(addr + 1,
						  kvm_granule_size(err_level));
				break;
			}

			ret = realm_create_rtt_levels(realm, addr, err_level,
						      level, NULL);
			if (WARN_ON(ret))
				return;
			/* Retry with the RTT levels in place. */
			next_addr = addr;
			break;
		}
		default:
			WARN_ON(1);
			return;
		}

		if (may_block)
			cond_resched_rwlock_write(&kvm->mmu_lock);
	}

	realm_fold_rtt_level(realm, get_start_level(realm) + 1,
			     start, end);
}

void kvm_realm_unmap_range_filter(struct kvm *kvm, unsigned long start,
				  unsigned long size, bool may_block,
				  enum kvm_gfn_range_filter attr_filter)
{
	unsigned long end = start + size;
	struct realm *realm = &kvm->arch.realm;

	if (!kvm_realm_is_created(kvm))
		return;

	end = min(BIT(realm->ia_bits - 1), end);

	if (attr_filter & KVM_FILTER_SHARED)
		realm_unmap_shared_range(kvm, start, end, may_block);
	if (attr_filter & KVM_FILTER_PRIVATE)
		realm_unmap_private_range(kvm, start, end, may_block);
}

void kvm_destroy_realm(struct kvm *kvm)
{
	struct realm *realm = &kvm->arch.realm;
	size_t pgd_size = kvm_pgtable_stage2_pgd_size(kvm->arch.mmu.vtcr);

	guard(mutex)(&kvm->arch.config_lock);

	if (!kvm_realm_is_created(kvm)) {
		kfree(realm->sro);
		realm->sro = NULL;
		return;
	}

	kvm_set_realm_state(kvm, REALM_STATE_DYING);

	/*
	 * REALM_DESTROY requires the realm to be non-live: all RECs must have
	 * been destroyed and the root RTTs must be empty. Unmap the IPA space
	 * and destroy any non-root RTTs before tearing down the RD. The root
	 * RTT pages are still owned by the RMM at this point, so keep the KVM
	 * pgtable alive until after REALM_DESTROY and undelegation.
	 */
	realm_unmap_stage2(kvm);

	if (realm->rd) {
		phys_addr_t rd_phys = virt_to_phys(realm->rd);

		if (WARN_ON(rmi_realm_terminate(rd_phys, realm->sro)))
			return;

		if (WARN_ON(realm_destroy_rtts(kvm)))
			return;

		if (WARN_ON(rmi_realm_destroy(rd_phys, realm->sro)))
			return;
		rmi_free_delegated_page(rd_phys);
		realm->rd = NULL;
	}

	if (WARN_ON(rmi_undelegate_range(kvm->arch.mmu.pgd_phys,
					 pgd_size)))
		return;

	kvm_set_realm_state(kvm, REALM_STATE_DEAD);

	/* Now that the realm is destroyed, free the entry-level RTTs. */
	kvm_free_stage2_pgd(&kvm->arch.mmu);

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

static int rmm_check_features(void)
{
	if (kvm_lpa2_is_enabled() &&
	    !rmi_has_feature(0, RMI_FEATURE_REGISTER_0_LPA2)) {
		kvm_err("RMM doesn't support LPA2\n");
		return -ENXIO;
	}

	if (!rmi_has_feature(1, RMI_FEATURE_REGISTER_1_HASH_SHA_256)) {
		kvm_err("RMM doesn't support SHA-256 measurements\n");
		return -ENXIO;
	}

	return 0;
}

void kvm_init_rmi(void)
{
	if (kvm_get_mode() != KVM_MODE_RMM)
		return;

	if (!is_rmi_available()) {
		kvm_info("Realm VM support is disabled because RMI is not available\n");
		return;
	}

	if (rmm_check_features())
		return;

	/* Future patch will enable static branch kvm_rmi_is_available */
}
