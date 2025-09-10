// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Arm Ltd.

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/arm_mpam.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/cacheinfo.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "mpam_internal.h"

DEFINE_STATIC_KEY_FALSE(mpam_enabled); /* TODO: move to arch code */

/*
 * mpam_list_lock protects the SRCU lists when writing. Once the
 * mpam_enabled key is enabled these lists are read-only,
 * unless the error interrupt disables the driver.
 */
static DEFINE_MUTEX(mpam_list_lock);
static LIST_HEAD(mpam_all_msc);

struct srcu_struct mpam_srcu;

/*
 * Number of MSCs that have been probed. Once all MSC have been probed MPAM
 * can be enabled.
 */
static atomic_t mpam_num_msc;

static int mpam_cpuhp_state;
static DEFINE_MUTEX(mpam_cpuhp_state_lock);

/*
 * The smallest common values for any CPU or MSC in the system.
 * Generating traffic outside this range will result in screaming interrupts.
 */
u16 mpam_partid_max;
u8 mpam_pmg_max;
static bool partid_max_init, partid_max_published;
static DEFINE_SPINLOCK(partid_max_lock);

/*
 * mpam is enabled once all devices have been probed from CPU online callbacks,
 * scheduled via this work_struct. If access to an MSC depends on a CPU that
 * was not brought online at boot, this can happen surprisingly late.
 */
static DECLARE_WORK(mpam_enable_work, &mpam_enable);

/*
 * All mpam error interrupts indicate a software bug. On receipt, disable the
 * driver.
 */
static DECLARE_WORK(mpam_broken_work, &mpam_disable);

/*
 * An MSC is a physical container for controls and monitors, each identified by
 * their RIS index. These share a base-address, interrupts and some MMIO
 * registers. A vMSC is a virtual container for RIS in an MSC that control or
 * monitor the same thing. Members of a vMSC are all RIS in the same MSC, but
 * not all RIS in an MSC share a vMSC.
 * Components are a group of vMSC that control or monitor the same thing but
 * are from different MSC, so have different base-address, interrupts etc.
 * Classes are the set components of the same type.
 *
 * The features of a vMSC is the union of the RIS it contains.
 * The features of a Class and Component are the common subset of the vMSC
 * they contain.
 *
 * e.g. The system cache may have bandwidth controls on multiple interfaces,
 * for regulating traffic from devices independently of traffic from CPUs.
 * If these are two RIS in one MSC, they will be treated as controlling
 * different things, and will not share a vMSC/component/class.
 *
 * e.g. The L2 may have one MSC and two RIS, one for cache-controls another
 * for bandwidth. These two RIS are members of the same vMSC.
 *
 * e.g. The set of RIS that make up the L2 are grouped as a component. These
 * are sometimes termed slices. They should be configured the same, as if there
 * were only one.
 *
 * e.g. The SoC probably has more than one L2, each attached to a distinct set
 * of CPUs. All the L2 components are grouped as a class.
 *
 * When creating an MSC, struct mpam_msc is added to the all mpam_all_msc list,
 * then linked via struct mpam_ris to a vmsc, component and class.
 * The same MSC may exist under different class->component->vmsc paths, but the
 * RIS index will be unique.
 */
LIST_HEAD(mpam_classes);

/* List of all objects that can be free()d after synchronise_srcu() */
static LLIST_HEAD(mpam_garbage);

/* When mpam is disabled, the printed reason to aid debugging */
static char *mpam_disable_reason;

/*
 * Once mpam is enabled, new requestors cannot further reduce the available
 * partid. Assert that the size is fixed, and new requestors will be turned
 * away.
 */
static void mpam_assert_partid_sizes_fixed(void)
{
	WARN_ON_ONCE(!partid_max_published);
}

static u32 __mpam_read_reg(struct mpam_msc *msc, u16 reg)
{
	WARN_ON_ONCE(!cpumask_test_cpu(smp_processor_id(), &msc->accessibility));

	return readl_relaxed(msc->mapped_hwpage + reg);
}

static inline u32 _mpam_read_partsel_reg(struct mpam_msc *msc, u16 reg)
{
	lockdep_assert_held_once(&msc->part_sel_lock);
	return __mpam_read_reg(msc, reg);
}

#define mpam_read_partsel_reg(msc, reg)        _mpam_read_partsel_reg(msc, MPAMF_##reg)

static void __mpam_write_reg(struct mpam_msc *msc, u16 reg, u32 val)
{
	WARN_ON_ONCE(reg + sizeof(u32) > msc->mapped_hwpage_sz);
	WARN_ON_ONCE(!cpumask_test_cpu(smp_processor_id(), &msc->accessibility));

	writel_relaxed(val, msc->mapped_hwpage + reg);
}

static inline void _mpam_write_partsel_reg(struct mpam_msc *msc, u16 reg, u32 val)
{
	lockdep_assert_held_once(&msc->part_sel_lock);
	__mpam_write_reg(msc, reg, val);
}
#define mpam_write_partsel_reg(msc, reg, val)  _mpam_write_partsel_reg(msc, MPAMCFG_##reg, val)

static inline u32 _mpam_read_monsel_reg(struct mpam_msc *msc, u16 reg)
{
	mpam_mon_sel_lock_held(msc);
	return __mpam_read_reg(msc, reg);
}
#define mpam_read_monsel_reg(msc, reg) _mpam_read_monsel_reg(msc, MSMON_##reg)

static inline void _mpam_write_monsel_reg(struct mpam_msc *msc, u16 reg, u32 val)
{
	mpam_mon_sel_lock_held(msc);
	__mpam_write_reg(msc, reg, val);
}
#define mpam_write_monsel_reg(msc, reg, val)   _mpam_write_monsel_reg(msc, MSMON_##reg, val)

static u64 mpam_msc_read_idr(struct mpam_msc *msc)
{
	u64 idr_high = 0, idr_low;

	lockdep_assert_held(&msc->part_sel_lock);

	idr_low = mpam_read_partsel_reg(msc, IDR);
	if (FIELD_GET(MPAMF_IDR_EXT, idr_low))
		idr_high = mpam_read_partsel_reg(msc, IDR + 4);

	return (idr_high << 32) | idr_low;
}

static void mpam_msc_zero_esr(struct mpam_msc *msc)
{
	__mpam_write_reg(msc, MPAMF_ESR, 0);
	if (msc->has_extd_esr)
		__mpam_write_reg(msc, MPAMF_ESR + 4, 0);
}

static u64 mpam_msc_read_esr(struct mpam_msc *msc)
{
	u64 esr_high = 0, esr_low;

	esr_low = __mpam_read_reg(msc, MPAMF_ESR);
	if (msc->has_extd_esr)
		esr_high = __mpam_read_reg(msc, MPAMF_ESR + 4);

	return (esr_high << 32) | esr_low;
}

static void __mpam_part_sel_raw(u32 partsel, struct mpam_msc *msc)
{
	lockdep_assert_held(&msc->part_sel_lock);

	mpam_write_partsel_reg(msc, PART_SEL, partsel);
}

static void __mpam_part_sel(u8 ris_idx, u16 partid, struct mpam_msc *msc)
{
	u32 partsel = FIELD_PREP(MPAMCFG_PART_SEL_RIS, ris_idx) |
		      FIELD_PREP(MPAMCFG_PART_SEL_PARTID_SEL, partid);

	__mpam_part_sel_raw(partsel, msc);
}

static void __mpam_intpart_sel(u8 ris_idx, u16 intpartid, struct mpam_msc *msc)
{
	u32 partsel = FIELD_PREP(MPAMCFG_PART_SEL_RIS, ris_idx) |
		      FIELD_PREP(MPAMCFG_PART_SEL_PARTID_SEL, intpartid) |
		      MPAMCFG_PART_SEL_INTERNAL;

	__mpam_part_sel_raw(partsel, msc);
}

int mpam_register_requestor(u16 partid_max, u8 pmg_max)
{
	int err = 0;

	spin_lock(&partid_max_lock);
	if (!partid_max_init) {
		mpam_partid_max = partid_max;
		mpam_pmg_max = pmg_max;
		partid_max_init = true;
	} else if (!partid_max_published) {
		mpam_partid_max = min(mpam_partid_max, partid_max);
		mpam_pmg_max = min(mpam_pmg_max, pmg_max);
	} else {
		/* New requestors can't lower the values */
		if (partid_max < mpam_partid_max || pmg_max < mpam_pmg_max)
			err = -EBUSY;
	}
	spin_unlock(&partid_max_lock);

	return err;
}
EXPORT_SYMBOL(mpam_register_requestor);

#define init_garbage(x)	init_llist_node(&(x)->garbage.llist)

static struct mpam_vmsc *
mpam_vmsc_alloc(struct mpam_component *comp, struct mpam_msc *msc)
{
	struct mpam_vmsc *vmsc;

	lockdep_assert_held(&mpam_list_lock);

	vmsc = kzalloc(sizeof(*vmsc), GFP_KERNEL);
	if (!vmsc)
		return ERR_PTR(-ENOMEM);
	init_garbage(vmsc);

	INIT_LIST_HEAD_RCU(&vmsc->ris);
	INIT_LIST_HEAD_RCU(&vmsc->comp_list);
	vmsc->comp = comp;
	vmsc->msc = msc;

	list_add_rcu(&vmsc->comp_list, &comp->vmsc);

	return vmsc;
}

static struct mpam_vmsc *mpam_vmsc_get(struct mpam_component *comp,
				       struct mpam_msc *msc)
{
	struct mpam_vmsc *vmsc;

