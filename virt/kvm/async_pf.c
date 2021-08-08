// SPDX-License-Identifier: GPL-2.0-only
/*
 * kvm asynchronous fault support
 *
 * Copyright 2010 Red Hat, Inc.
 *
 * Author:
 *      Gleb Natapov <gleb@redhat.com>
 */

#include <linux/kvm_host.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mmu_context.h>
#include <linux/sched/mm.h>
#ifdef CONFIG_KVM_ASYNC_PF_SLOT
#include <linux/hash.h>
#endif

#include "async_pf.h"
#include <trace/events/kvm.h>

static struct kmem_cache *async_pf_cache;

#ifdef CONFIG_KVM_ASYNC_PF_SLOT
static inline u32 kvm_async_pf_hash(gfn_t gfn)
{
	BUILD_BUG_ON(!is_power_of_2(ASYNC_PF_PER_VCPU));

	return hash_32(gfn & 0xffffffff, order_base_2(ASYNC_PF_PER_VCPU));
}

static inline u32 kvm_async_pf_next_slot(u32 key)
{
	return (key + 1) & (ASYNC_PF_PER_VCPU - 1);
}

static u32 kvm_async_pf_slot(struct kvm_vcpu *vcpu, gfn_t gfn)
{
	u32 key = kvm_async_pf_hash(gfn);
	int i;

	for (i = 0; i < ASYNC_PF_PER_VCPU &&
		(vcpu->async_pf.gfns[key] != gfn &&
		vcpu->async_pf.gfns[key] != ~0); i++)
		key = kvm_async_pf_next_slot(key);

	return key;
}

void kvm_async_pf_reset_slot(struct kvm_vcpu *vcpu)
{
	int i;

	for (i = 0; i < ASYNC_PF_PER_VCPU; i++)
		vcpu->async_pf.gfns[i] = ~0;
}

void kvm_async_pf_add_slot(struct kvm_vcpu *vcpu, gfn_t gfn)
{
	u32 key = kvm_async_pf_hash(gfn);

	while (vcpu->async_pf.gfns[key] != ~0)
		key = kvm_async_pf_next_slot(key);

	vcpu->async_pf.gfns[key] = gfn;
}

void kvm_async_pf_remove_slot(struct kvm_vcpu *vcpu, gfn_t gfn)
{
	u32 i, j, k;

	i = j = kvm_async_pf_slot(vcpu, gfn);

	if (WARN_ON_ONCE(vcpu->async_pf.gfns[i] != gfn))
		return;

	while (true) {
		vcpu->async_pf.gfns[i] = ~0;

		do {
			j = kvm_async_pf_next_slot(j);
			if (vcpu->async_pf.gfns[j] == ~0)
				return;

			k = kvm_async_pf_hash(vcpu->async_pf.gfns[j]);
			/*
			 * k lies cyclically in ]i,j]
			 * |    i.k.j |
			 * |....j i.k.| or  |.k..j i...|
			 */
		} while ((i <= j) ? (i < k && k <= j) : (i < k || k <= j));

		vcpu->async_pf.gfns[i] = vcpu->async_pf.gfns[j];
		i = j;
	}
}

bool kvm_async_pf_find_slot(struct kvm_vcpu *vcpu, gfn_t gfn)
{
	u32 key = kvm_async_pf_slot(vcpu, gfn);

	return vcpu->async_pf.gfns[key] == gfn;
}
#endif /* CONFIG_KVM_ASYNC_PF_SLOT */

int kvm_async_pf_init(void)
{
	async_pf_cache = KMEM_CACHE(kvm_async_pf, 0);

	if (!async_pf_cache)
		return -ENOMEM;

	return 0;
}

void kvm_async_pf_deinit(void)
{
	kmem_cache_destroy(async_pf_cache);
	async_pf_cache = NULL;
}

void kvm_async_pf_vcpu_init(struct kvm_vcpu *vcpu)
{
	INIT_LIST_HEAD(&vcpu->async_pf.done);
	INIT_LIST_HEAD(&vcpu->async_pf.queue);
	spin_lock_init(&vcpu->async_pf.lock);
}

static void async_pf_execute(struct work_struct *work)
{
	struct kvm_async_pf *apf =
		container_of(work, struct kvm_async_pf, work);
	struct mm_struct *mm = apf->mm;
	struct kvm_vcpu *vcpu = apf->vcpu;
	unsigned long addr = apf->addr;
	gpa_t cr2_or_gpa = apf->cr2_or_gpa;
	int locked = 1;
	bool first;

	might_sleep();

	/*
	 * This work is run asynchronously to the task which owns
	 * mm and might be done in another context, so we must
	 * access remotely.
	 */
	mmap_read_lock(mm);
	get_user_pages_remote(mm, addr, 1, FOLL_WRITE, NULL, NULL,
			&locked);
	if (locked)
		mmap_read_unlock(mm);

	if (IS_ENABLED(CONFIG_KVM_ASYNC_PF_SYNC))
		kvm_arch_async_page_present(vcpu, apf);

	spin_lock(&vcpu->async_pf.lock);
	first = !kvm_check_async_pf_completion_queue(vcpu);
	list_add_tail(&apf->link, &vcpu->async_pf.done);
	apf->vcpu = NULL;
	spin_unlock(&vcpu->async_pf.lock);

	if (!IS_ENABLED(CONFIG_KVM_ASYNC_PF_SYNC) && first)
		kvm_arch_async_page_present_queued(vcpu);

	/*
	 * apf may be freed by kvm_check_async_pf_completion() after
	 * this point
	 */

	trace_kvm_async_pf_completed(addr, cr2_or_gpa);

	rcuwait_wake_up(&vcpu->wait);

	mmput(mm);
	kvm_put_kvm(vcpu->kvm);
}