	lockdep_assert_held(&mpam_list_lock);

	list_for_each_entry(vmsc, &comp->vmsc, comp_list) {
		if (vmsc->msc->id == msc->id)
			return vmsc;
	}

	return mpam_vmsc_alloc(comp, msc);
}

static struct mpam_component *
mpam_component_alloc(struct mpam_class *class, int id)
{
	struct mpam_component *comp;

	lockdep_assert_held(&mpam_list_lock);

	comp = kzalloc(sizeof(*comp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);
	init_garbage(comp);

	comp->comp_id = id;
	INIT_LIST_HEAD_RCU(&comp->vmsc);
	/* affinity is updated when ris are added */
	INIT_LIST_HEAD_RCU(&comp->class_list);
	comp->class = class;

	list_add_rcu(&comp->class_list, &class->components);

	return comp;
}

static struct mpam_component *
mpam_component_get(struct mpam_class *class, int id)
{
	struct mpam_component *comp;

	lockdep_assert_held(&mpam_list_lock);

	list_for_each_entry(comp, &class->components, class_list) {
		if (comp->comp_id == id)
			return comp;
	}

	return mpam_component_alloc(class, id);
}

static struct mpam_class *
mpam_class_alloc(u8 level_idx, enum mpam_class_types type)
{
	struct mpam_class *class;

	lockdep_assert_held(&mpam_list_lock);

	class = kzalloc(sizeof(*class), GFP_KERNEL);
	if (!class)
		return ERR_PTR(-ENOMEM);
	init_garbage(class);

	INIT_LIST_HEAD_RCU(&class->components);
	/* affinity is updated when ris are added */
	class->level = level_idx;
	class->type = type;
	INIT_LIST_HEAD_RCU(&class->classes_list);

	list_add_rcu(&class->classes_list, &mpam_classes);

	return class;
}

static struct mpam_class *
mpam_class_get(u8 level_idx, enum mpam_class_types type)
{
	bool found = false;
	struct mpam_class *class;

	lockdep_assert_held(&mpam_list_lock);

	list_for_each_entry(class, &mpam_classes, classes_list) {
		if (class->type == type && class->level == level_idx) {
			found = true;
			break;
		}
	}

	if (found)
		return class;

	return mpam_class_alloc(level_idx, type);
}

#define add_to_garbage(x)				\
do {							\
	__typeof__(x) _x = (x);				\
	_x->garbage.to_free = _x;			\
	llist_add(&_x->garbage.llist, &mpam_garbage);	\
} while (0)

static void mpam_class_destroy(struct mpam_class *class)
{
	lockdep_assert_held(&mpam_list_lock);

	list_del_rcu(&class->classes_list);
	add_to_garbage(class);
}

static void __destroy_component_cfg(struct mpam_component *comp);

static void mpam_comp_destroy(struct mpam_component *comp)
{
	struct mpam_class *class = comp->class;

	lockdep_assert_held(&mpam_list_lock);

	__destroy_component_cfg(comp);

	list_del_rcu(&comp->class_list);
	add_to_garbage(comp);

	if (list_empty(&class->components))
		mpam_class_destroy(class);
}

static void mpam_vmsc_destroy(struct mpam_vmsc *vmsc)
{
	struct mpam_component *comp = vmsc->comp;

	lockdep_assert_held(&mpam_list_lock);

	list_del_rcu(&vmsc->comp_list);
	add_to_garbage(vmsc);

	if (list_empty(&comp->vmsc))
		mpam_comp_destroy(comp);
}

static void mpam_ris_destroy(struct mpam_msc_ris *ris)
{
	struct mpam_vmsc *vmsc = ris->vmsc;
	struct mpam_msc *msc = vmsc->msc;
	struct platform_device *pdev = msc->pdev;
	struct mpam_component *comp = vmsc->comp;
	struct mpam_class *class = comp->class;

	lockdep_assert_held(&mpam_list_lock);

	/*
	 * It is assumed affinities don't overlap. If they do the class becomes
	 * unusable immediately.
	 */
	cpumask_andnot(&comp->affinity, &comp->affinity, &ris->affinity);
	cpumask_andnot(&class->affinity, &class->affinity, &ris->affinity);
	clear_bit(ris->ris_idx, &msc->ris_idxs);
	list_del_rcu(&ris->vmsc_list);
	list_del_rcu(&ris->msc_list);
	add_to_garbage(ris);
	ris->garbage.pdev = pdev;

	if (list_empty(&vmsc->ris))
		mpam_vmsc_destroy(vmsc);
}

/*
 * There are two ways of reaching a struct mpam_msc_ris. Via the
 * class->component->vmsc->ris, or via the msc.
 * When destroying the msc, the other side needs unlinking and cleaning up too.
 */
static void mpam_msc_destroy(struct mpam_msc *msc)
{
	struct platform_device *pdev = msc->pdev;
	struct mpam_msc_ris *ris, *tmp;

	lockdep_assert_held(&mpam_list_lock);

	list_for_each_entry_safe(ris, tmp, &msc->ris, msc_list)
		mpam_ris_destroy(ris);

	list_del_rcu(&msc->all_msc_list);
	platform_set_drvdata(pdev, NULL);

	add_to_garbage(msc);
	msc->garbage.pdev = pdev;
}

static void mpam_free_garbage(void)
{
	struct mpam_garbage *iter, *tmp;
	struct llist_node *to_free = llist_del_all(&mpam_garbage);

	if (!to_free)
		return;

	synchronize_srcu(&mpam_srcu);

	llist_for_each_entry_safe(iter, tmp, to_free, llist) {
		if (iter->pdev)
			devm_kfree(&iter->pdev->dev, iter->to_free);
		else
			kfree(iter->to_free);
	}
}

/*
 * The cacheinfo structures are only populated when CPUs are online.
 */
int mpam_get_cpumask_from_cache_id(unsigned long cache_id, u32 cache_level,
				   cpumask_t *affinity)
{
	return acpi_pptt_get_cpumask_from_cache_id(cache_id, affinity);
}

/*
 * cpumask_of_node() only knows about online CPUs. This can't tell us whether
 * a class is represented on all possible CPUs.
 */
static void get_cpumask_from_node_id(u32 node_id, cpumask_t *affinity)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (node_id == cpu_to_node(cpu))
			cpumask_set_cpu(cpu, affinity);
	}
}

static int mpam_ris_get_affinity(struct mpam_msc *msc, cpumask_t *affinity,
				 enum mpam_class_types type,
				 struct mpam_class *class,
				 struct mpam_component *comp)
{
	int err;

	switch (type) {
	case MPAM_CLASS_CACHE:
		err = mpam_get_cpumask_from_cache_id(comp->comp_id, class->level,
						     affinity);
		if (err)
			return err;

		if (cpumask_empty(affinity))
			pr_warn_once("%s no CPUs associated with cache node",
				     dev_name(&msc->pdev->dev));

		break;
	case MPAM_CLASS_MEMORY:
		get_cpumask_from_node_id(comp->comp_id, affinity);
		/* affinity may be empty for CPU-less memory nodes */
		break;
	case MPAM_CLASS_UNKNOWN:
		return 0;
	}

	cpumask_and(affinity, affinity, &msc->accessibility);

	return 0;
}

static int mpam_ris_create_locked(struct mpam_msc *msc, u8 ris_idx,
				  enum mpam_class_types type, u8 class_id,
				  int component_id)
{
	int err;
	struct mpam_vmsc *vmsc;
	struct mpam_msc_ris *ris;
	struct mpam_class *class;
	struct mpam_component *comp;

	lockdep_assert_held(&mpam_list_lock);

	if (ris_idx > MPAM_MSC_MAX_NUM_RIS)
		return -EINVAL;

	if (test_and_set_bit(ris_idx, &msc->ris_idxs))
		return -EBUSY;

	ris = devm_kzalloc(&msc->pdev->dev, sizeof(*ris), GFP_KERNEL);
	if (!ris)
		return -ENOMEM;
	init_garbage(ris);

	class = mpam_class_get(class_id, type);
	if (IS_ERR(class))
		return PTR_ERR(class);

	comp = mpam_component_get(class, component_id);
	if (IS_ERR(comp)) {
		if (list_empty(&class->components))
			mpam_class_destroy(class);
		return PTR_ERR(comp);
	}

	vmsc = mpam_vmsc_get(comp, msc);
	if (IS_ERR(vmsc)) {
		if (list_empty(&comp->vmsc))
			mpam_comp_destroy(comp);
		return PTR_ERR(vmsc);
	}

	err = mpam_ris_get_affinity(msc, &ris->affinity, type, class, comp);
	if (err) {
		if (list_empty(&vmsc->ris))
			mpam_vmsc_destroy(vmsc);
		return err;
	}

	ris->ris_idx = ris_idx;
	INIT_LIST_HEAD_RCU(&ris->vmsc_list);
	ris->vmsc = vmsc;

	cpumask_or(&comp->affinity, &comp->affinity, &ris->affinity);
	cpumask_or(&class->affinity, &class->affinity, &ris->affinity);
	list_add_rcu(&ris->vmsc_list, &vmsc->ris);
	list_add_rcu(&ris->msc_list, &msc->ris);

	return 0;
}

int mpam_ris_create(struct mpam_msc *msc, u8 ris_idx,
		    enum mpam_class_types type, u8 class_id, int component_id)
{
	int err;

	mutex_lock(&mpam_list_lock);
	err = mpam_ris_create_locked(msc, ris_idx, type, class_id,
				     component_id);
	mutex_unlock(&mpam_list_lock);
	if (err)
		mpam_free_garbage();

	return err;
}

static struct mpam_msc_ris *mpam_get_or_create_ris(struct mpam_msc *msc,
						   u8 ris_idx)
{
	int err;
	struct mpam_msc_ris *ris, *found = ERR_PTR(-ENOENT);

	lockdep_assert_held(&mpam_list_lock);

	if (!test_bit(ris_idx, &msc->ris_idxs)) {
		err = mpam_ris_create_locked(msc, ris_idx, MPAM_CLASS_UNKNOWN,
					     0, 0);
		if (err)
			return ERR_PTR(err);
	}

	list_for_each_entry(ris, &msc->ris, msc_list) {
		if (ris->ris_idx == ris_idx) {
			found = ris;
			break;
		}
	}

	return found;
}

/*
 * IHI009A.a has this nugget: "If a monitor does not support automatic behaviour
 * of NRDY, software can use this bit for any purpose" - so hardware might not
 * implement this - but it isn't RES0.
 *
 * Try and see what values stick in this bit. If we can write either value,
 * its probably not implemented by hardware.
 */
static bool _mpam_ris_hw_probe_hw_nrdy(struct mpam_msc_ris * ris, u32 mon_reg)
{
	u32 now;
	u64 mon_sel;
	bool can_set, can_clear;
	struct mpam_msc *msc = ris->vmsc->msc;

	if (WARN_ON_ONCE(!mpam_mon_sel_lock(msc)))
		return false;

	mon_sel = FIELD_PREP(MSMON_CFG_MON_SEL_MON_SEL, 0) |
		  FIELD_PREP(MSMON_CFG_MON_SEL_RIS, ris->ris_idx);
	_mpam_write_monsel_reg(msc, mon_reg, mon_sel);

	_mpam_write_monsel_reg(msc, mon_reg, MSMON___NRDY);
	now = _mpam_read_monsel_reg(msc, mon_reg);
	can_set = now & MSMON___NRDY;

	_mpam_write_monsel_reg(msc, mon_reg, 0);
	now = _mpam_read_monsel_reg(msc, mon_reg);
	can_clear = !(now & MSMON___NRDY);
	mpam_mon_sel_unlock(msc);

	return (!can_set || !can_clear);
}