void kvm_clear_async_pf_completion_queue(struct kvm_vcpu *vcpu)
{
	spin_lock(&vcpu->async_pf.lock);

	/* cancel outstanding work queue item */
	while (!list_empty(&vcpu->async_pf.queue)) {
		struct kvm_async_pf *work =
			list_first_entry(&vcpu->async_pf.queue,
					 typeof(*work), queue);
		list_del(&work->queue);

		/*
		 * We know it's present in vcpu->async_pf.done, do
		 * nothing here.
		 */
		if (!work->vcpu)
			continue;

		spin_unlock(&vcpu->async_pf.lock);
#ifdef CONFIG_KVM_ASYNC_PF_SYNC
		flush_work(&work->work);
#else
		if (cancel_work_sync(&work->work)) {
			mmput(work->mm);
			kvm_put_kvm(vcpu->kvm); /* == work->vcpu->kvm */
			kmem_cache_free(async_pf_cache, work);
		}
#endif
		spin_lock(&vcpu->async_pf.lock);
	}

	while (kvm_check_async_pf_completion_queue(vcpu)) {
		struct kvm_async_pf *work =
			list_first_entry(&vcpu->async_pf.done,
					 typeof(*work), link);
		list_del(&work->link);
		kmem_cache_free(async_pf_cache, work);
	}
	spin_unlock(&vcpu->async_pf.lock);

	vcpu->async_pf.queued = 0;
}

void kvm_check_async_pf_completion(struct kvm_vcpu *vcpu)
{
	struct kvm_async_pf *work;

	while (kvm_check_async_pf_completion_queue(vcpu) &&
	      kvm_arch_can_dequeue_async_page_present(vcpu)) {
		spin_lock(&vcpu->async_pf.lock);
		work = list_first_entry(&vcpu->async_pf.done, typeof(*work),
					      link);
		list_del(&work->link);
		spin_unlock(&vcpu->async_pf.lock);

		kvm_arch_async_page_ready(vcpu, work);
		if (!IS_ENABLED(CONFIG_KVM_ASYNC_PF_SYNC))
			kvm_arch_async_page_present(vcpu, work);

		list_del(&work->queue);
		vcpu->async_pf.queued--;
		kmem_cache_free(async_pf_cache, work);
	}
}

/*
 * Try to schedule a job to handle page fault asynchronously. Returns 'true' on
 * success, 'false' on failure (page fault has to be handled synchronously).
 */
bool kvm_setup_async_pf(struct kvm_vcpu *vcpu, gpa_t cr2_or_gpa,
			unsigned long hva, struct kvm_arch_async_pf *arch)
{
	struct kvm_async_pf *work;

	if (vcpu->async_pf.queued >= ASYNC_PF_PER_VCPU)
		return false;

	/* Arch specific code should not do async PF in this case */
	if (unlikely(kvm_is_error_hva(hva)))
		return false;

	/*
	 * do alloc nowait since if we are going to sleep anyway we
	 * may as well sleep faulting in page
	 */
	work = kmem_cache_zalloc(async_pf_cache, GFP_NOWAIT | __GFP_NOWARN);
	if (!work)
		return false;

	work->wakeup_all = false;
	work->vcpu = vcpu;
	work->cr2_or_gpa = cr2_or_gpa;
	work->addr = hva;
	work->arch = *arch;
	work->mm = current->mm;
	mmget(work->mm);
	kvm_get_kvm(work->vcpu->kvm);

	INIT_WORK(&work->work, async_pf_execute);

	list_add_tail(&work->queue, &vcpu->async_pf.queue);
	vcpu->async_pf.queued++;
	work->notpresent_injected = kvm_arch_async_page_not_present(vcpu, work);

	schedule_work(&work->work);

	return true;
}

int kvm_async_pf_wakeup_all(struct kvm_vcpu *vcpu)
{
	struct kvm_async_pf *work;
	bool first;

	if (kvm_check_async_pf_completion_queue(vcpu))
		return 0;

	work = kmem_cache_zalloc(async_pf_cache, GFP_ATOMIC);
	if (!work)
		return -ENOMEM;

	work->wakeup_all = true;
	INIT_LIST_HEAD(&work->queue); /* for list_del to work */

	spin_lock(&vcpu->async_pf.lock);
	first = !kvm_check_async_pf_completion_queue(vcpu);
	list_add_tail(&work->link, &vcpu->async_pf.done);
	spin_unlock(&vcpu->async_pf.lock);

	if (!IS_ENABLED(CONFIG_KVM_ASYNC_PF_SYNC) && first)
		kvm_arch_async_page_present_queued(vcpu);

	vcpu->async_pf.queued++;
	return 0;
}