#define mpam_ris_hw_probe_hw_nrdy(_ris, _mon_reg)			\
        _mpam_ris_hw_probe_hw_nrdy(_ris, MSMON_##_mon_reg)

static void mpam_ris_hw_probe(struct mpam_msc_ris *ris)
{
	int err;
	struct mpam_msc *msc = ris->vmsc->msc;
	struct device *dev = &msc->pdev->dev;
	struct mpam_props *props = &ris->props;
	struct mpam_class *class = ris->vmsc->comp->class;

	lockdep_assert_held(&msc->probe_lock);
	lockdep_assert_held(&msc->part_sel_lock);

	/* Cache Capacity Partitioning */
	if (FIELD_GET(MPAMF_IDR_HAS_CCAP_PART, ris->idr)) {
		u32 ccap_features = mpam_read_partsel_reg(msc, CCAP_IDR);

		props->cmax_wd = FIELD_GET(MPAMF_CCAP_IDR_CMAX_WD, ccap_features);
		if (props->cmax_wd &&
		    FIELD_GET(MPAMF_CCAP_IDR_HAS_CMAX_SOFTLIM, ccap_features))
			mpam_set_feature(mpam_feat_cmax_softlim, props);

		if (props->cmax_wd &&
		    !FIELD_GET(MPAMF_CCAP_IDR_NO_CMAX, ccap_features))
			mpam_set_feature(mpam_feat_cmax_cmax, props);

		if (props->cmax_wd &&
		    FIELD_GET(MPAMF_CCAP_IDR_HAS_CMIN, ccap_features))
			mpam_set_feature(mpam_feat_cmax_cmin, props);

		props->cassoc_wd = FIELD_GET(MPAMF_CCAP_IDR_CASSOC_WD, ccap_features);

		if (props->cassoc_wd &&
		    FIELD_GET(MPAMF_CCAP_IDR_HAS_CASSOC, ccap_features))
			mpam_set_feature(mpam_feat_cmax_cassoc, props);
	}

	/* Cache Portion partitioning */
	if (FIELD_GET(MPAMF_IDR_HAS_CPOR_PART, ris->idr)) {
		u32 cpor_features = mpam_read_partsel_reg(msc, CPOR_IDR);

		props->cpbm_wd = FIELD_GET(MPAMF_CPOR_IDR_CPBM_WD, cpor_features);
		if (props->cpbm_wd)
			mpam_set_feature(mpam_feat_cpor_part, props);
	}

	/* Memory bandwidth partitioning */
	if (FIELD_GET(MPAMF_IDR_HAS_MBW_PART, ris->idr)) {
		u32 mbw_features = mpam_read_partsel_reg(msc, MBW_IDR);

		/* portion bitmap resolution */
		props->mbw_pbm_bits = FIELD_GET(MPAMF_MBW_IDR_BWPBM_WD, mbw_features);
		if (props->mbw_pbm_bits &&
		    FIELD_GET(MPAMF_MBW_IDR_HAS_PBM, mbw_features))
			mpam_set_feature(mpam_feat_mbw_part, props);

		props->bwa_wd = FIELD_GET(MPAMF_MBW_IDR_BWA_WD, mbw_features);
		if (props->bwa_wd && FIELD_GET(MPAMF_MBW_IDR_HAS_MAX, mbw_features))
			mpam_set_feature(mpam_feat_mbw_max, props);

		if (props->bwa_wd && FIELD_GET(MPAMF_MBW_IDR_HAS_MIN, mbw_features))
			mpam_set_feature(mpam_feat_mbw_min, props);

		if (props->bwa_wd && FIELD_GET(MPAMF_MBW_IDR_HAS_PROP, mbw_features))
			mpam_set_feature(mpam_feat_mbw_prop, props);
	}

	/* Priority partitioning */
	if (FIELD_GET(MPAMF_IDR_HAS_PRI_PART, ris->idr)) {
		u32 pri_features = mpam_read_partsel_reg(msc, PRI_IDR);

		props->intpri_wd = FIELD_GET(MPAMF_PRI_IDR_INTPRI_WD, pri_features);
		if (props->intpri_wd && FIELD_GET(MPAMF_PRI_IDR_HAS_INTPRI, pri_features)) {
			mpam_set_feature(mpam_feat_intpri_part, props);
			if (FIELD_GET(MPAMF_PRI_IDR_INTPRI_0_IS_LOW, pri_features))
				mpam_set_feature(mpam_feat_intpri_part_0_low, props);
		}

		props->dspri_wd = FIELD_GET(MPAMF_PRI_IDR_DSPRI_WD, pri_features);
		if (props->dspri_wd && FIELD_GET(MPAMF_PRI_IDR_HAS_DSPRI, pri_features)) {
			mpam_set_feature(mpam_feat_dspri_part, props);
			if (FIELD_GET(MPAMF_PRI_IDR_DSPRI_0_IS_LOW, pri_features))
				mpam_set_feature(mpam_feat_dspri_part_0_low, props);
		}
	}

	/* Performance Monitoring */
	if (FIELD_GET(MPAMF_IDR_HAS_MSMON, ris->idr)) {
		u32 msmon_features = mpam_read_partsel_reg(msc, MSMON_IDR);

		/*
		 * If the firmware max-nrdy-us property is missing, the
		 * CSU counters can't be used. Should we wait forever?
		 */
		err = device_property_read_u32(&msc->pdev->dev,
					       "arm,not-ready-us",
					       &msc->nrdy_usec);

		if (FIELD_GET(MPAMF_MSMON_IDR_MSMON_CSU, msmon_features)) {
			u32 csumonidr;

			csumonidr = mpam_read_partsel_reg(msc, CSUMON_IDR);
			props->num_csu_mon = FIELD_GET(MPAMF_CSUMON_IDR_NUM_MON, csumonidr);
			if (props->num_csu_mon) {
				bool hw_managed;

				mpam_set_feature(mpam_feat_msmon_csu, props);

				if (FIELD_GET(MPAMF_CSUMON_IDR_HAS_XCL, csumonidr))
					mpam_set_feature(mpam_feat_msmon_csu_xcl, props);

				/* Is NRDY hardware managed? */
				hw_managed = mpam_ris_hw_probe_hw_nrdy(ris, CSU);
				if (hw_managed)
					mpam_set_feature(mpam_feat_msmon_csu_hw_nrdy, props);
			}

			/*
			 * Accept the missing firmware property if NRDY appears
			 * un-implemented.
			 */
			if (err && mpam_has_feature(mpam_feat_msmon_csu_hw_nrdy, props))
				dev_err_once(dev, "Counters are not usable because not-ready timeout was not provided by firmware.");
		}
		if (FIELD_GET(MPAMF_MSMON_IDR_MSMON_MBWU, msmon_features)) {
			bool hw_managed;
			u32 mbwumon_idr = mpam_read_partsel_reg(msc, MBWUMON_IDR);

			props->num_mbwu_mon = FIELD_GET(MPAMF_MBWUMON_IDR_NUM_MON, mbwumon_idr);
			if (props->num_mbwu_mon)
				mpam_set_feature(mpam_feat_msmon_mbwu, props);

			if (FIELD_GET(MPAMF_MBWUMON_IDR_HAS_RWBW, mbwumon_idr))
				mpam_set_feature(mpam_feat_msmon_mbwu_rwbw, props);

			/* Is NRDY hardware managed? */
			hw_managed = mpam_ris_hw_probe_hw_nrdy(ris, MBWU);
			if (hw_managed)
				mpam_set_feature(mpam_feat_msmon_mbwu_hw_nrdy, props);

			/*
			 * Don't warn about any missing firmware property for
			 * MBWU NRDY - it doesn't make any sense!
			 */
		}
	}

	/*
	 * RIS with PARTID narrowing don't have enough storage for one
	 * configuration per PARTID. If these are in a class we could use,
	 * reduce the supported partid_max to match the number of intpartid.
	 * If the class is unknown, just ignore it.
	 */
	if (FIELD_GET(MPAMF_IDR_HAS_PARTID_NRW, ris->idr) &&
	    class->type != MPAM_CLASS_UNKNOWN) {
		u32 nrwidr = mpam_read_partsel_reg(msc, PARTID_NRW_IDR);
		u16 partid_max = FIELD_GET(MPAMF_PARTID_NRW_IDR_INTPARTID_MAX, nrwidr);

		mpam_set_feature(mpam_feat_partid_nrw, props);
		msc->partid_max = min(msc->partid_max, partid_max);
	}
}

static int mpam_msc_hw_probe(struct mpam_msc *msc)
{
	u64 idr;
	u16 partid_max;
	u8 ris_idx, pmg_max;
	struct mpam_msc_ris *ris;
	struct device *dev = &msc->pdev->dev;

	lockdep_assert_held(&msc->probe_lock);

	idr = __mpam_read_reg(msc, MPAMF_AIDR);
	if ((idr & MPAMF_AIDR_ARCH_MAJOR_REV) != MPAM_ARCHITECTURE_V1) {
		dev_err_once(dev, "MSC does not match MPAM architecture v1.x\n");
		return -EIO;
	}

	/* Grab an IDR value to find out how many RIS there are */
	mutex_lock(&msc->part_sel_lock);
	idr = mpam_msc_read_idr(msc);
	mutex_unlock(&msc->part_sel_lock);

	msc->ris_max = FIELD_GET(MPAMF_IDR_RIS_MAX, idr);

	/* Use these values so partid/pmg always starts with a valid value */
	msc->partid_max = FIELD_GET(MPAMF_IDR_PARTID_MAX, idr);
	msc->pmg_max = FIELD_GET(MPAMF_IDR_PMG_MAX, idr);

	for (ris_idx = 0; ris_idx <= msc->ris_max; ris_idx++) {
		mutex_lock(&msc->part_sel_lock);
		__mpam_part_sel(ris_idx, 0, msc);
		idr = mpam_msc_read_idr(msc);
		mutex_unlock(&msc->part_sel_lock);

		partid_max = FIELD_GET(MPAMF_IDR_PARTID_MAX, idr);
		pmg_max = FIELD_GET(MPAMF_IDR_PMG_MAX, idr);
		msc->partid_max = min(msc->partid_max, partid_max);
		msc->pmg_max = min(msc->pmg_max, pmg_max);
		msc->has_extd_esr = FIELD_GET(MPAMF_IDR_HAS_EXTD_ESR, idr);

		mutex_lock(&mpam_list_lock);
		ris = mpam_get_or_create_ris(msc, ris_idx);
		mutex_unlock(&mpam_list_lock);
		if (IS_ERR(ris))
			return PTR_ERR(ris);
		ris->idr = idr;

		mutex_lock(&msc->part_sel_lock);
		__mpam_part_sel(ris_idx, 0, msc);
		mpam_ris_hw_probe(ris);
		mutex_unlock(&msc->part_sel_lock);
	}

	/* Clear any stale errors */
	mpam_msc_zero_esr(msc);

	spin_lock(&partid_max_lock);
	mpam_partid_max = min(mpam_partid_max, msc->partid_max);
	mpam_pmg_max = min(mpam_pmg_max, msc->pmg_max);
	spin_unlock(&partid_max_lock);

	msc->probed = true;

	return 0;
}

static void mpam_reset_msc_bitmap(struct mpam_msc *msc, u16 reg, u16 wd)
{
	u32 num_words, msb;
	u32 bm = ~0;
	int i;

	lockdep_assert_held(&msc->part_sel_lock);

	if (wd == 0)
		return;

	/*
	 * Write all ~0 to all but the last 32bit-word, which may
	 * have fewer bits...
	 */
	num_words = DIV_ROUND_UP(wd, 32);
	for (i = 0; i < num_words - 1; i++, reg += sizeof(bm))
		__mpam_write_reg(msc, reg, bm);

	/*
	 * ....and then the last (maybe) partial 32bit word. When wd is a
	 * multiple of 32, msb should be 31 to write a full 32bit word.
	 */
	msb = (wd - 1) % 32;
	bm = GENMASK(msb, 0);
	__mpam_write_reg(msc, reg, bm);
}

/* Called via IPI. Call while holding an SRCU reference */
static void mpam_reprogram_ris_partid(struct mpam_msc_ris *ris, u16 partid,
				      struct mpam_config *cfg)
{
	u32 pri_val = 0;
	u16 cmax = MPAMCFG_CMAX_CMAX;
	struct mpam_msc *msc = ris->vmsc->msc;
	struct mpam_props *rprops = &ris->props;
	u16 dspri = GENMASK(rprops->dspri_wd, 0);
	u16 intpri = GENMASK(rprops->intpri_wd, 0);

	mutex_lock(&msc->part_sel_lock);
	__mpam_part_sel(ris->ris_idx, partid, msc);

	if (mpam_has_feature(mpam_feat_partid_nrw, rprops)) {
		/* Update the intpartid mapping */
		mpam_write_partsel_reg(msc, INTPARTID,
				       MPAMCFG_INTPARTID_INTERNAL | partid);

		/*
		 * Then switch to the 'internal' partid to update the
		 * configuration.
		 */
		__mpam_intpart_sel(ris->ris_idx, partid, msc);
	}

	if (mpam_has_feature(mpam_feat_cpor_part, rprops) &&
	    mpam_has_feature(mpam_feat_cpor_part, cfg)) {
		if (cfg->reset_cpbm)
			mpam_reset_msc_bitmap(msc, MPAMCFG_CPBM,
					      rprops->cpbm_wd);
		else
			mpam_write_partsel_reg(msc, CPBM, cfg->cpbm);
	}

	if (mpam_has_feature(mpam_feat_mbw_part, rprops) &&
	    mpam_has_feature(mpam_feat_mbw_part, cfg)) {
		if (cfg->reset_mbw_pbm)
			mpam_reset_msc_bitmap(msc, MPAMCFG_MBW_PBM,
					      rprops->mbw_pbm_bits);
		else
			mpam_write_partsel_reg(msc, MBW_PBM, cfg->mbw_pbm);
	}

	if (mpam_has_feature(mpam_feat_mbw_min, rprops) &&
	    mpam_has_feature(mpam_feat_mbw_min, cfg))
		mpam_write_partsel_reg(msc, MBW_MIN, 0);

	if (mpam_has_feature(mpam_feat_mbw_max, rprops) &&
	    mpam_has_feature(mpam_feat_mbw_max, cfg))
		mpam_write_partsel_reg(msc, MBW_MAX, cfg->mbw_max);

	if (mpam_has_feature(mpam_feat_mbw_prop, rprops) &&
	    mpam_has_feature(mpam_feat_mbw_prop, cfg))
		mpam_write_partsel_reg(msc, MBW_PROP, 0);

	if (mpam_has_feature(mpam_feat_cmax_cmax, rprops))
		mpam_write_partsel_reg(msc, CMAX, cmax);

	if (mpam_has_feature(mpam_feat_cmax_cmin, rprops))
		mpam_write_partsel_reg(msc, CMIN, 0);

	if (mpam_has_feature(mpam_feat_cmax_cassoc, rprops))
		mpam_write_partsel_reg(msc, CASSOC, MPAMCFG_CASSOC_CASSOC);

	if (mpam_has_feature(mpam_feat_intpri_part, rprops) ||
	    mpam_has_feature(mpam_feat_dspri_part, rprops)) {
		/* aces high? */
		if (!mpam_has_feature(mpam_feat_intpri_part_0_low, rprops))
			intpri = 0;
		if (!mpam_has_feature(mpam_feat_dspri_part_0_low, rprops))
			dspri = 0;

		if (mpam_has_feature(mpam_feat_intpri_part, rprops))
			pri_val |= FIELD_PREP(MPAMCFG_PRI_INTPRI, intpri);
		if (mpam_has_feature(mpam_feat_dspri_part, rprops))
			pri_val |= FIELD_PREP(MPAMCFG_PRI_DSPRI, dspri);

		mpam_write_partsel_reg(msc, PRI, pri_val);
	}

	mutex_unlock(&msc->part_sel_lock);
}

struct reprogram_ris {
	struct mpam_msc_ris *ris;
	struct mpam_config *cfg;
};

/* Call with MSC lock held */
static int mpam_reprogram_ris(void *_arg)
{
	u16 partid, partid_max;
	struct reprogram_ris *arg = _arg;
	struct mpam_msc_ris *ris = arg->ris;
	struct mpam_config *cfg = arg->cfg;

	if (ris->in_reset_state)
		return 0;

	spin_lock(&partid_max_lock);
	partid_max = mpam_partid_max;
	spin_unlock(&partid_max_lock);
	for (partid = 0; partid <= partid_max; partid++)
		mpam_reprogram_ris_partid(ris, partid, cfg);

	return 0;
}

static void mpam_init_reset_cfg(struct mpam_config *reset_cfg)
{
	memset(reset_cfg, 0, sizeof(*reset_cfg));

	reset_cfg->features = ~0;
	reset_cfg->cpbm = ~0;
	reset_cfg->mbw_pbm = ~0;
	reset_cfg->mbw_max = MPAMCFG_MBW_MAX_MAX;

	reset_cfg->reset_cpbm = true;
	reset_cfg->reset_mbw_pbm = true;
}

/*
 * Called via smp_call_on_cpu() to prevent migration, while still being
 * pre-emptible.
 */
static int mpam_reset_ris(void *arg)
{
	struct mpam_config reset_cfg;
	struct mpam_msc_ris *ris = arg;
	struct reprogram_ris reprogram_arg;

	if (ris->in_reset_state)
		return 0;

	mpam_init_reset_cfg(&reset_cfg);

	reprogram_arg.ris = ris;
	reprogram_arg.cfg = &reset_cfg;

	mpam_reprogram_ris(&reprogram_arg);

	return 0;
}

/*
 * Get the preferred CPU for this MSC. If it is accessible from this CPU,
 * this CPU is preferred. This can be preempted/migrated, it will only result
 * in more work.
 */
static int mpam_get_msc_preferred_cpu(struct mpam_msc *msc)
{
	int cpu = raw_smp_processor_id();

	if (cpumask_test_cpu(cpu, &msc->accessibility))
		return cpu;

	return cpumask_first_and(&msc->accessibility, cpu_online_mask);
}

static int mpam_touch_msc(struct mpam_msc *msc, int (*fn)(void *a), void *arg)
{
	lockdep_assert_irqs_enabled();
	lockdep_assert_cpus_held();
	mpam_assert_srcu_read_lock_held();

	return smp_call_on_cpu(mpam_get_msc_preferred_cpu(msc), fn, arg, true);
}

static void mpam_reset_msc(struct mpam_msc *msc, bool online)
{
	struct mpam_msc_ris *ris;

	mpam_assert_srcu_read_lock_held();

	list_for_each_entry_srcu(ris, &msc->ris, msc_list, srcu_read_lock_held(&mpam_srcu)) {
		mpam_touch_msc(msc, &mpam_reset_ris, ris);

		/*
		 * Set in_reset_state when coming online. The reset state
		 * for non-zero partid may be lost while the CPUs are offline.
		 */
		ris->in_reset_state = online;
	}
}

static void mpam_reprogram_msc(struct mpam_msc *msc)
{
	u16 partid;
	bool reset;
	struct mpam_config *cfg;
	struct mpam_msc_ris *ris;

	/*
	 * No lock for mpam_partid_max as partid_max_published has been
	 * set by mpam_enabled(), so the values can no longer change.
	 */
	mpam_assert_partid_sizes_fixed();

	guard(srcu)(&mpam_srcu);
	list_for_each_entry_srcu(ris, &msc->ris, msc_list,
				 srcu_read_lock_held(&mpam_srcu)) {
		if (!mpam_is_enabled() && !ris->in_reset_state) {
			mpam_touch_msc(msc, &mpam_reset_ris, ris);
			ris->in_reset_state = true;
			continue;
		}

		reset = true;
		for (partid = 0; partid <= mpam_partid_max; partid++) {
			cfg = &ris->vmsc->comp->cfg[partid];
			if (cfg->features)
				reset = false;

			mpam_reprogram_ris_partid(ris, partid, cfg);
		}
		ris->in_reset_state = reset;
	}
}

static void _enable_percpu_irq(void *_irq)
{
	int *irq = _irq;

	enable_percpu_irq(*irq, IRQ_TYPE_NONE);
}

static int mpam_cpu_online(unsigned int cpu)
{
	int idx;
	struct mpam_msc *msc;

	idx = srcu_read_lock(&mpam_srcu);
	list_for_each_entry_srcu(msc, &mpam_all_msc, all_msc_list,
				 srcu_read_lock_held(&mpam_srcu)) {
		if (!cpumask_test_cpu(cpu, &msc->accessibility))
			continue;

		if (msc->reenable_error_ppi)
			_enable_percpu_irq(&msc->reenable_error_ppi);

		if (atomic_fetch_inc(&msc->online_refs) == 0)
			mpam_reprogram_msc(msc);
	}
	srcu_read_unlock(&mpam_srcu, idx);

	return 0;
}

/* Before mpam is enabled, try to probe new MSC */
static int mpam_discovery_cpu_online(unsigned int cpu)
{
	int err = 0;
	struct mpam_msc *msc;
	bool new_device_probed = false;

	if (mpam_is_enabled())
		return 0;

	guard(srcu)(&mpam_srcu);
	list_for_each_entry_srcu(msc, &mpam_all_msc, all_msc_list,
				 srcu_read_lock_held(&mpam_srcu)) {
		if (!cpumask_test_cpu(cpu, &msc->accessibility))
			continue;

		mutex_lock(&msc->probe_lock);
		if (!msc->probed)
			err = mpam_msc_hw_probe(msc);
		mutex_unlock(&msc->probe_lock);

		if (!err)
			new_device_probed = true;
		else
			break;
	}

	if (new_device_probed && !err)
		schedule_work(&mpam_enable_work);
	if (err) {
		mpam_disable_reason = "error during probing";
		schedule_work(&mpam_broken_work);
	}

	return err;
}

static int mpam_cpu_offline(unsigned int cpu)
{
	int idx;
	struct mpam_msc *msc;

	idx = srcu_read_lock(&mpam_srcu);
	list_for_each_entry_srcu(msc, &mpam_all_msc, all_msc_list,
				 srcu_read_lock_held(&mpam_srcu)) {
		if (!cpumask_test_cpu(cpu, &msc->accessibility))
			continue;

		if (msc->reenable_error_ppi)
			disable_percpu_irq(msc->reenable_error_ppi);

		if (atomic_dec_and_test(&msc->online_refs))
			mpam_reset_msc(msc, false);
	}
	srcu_read_unlock(&mpam_srcu, idx);

	return 0;
}

static void mpam_register_cpuhp_callbacks(int (*online)(unsigned int online),
					  int (*offline)(unsigned int offline))
{
	mutex_lock(&mpam_cpuhp_state_lock);
	if (mpam_cpuhp_state) {
		cpuhp_remove_state(mpam_cpuhp_state);
		mpam_cpuhp_state = 0;
	}

	mpam_cpuhp_state = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "mpam:online",
					     online, offline);
	if (mpam_cpuhp_state <= 0) {
		pr_err("Failed to register cpuhp callbacks");
		mpam_cpuhp_state = 0;
	}
	mutex_unlock(&mpam_cpuhp_state_lock);
}

static int __setup_ppi(struct mpam_msc *msc)
{
	int cpu;
	struct device *dev = &msc->pdev->dev;

	msc->error_dev_id = alloc_percpu(struct mpam_msc *);
	if (!msc->error_dev_id)
		return -ENOMEM;

	for_each_cpu(cpu, &msc->accessibility) {
		struct mpam_msc *empty = *per_cpu_ptr(msc->error_dev_id, cpu);

		if (empty) {
			dev_err_once(dev, "MSC shares PPI with %s!\n",
				     dev_name(&empty->pdev->dev));
			return -EBUSY;
		}
		*per_cpu_ptr(msc->error_dev_id, cpu) = msc;
	}

	return 0;
}

static int mpam_msc_setup_error_irq(struct mpam_msc *msc)
{
	int irq;

	irq = platform_get_irq_byname_optional(msc->pdev, "error");
	if (irq <= 0)
		return 0;

	/* Allocate and initialise the percpu device pointer for PPI */
	if (irq_is_percpu(irq))
		return __setup_ppi(msc);

	/* sanity check: shared interrupts can be routed anywhere? */
	if (!cpumask_equal(&msc->accessibility, cpu_possible_mask)) {
		pr_err_once("msc:%u is a private resource with a shared error interrupt",
			    msc->id);
		return -EINVAL;
	}

	return 0;
}

/*
 * An MSC can control traffic from a set of CPUs, but may only be accessible
 * from a (hopefully wider) set of CPUs. The common reason for this is power
 * management. If all the CPUs in a cluster are in PSCI:CPU_SUSPEND, the
 * corresponding cache may also be powered off. By making accesses from
 * one of those CPUs, we ensure this isn't the case.
 */
static int update_msc_accessibility(struct mpam_msc *msc)
{
	u32 affinity_id;
	int err;

	err = device_property_read_u32(&msc->pdev->dev, "cpu_affinity",
				       &affinity_id);
	if (err)
		cpumask_copy(&msc->accessibility, cpu_possible_mask);
	else
		acpi_pptt_get_cpus_from_container(affinity_id,
						  &msc->accessibility);

	return 0;

	return err;
}

static int fw_num_msc;

static void mpam_msc_drv_remove(struct platform_device *pdev)
{
	struct mpam_msc *msc = platform_get_drvdata(pdev);

	if (!msc)
		return;

	mutex_lock(&mpam_list_lock);
	mpam_msc_destroy(msc);
	mutex_unlock(&mpam_list_lock);

	mpam_free_garbage();
}

static int mpam_msc_drv_probe(struct platform_device *pdev)
{
	int err;
	struct mpam_msc *msc;
	struct resource *msc_res;
	struct device *dev = &pdev->dev;
	void *plat_data = pdev->dev.platform_data;

	mutex_lock(&mpam_list_lock);
	do {
		msc = devm_kzalloc(&pdev->dev, sizeof(*msc), GFP_KERNEL);
		if (!msc) {
			err = -ENOMEM;
			break;
		}
		init_garbage(msc);

		mutex_init(&msc->probe_lock);
		mutex_init(&msc->part_sel_lock);
		mpam_mon_sel_lock_init(msc);
		msc->id = pdev->id;
		msc->pdev = pdev;
		INIT_LIST_HEAD_RCU(&msc->all_msc_list);
		INIT_LIST_HEAD_RCU(&msc->ris);

		err = update_msc_accessibility(msc);
		if (err)
			break;
		if (cpumask_empty(&msc->accessibility)) {
			dev_err_once(dev, "MSC is not accessible from any CPU!");
			err = -EINVAL;
			break;
		}

		err = mpam_msc_setup_error_irq(msc);
		if (err)
			break;

		if (device_property_read_u32(&pdev->dev, "pcc-channel",
					     &msc->pcc_subspace_id))
			msc->iface = MPAM_IFACE_MMIO;
		else
			msc->iface = MPAM_IFACE_PCC;

		if (msc->iface == MPAM_IFACE_MMIO) {
			void __iomem *io;

			io = devm_platform_get_and_ioremap_resource(pdev, 0,
								    &msc_res);
			if (IS_ERR(io)) {
				dev_err_once(dev, "Failed to map MSC base address\n");
				err = PTR_ERR(io);
				break;
			}
			msc->mapped_hwpage_sz = msc_res->end - msc_res->start;
			msc->mapped_hwpage = io;
		}

		list_add_rcu(&msc->all_msc_list, &mpam_all_msc);
		platform_set_drvdata(pdev, msc);
	} while (0);
	mutex_unlock(&mpam_list_lock);

	if (!err) {
		/* Create RIS entries described by firmware */
		err = acpi_mpam_parse_resources(msc, plat_data);
	}

	if (err && msc)
		mpam_msc_drv_remove(pdev);

	if (!err && atomic_add_return(1, &mpam_num_msc) == fw_num_msc)
		mpam_register_cpuhp_callbacks(mpam_discovery_cpu_online, NULL);

	return err;
}

static struct platform_driver mpam_msc_driver = {
	.driver = {
		.name = "mpam_msc",
	},
	.probe = mpam_msc_drv_probe,
	.remove = mpam_msc_drv_remove,
};

/* Any of these features mean the BWA_WD field is valid. */
static bool mpam_has_bwa_wd_feature(struct mpam_props *props)
{
	if (mpam_has_feature(mpam_feat_mbw_min, props))
		return true;
	if (mpam_has_feature(mpam_feat_mbw_max, props))
		return true;
	if (mpam_has_feature(mpam_feat_mbw_prop, props))
		return true;
	return false;
}

/* Any of these features mean the CMAX_WD field is valid. */
static bool mpam_has_cmax_wd_feature(struct mpam_props *props)
{
	if (mpam_has_feature(mpam_feat_cmax_cmax, props))
		return true;
	if (mpam_has_feature(mpam_feat_cmax_cmin, props))
		return true;
	return false;
}

#define MISMATCHED_HELPER(parent, child, helper, field, alias)		\
	helper(parent) &&						\
	((helper(child) && (parent)->field != (child)->field) ||	\
	 (!helper(child) && !(alias)))

#define MISMATCHED_FEAT(parent, child, feat, field, alias)		     \
	mpam_has_feature((feat), (parent)) &&				     \
	((mpam_has_feature((feat), (child)) && (parent)->field != (child)->field) || \
	 (!mpam_has_feature((feat), (child)) && !(alias)))

#define CAN_MERGE_FEAT(parent, child, feat, alias)			\
	(alias) && !mpam_has_feature((feat), (parent)) &&		\
	mpam_has_feature((feat), (child))

/*
 * Combine two props fields.
 * If this is for controls that alias the same resource, it is safe to just
 * copy the values over. If two aliasing controls implement the same scheme
 * a safe value must be picked.
 * For non-aliasing controls, these control different resources, and the
 * resulting safe value must be compatible with both. When merging values in
 * the tree, all the aliasing resources must be handled first.
 * On mismatch, parent is modified.
 */
static void __props_mismatch(struct mpam_props *parent,
			     struct mpam_props *child, bool alias)
{
	if (CAN_MERGE_FEAT(parent, child, mpam_feat_cpor_part, alias)) {
		parent->cpbm_wd = child->cpbm_wd;
	} else if (MISMATCHED_FEAT(parent, child, mpam_feat_cpor_part,
				   cpbm_wd, alias)) {
		pr_debug("%s cleared cpor_part\n", __func__);
		mpam_clear_feature(mpam_feat_cpor_part, &parent->features);
		parent->cpbm_wd = 0;
	}

	if (CAN_MERGE_FEAT(parent, child, mpam_feat_mbw_part, alias)) {
		parent->mbw_pbm_bits = child->mbw_pbm_bits;
	} else if (MISMATCHED_FEAT(parent, child, mpam_feat_mbw_part,
				   mbw_pbm_bits, alias)) {
		pr_debug("%s cleared mbw_part\n", __func__);
		mpam_clear_feature(mpam_feat_mbw_part, &parent->features);
		parent->mbw_pbm_bits = 0;
	}

	/* bwa_wd is a count of bits, fewer bits means less precision */
	if (alias && !mpam_has_bwa_wd_feature(parent) && mpam_has_bwa_wd_feature(child)) {
		parent->bwa_wd = child->bwa_wd;
	} else if (MISMATCHED_HELPER(parent, child, mpam_has_bwa_wd_feature,
				     bwa_wd, alias)) {
		pr_debug("%s took the min bwa_wd\n", __func__);
		parent->bwa_wd = min(parent->bwa_wd, child->bwa_wd);
	}

	if (alias && !mpam_has_cmax_wd_feature(parent) && mpam_has_cmax_wd_feature(child)) {
		parent->cmax_wd = child->cmax_wd;
	} else if (MISMATCHED_HELPER(parent, child, mpam_has_cmax_wd_feature,
				     cmax_wd, alias)) {
		pr_debug("%s took the min cmax_wd\n", __func__);
		parent->cmax_wd = min(parent->cmax_wd, child->cmax_wd);
	}

	if (CAN_MERGE_FEAT(parent, child, mpam_feat_cmax_cassoc, alias)) {
		parent->cassoc_wd = child->cassoc_wd;
	} else if (MISMATCHED_FEAT(parent, child, mpam_feat_cmax_cassoc,
				   cassoc_wd, alias)) {
		pr_debug("%s cleared cassoc_wd\n", __func__);
		mpam_clear_feature(mpam_feat_cmax_cassoc, &parent->features);
		parent->cassoc_wd = 0;
	}

	/* For num properties, take the minimum */
	if (CAN_MERGE_FEAT(parent, child, mpam_feat_msmon_csu, alias)) {
		parent->num_csu_mon = child->num_csu_mon;
	} else if (MISMATCHED_FEAT(parent, child, mpam_feat_msmon_csu,
				   num_csu_mon, alias)) {
		pr_debug("%s took the min num_csu_mon\n", __func__);
		parent->num_csu_mon = min(parent->num_csu_mon, child->num_csu_mon);
	}

	if (CAN_MERGE_FEAT(parent, child, mpam_feat_msmon_mbwu, alias)) {
		parent->num_mbwu_mon = child->num_mbwu_mon;
	} else if (MISMATCHED_FEAT(parent, child, mpam_feat_msmon_mbwu,
				   num_mbwu_mon, alias)) {
		pr_debug("%s took the min num_mbwu_mon\n", __func__);
		parent->num_mbwu_mon = min(parent->num_mbwu_mon, child->num_mbwu_mon);
	}

	if (CAN_MERGE_FEAT(parent, child, mpam_feat_intpri_part, alias)) {
		parent->intpri_wd = child->intpri_wd;
	} else if (MISMATCHED_FEAT(parent, child, mpam_feat_intpri_part,
				   intpri_wd, alias)) {
		pr_debug("%s took the min intpri_wd\n", __func__);
		parent->intpri_wd = min(parent->intpri_wd, child->intpri_wd);
	}

	if (CAN_MERGE_FEAT(parent, child, mpam_feat_dspri_part, alias)) {
		parent->dspri_wd = child->dspri_wd;
	} else if (MISMATCHED_FEAT(parent, child, mpam_feat_dspri_part,
				   dspri_wd, alias)) {
		pr_debug("%s took the min dspri_wd\n", __func__);
		parent->dspri_wd = min(parent->dspri_wd, child->dspri_wd);
	}

	/* TODO: alias support for these two */
	/* {int,ds}pri may not have differing 0-low behaviour */
	if (mpam_has_feature(mpam_feat_intpri_part, parent) &&
	    (!mpam_has_feature(mpam_feat_intpri_part, child) ||
	     mpam_has_feature(mpam_feat_intpri_part_0_low, parent) !=
	     mpam_has_feature(mpam_feat_intpri_part_0_low, child))) {
		pr_debug("%s cleared intpri_part\n", __func__);
		mpam_clear_feature(mpam_feat_intpri_part, &parent->features);
		mpam_clear_feature(mpam_feat_intpri_part_0_low, &parent->features);
	}
	if (mpam_has_feature(mpam_feat_dspri_part, parent) &&
	    (!mpam_has_feature(mpam_feat_dspri_part, child) ||
	     mpam_has_feature(mpam_feat_dspri_part_0_low, parent) !=
	     mpam_has_feature(mpam_feat_dspri_part_0_low, child))) {
		pr_debug("%s cleared dspri_part\n", __func__);
		mpam_clear_feature(mpam_feat_dspri_part, &parent->features);
		mpam_clear_feature(mpam_feat_dspri_part_0_low, &parent->features);
	}

	if (alias) {
		/* Merge features for aliased resources */
		parent->features |= child->features;
	} else {
		/* Clear missing features for non aliasing */
		parent->features &= child->features;
	}
}

/*
 * If a vmsc doesn't match class feature/configuration, do the right thing(tm).
 * For 'num' properties we can just take the minimum.
 * For properties where the mismatched unused bits would make a difference, we
 * nobble the class feature, as we can't configure all the resources.
 * e.g. The L3 cache is composed of two resources with 13 and 17 portion
 * bitmaps respectively.
 */
static void
__class_props_mismatch(struct mpam_class *class, struct mpam_vmsc *vmsc)
{
	struct mpam_props *cprops = &class->props;
	struct mpam_props *vprops = &vmsc->props;

	lockdep_assert_held(&mpam_list_lock); /* we modify class */

	pr_debug("%s: Merging features for class:0x%lx &= vmsc:0x%lx\n",
		 dev_name(&vmsc->msc->pdev->dev),
		 (long)cprops->features, (long)vprops->features);

	/* Take the safe value for any common features */
	__props_mismatch(cprops, vprops, false);
}

static void
__vmsc_props_mismatch(struct mpam_vmsc *vmsc, struct mpam_msc_ris *ris)
{
	struct mpam_props *rprops = &ris->props;
	struct mpam_props *vprops = &vmsc->props;

	lockdep_assert_held(&mpam_list_lock); /* we modify vmsc */

	pr_debug("%s: Merging features for vmsc:0x%lx |= ris:0x%lx\n",
		 dev_name(&vmsc->msc->pdev->dev),
		 (long)vprops->features, (long)rprops->features);

	/*
	 * Merge mismatched features - Copy any features that aren't common,
	 * but take the safe value for any common features.
	 */
	__props_mismatch(vprops, rprops, true);
}

/*
 * Copy the first component's first vMSC's properties and features to the
 * class. __class_props_mismatch() will remove conflicts.
 * It is not possible to have a class with no components, or a component with
 * no resources. The vMSC properties have already been built.
 */
static void mpam_enable_init_class_features(struct mpam_class *class)
{
	struct mpam_vmsc *vmsc;
	struct mpam_component *comp;

	comp = list_first_entry_or_null(&class->components,
					struct mpam_component, class_list);
	if (WARN_ON(!comp))
		return;

	vmsc = list_first_entry_or_null(&comp->vmsc,
					struct mpam_vmsc, comp_list);
	if (WARN_ON(!vmsc))
		return;

	class->props = vmsc->props;
}

static void mpam_enable_merge_vmsc_features(struct mpam_component *comp)
{
	struct mpam_vmsc *vmsc;
	struct mpam_msc_ris *ris;
	struct mpam_class *class = comp->class;

	list_for_each_entry(vmsc, &comp->vmsc, comp_list) {
		list_for_each_entry(ris, &vmsc->ris, vmsc_list) {
			__vmsc_props_mismatch(vmsc, ris);
			class->nrdy_usec = max(class->nrdy_usec,
					       vmsc->msc->nrdy_usec);
		}
	}
}

static void mpam_enable_merge_class_features(struct mpam_component *comp)
{
	struct mpam_vmsc *vmsc;
	struct mpam_class *class = comp->class;

	list_for_each_entry(vmsc, &comp->vmsc, comp_list)
		__class_props_mismatch(class, vmsc);
}

/*
 * Merge all the common resource features into class.
 * vmsc features are bitwise-or'd together, this must be done first.
 * Next the class features are the bitwise-and of all the vmsc features.
 * Other features are the min/max as appropriate.
 *
 * To avoid walking the whole tree twice, the class->nrdy_usec property is
 * updated when working with the vmsc as it is a max(), and doesn't need
 * initialising first.
 */
static void mpam_enable_merge_features(struct list_head *all_classes_list)
{
	struct mpam_class *class;
	struct mpam_component *comp;

	lockdep_assert_held(&mpam_list_lock);

	list_for_each_entry(class, all_classes_list, classes_list) {
		list_for_each_entry(comp, &class->components, class_list)
			mpam_enable_merge_vmsc_features(comp);

		mpam_enable_init_class_features(class);

		list_for_each_entry(comp, &class->components, class_list)
			mpam_enable_merge_class_features(comp);
	}
}

static char *mpam_errcode_names[16] = {
	[0] = "No error",
	[1] = "PARTID_SEL_Range",
	[2] = "Req_PARTID_Range",
	[3] = "MSMONCFG_ID_RANGE",
	[4] = "Req_PMG_Range",
	[5] = "Monitor_Range",
	[6] = "intPARTID_Range",
	[7] = "Unexpected_INTERNAL",
	[8] = "Undefined_RIS_PART_SEL",
	[9] = "RIS_No_Control",
	[10] = "Undefined_RIS_MON_SEL",
	[11] = "RIS_No_Monitor",
	[12 ... 15] = "Reserved"
};

static int mpam_enable_msc_ecr(void *_msc)
{
	struct mpam_msc *msc = _msc;

	__mpam_write_reg(msc, MPAMF_ECR, MPAMF_ECR_INTEN);

	return 0;
}

/* This can run in mpam_disable(), and the interrupt handler on the same CPU */
static int mpam_disable_msc_ecr(void *_msc)
{
	struct mpam_msc *msc = _msc;

	__mpam_write_reg(msc, MPAMF_ECR, 0);

	return 0;
}

static irqreturn_t __mpam_irq_handler(int irq, struct mpam_msc *msc)
{
	u64 reg;
	u16 partid;
	u8 errcode, pmg, ris;

	if (WARN_ON_ONCE(!msc) ||
	    WARN_ON_ONCE(!cpumask_test_cpu(smp_processor_id(),
					   &msc->accessibility)))
		return IRQ_NONE;

	reg = mpam_msc_read_esr(msc);

	errcode = FIELD_GET(MPAMF_ESR_ERRCODE, reg);
	if (!errcode)
		return IRQ_NONE;

	/* Clear level triggered irq */
	mpam_msc_zero_esr(msc);

	partid = FIELD_GET(MPAMF_ESR_PARTID_MON, reg);
	pmg = FIELD_GET(MPAMF_ESR_PMG, reg);
	ris = FIELD_GET(MPAMF_ESR_RIS, reg);

	pr_err_ratelimited("error irq from msc:%u '%s', partid:%u, pmg: %u, ris: %u\n",
			   msc->id, mpam_errcode_names[errcode], partid, pmg,
			   ris);

	/* Disable this interrupt. */
	mpam_disable_msc_ecr(msc);

	/* Are we racing with the thread disabling MPAM? */
	if (!mpam_is_enabled())
		return IRQ_HANDLED;

	/*
	 * Schedule the teardown work. Don't use a threaded IRQ as we can't
	 * unregister the interrupt from the threaded part of the handler.
	 */
	mpam_disable_reason = "hardware error interrupt";
	schedule_work(&mpam_broken_work);

	return IRQ_HANDLED;
}

static irqreturn_t mpam_ppi_handler(int irq, void *dev_id)
{
	struct mpam_msc *msc = *(struct mpam_msc **)dev_id;

	return __mpam_irq_handler(irq, msc);
}

static irqreturn_t mpam_spi_handler(int irq, void *dev_id)
{
	struct mpam_msc *msc = dev_id;

	return __mpam_irq_handler(irq, msc);
}

static int mpam_register_irqs(void)
{
	int err, irq;
	struct mpam_msc *msc;

	lockdep_assert_cpus_held();

	guard(srcu)(&mpam_srcu);
	list_for_each_entry_srcu(msc, &mpam_all_msc, all_msc_list,
				 srcu_read_lock_held(&mpam_srcu)) {
		irq = platform_get_irq_byname_optional(msc->pdev, "error");
		if (irq <= 0)
			continue;

		/* The MPAM spec says the interrupt can be SPI, PPI or LPI */
		/* We anticipate sharing the interrupt with other MSCs */
		if (irq_is_percpu(irq)) {
			err = request_percpu_irq(irq, &mpam_ppi_handler,
						 "mpam:msc:error",
						 msc->error_dev_id);
			if (err)
				return err;

			msc->reenable_error_ppi = irq;
			smp_call_function_many(&msc->accessibility,
					       &_enable_percpu_irq, &irq,
					       true);
		} else {
			err = devm_request_irq(&msc->pdev->dev,irq,
					       &mpam_spi_handler, IRQF_SHARED,
					       "mpam:msc:error", msc);
			if (err)
				return err;
		}

		set_bit(MPAM_ERROR_IRQ_REQUESTED, &msc->error_irq_flags);
		mpam_touch_msc(msc, mpam_enable_msc_ecr, msc);
		set_bit(MPAM_ERROR_IRQ_HW_ENABLED, &msc->error_irq_flags);
	}

	return 0;
}

static void mpam_unregister_irqs(void)
{
	int irq, idx;
	struct mpam_msc *msc;

	cpus_read_lock();
	/* take the lock as free_irq() can sleep */
	idx = srcu_read_lock(&mpam_srcu);
	list_for_each_entry_srcu(msc, &mpam_all_msc, all_msc_list,
				 srcu_read_lock_held(&mpam_srcu)) {
		irq = platform_get_irq_byname_optional(msc->pdev, "error");
		if (irq <= 0)
			continue;

		if (test_and_clear_bit(MPAM_ERROR_IRQ_HW_ENABLED, &msc->error_irq_flags))
			mpam_touch_msc(msc, mpam_disable_msc_ecr, msc);

		if (test_and_clear_bit(MPAM_ERROR_IRQ_REQUESTED, &msc->error_irq_flags)) {
			if (irq_is_percpu(irq)) {
				msc->reenable_error_ppi = 0;
				free_percpu_irq(irq, msc->error_dev_id);
			} else {
				devm_free_irq(&msc->pdev->dev, irq, msc);
			}
		}
	}
	srcu_read_unlock(&mpam_srcu, idx);
	cpus_read_unlock();
}

static void __destroy_component_cfg(struct mpam_component *comp)
{
	add_to_garbage(comp->cfg);
}

static int __allocate_component_cfg(struct mpam_component *comp)
{
	mpam_assert_partid_sizes_fixed();

	if (comp->cfg)
		return 0;

	comp->cfg = kcalloc(mpam_partid_max + 1, sizeof(*comp->cfg), GFP_KERNEL);
	if (!comp->cfg)
		return -ENOMEM;
	init_garbage(comp->cfg);

	return 0;
}

static int mpam_allocate_config(void)
{
	int err = 0;
	struct mpam_class *class;
	struct mpam_component *comp;

	lockdep_assert_held(&mpam_list_lock);

	list_for_each_entry(class, &mpam_classes, classes_list) {
		list_for_each_entry(comp, &class->components, class_list) {
			err = __allocate_component_cfg(comp);
			if (err)
				return err;
		}
	}

	return 0;
}

static void mpam_enable_once(void)
{
	int err;

	/*
	 * Once the cpuhp callbacks have been changed, mpam_partid_max can no
	 * longer change.
	 */
	spin_lock(&partid_max_lock);
	partid_max_published = true;
	spin_unlock(&partid_max_lock);

	/*
	 * If all the MSC have been probed, enabling the IRQs happens next.
	 * That involves cross-calling to a CPU that can reach the MSC, and
	 * the locks must be taken in this order:
	 */
	cpus_read_lock();
	mutex_lock(&mpam_list_lock);
	do {
		mpam_enable_merge_features(&mpam_classes);

		err = mpam_register_irqs();
		if (err) {
			pr_warn("Failed to register irqs: %d\n", err);
			break;
		}

		err = mpam_allocate_config();
		if (err) {
			pr_err("Failed to allocate configuration arrays.\n");
			break;
		}
	} while (0);
	mutex_unlock(&mpam_list_lock);
	cpus_read_unlock();

	if (err) {
		schedule_work(&mpam_broken_work);
		return;
	}

	static_branch_enable(&mpam_enabled);
	mpam_register_cpuhp_callbacks(mpam_cpu_online, mpam_cpu_offline);

	printk(KERN_INFO "MPAM enabled with %u PARTIDs and %u PMGs\n",
	       mpam_partid_max + 1, mpam_pmg_max + 1);
}

static void mpam_reset_component_locked(struct mpam_component *comp)
{
	struct mpam_msc *msc;
	struct mpam_vmsc *vmsc;
	struct mpam_msc_ris *ris;

	lockdep_assert_cpus_held();
	mpam_assert_partid_sizes_fixed();

	memset(comp->cfg, 0, (mpam_partid_max * sizeof(*comp->cfg)));

	guard(srcu)(&mpam_srcu);
	list_for_each_entry_srcu(vmsc, &comp->vmsc, comp_list,
				 srcu_read_lock_held(&mpam_srcu)) {
		msc = vmsc->msc;

		list_for_each_entry_srcu(ris, &vmsc->ris, vmsc_list,
					 srcu_read_lock_held(&mpam_srcu)) {
			if (!ris->in_reset_state)
				mpam_touch_msc(msc, mpam_reset_ris, ris);
			ris->in_reset_state = true;
		}
	}
}

static void mpam_reset_class_locked(struct mpam_class *class)
{
	struct mpam_component *comp;

	lockdep_assert_cpus_held();

	guard(srcu)(&mpam_srcu);
	list_for_each_entry_srcu(comp, &class->components, class_list,
				 srcu_read_lock_held(&mpam_srcu))
		mpam_reset_component_locked(comp);
}

static void mpam_reset_class(struct mpam_class *class)
{
	cpus_read_lock();
	mpam_reset_class_locked(class);
	cpus_read_unlock();
}

/*
 * Called in response to an error IRQ.
 * All of MPAMs errors indicate a software bug, restore any modified
 * controls to their reset values.
 */
void mpam_disable(struct work_struct *ignored)
{
	int idx;
	struct mpam_class *class;
	struct mpam_msc *msc, *tmp;

	mutex_lock(&mpam_cpuhp_state_lock);
	if (mpam_cpuhp_state) {
		cpuhp_remove_state(mpam_cpuhp_state);
		mpam_cpuhp_state = 0;
	}
	mutex_unlock(&mpam_cpuhp_state_lock);

	static_branch_disable(&mpam_enabled);

	mpam_unregister_irqs();

	idx = srcu_read_lock(&mpam_srcu);
	list_for_each_entry_srcu(class, &mpam_classes, classes_list,
				 srcu_read_lock_held(&mpam_srcu))
		mpam_reset_class(class);
	srcu_read_unlock(&mpam_srcu, idx);

	mutex_lock(&mpam_list_lock);
	list_for_each_entry_safe(msc, tmp, &mpam_all_msc, all_msc_list)
		mpam_msc_destroy(msc);
	mutex_unlock(&mpam_list_lock);
	mpam_free_garbage();

	pr_err_once("MPAM disabled due to %s\n", mpam_disable_reason);
}

/*
 * Enable mpam once all devices have been probed.
 * Scheduled by mpam_discovery_cpu_online() once all devices have been created.
 * Also scheduled when new devices are probed when new CPUs come online.
 */
void mpam_enable(struct work_struct *work)
{
	static atomic_t once;
	struct mpam_msc *msc;
	bool all_devices_probed = true;

	/* Have we probed all the hw devices? */
	guard(srcu)(&mpam_srcu);
	list_for_each_entry_srcu(msc, &mpam_all_msc, all_msc_list,
				 srcu_read_lock_held(&mpam_srcu)) {
		mutex_lock(&msc->probe_lock);
		if (!msc->probed)
			all_devices_probed = false;
		mutex_unlock(&msc->probe_lock);

		if (!all_devices_probed)
			break;
	}

	if (all_devices_probed && !atomic_fetch_inc(&once))
		mpam_enable_once();
}

struct mpam_write_config_arg {
	struct mpam_msc_ris *ris;
	struct mpam_component *comp;
	u16 partid;
};

static int __write_config(void *arg)
{
	struct mpam_write_config_arg *c = arg;

	mpam_reprogram_ris_partid(c->ris, c->partid, &c->comp->cfg[c->partid]);

	return 0;
}

#define maybe_update_config(cfg, feature, newcfg, member, changes) do { \
	if (mpam_has_feature(feature, newcfg) &&			\
	    (newcfg)->member != (cfg)->member) {			\
		(cfg)->member = (newcfg)->member;			\
		cfg->features |= (1 << feature);			\
									\
		(changes) |= (1 << feature);				\
	}								\
} while (0)

static mpam_features_t mpam_update_config(struct mpam_config *cfg,
					  const struct mpam_config *newcfg)
{
	mpam_features_t changes = 0;

	maybe_update_config(cfg, mpam_feat_cpor_part, newcfg, cpbm, changes);
	maybe_update_config(cfg, mpam_feat_mbw_part, newcfg, mbw_pbm, changes);
	maybe_update_config(cfg, mpam_feat_mbw_max, newcfg, mbw_max, changes);

	return changes;
}

int mpam_apply_config(struct mpam_component *comp, u16 partid,
		      struct mpam_config *cfg)
{
	struct mpam_write_config_arg arg;
	struct mpam_msc_ris *ris;
	struct mpam_vmsc *vmsc;
	struct mpam_msc *msc;

	lockdep_assert_cpus_held();

	/* Don't pass in the current config! */
	WARN_ON_ONCE(&comp->cfg[partid] == cfg);

	if (!mpam_update_config(&comp->cfg[partid], cfg))
		return 0;

	arg.comp = comp;
	arg.partid = partid;

	guard(srcu)(&mpam_srcu);
	list_for_each_entry_srcu(vmsc, &comp->vmsc, comp_list,
				 srcu_read_lock_held(&mpam_srcu)) {
		msc = vmsc->msc;

		list_for_each_entry_srcu(ris, &vmsc->ris, vmsc_list,
					 srcu_read_lock_held(&mpam_srcu)) {
			arg.ris = ris;
			mpam_touch_msc(msc, __write_config, &arg);
		}
	}

	return 0;
}

static int __init mpam_msc_driver_init(void)
{
	if (!system_supports_mpam())
		return -EOPNOTSUPP;

	init_srcu_struct(&mpam_srcu);

	fw_num_msc = acpi_mpam_count_msc();

	if (fw_num_msc <= 0) {
		pr_err("No MSC devices found in firmware\n");
		return -EINVAL;
	}

	return platform_driver_register(&mpam_msc_driver);
}
/* Must occur after arm64_mpam_register_cpus() from arch_initcall() */
subsys_initcall(mpam_msc_driver_init);
