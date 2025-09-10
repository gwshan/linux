// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Arm Ltd.

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/arm_mpam.h>
#include <linux/cacheinfo.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gfp.h>
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

static void mpam_comp_destroy(struct mpam_component *comp)
{
	struct mpam_class *class = comp->class;

	lockdep_assert_held(&mpam_list_lock);

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

static int mpam_msc_hw_probe(struct mpam_msc *msc)
{
	u64 idr;
	struct device *dev = &msc->pdev->dev;

	lockdep_assert_held(&msc->probe_lock);

	idr = __mpam_read_reg(msc, MPAMF_AIDR);
	if ((idr & MPAMF_AIDR_ARCH_MAJOR_REV) != MPAM_ARCHITECTURE_V1) {
		dev_err_once(dev, "MSC does not match MPAM architecture v1.x\n");
		return -EIO;
	}

	msc->probed = true;

	return 0;
}

static int mpam_cpu_online(unsigned int cpu)
{
	return 0;
}

/* Before mpam is enabled, try to probe new MSC */
static int mpam_discovery_cpu_online(unsigned int cpu)
{
	int err = 0;
	struct mpam_msc *msc;
	bool new_device_probed = false;

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
		mutex_init(&msc->outer_mon_sel_lock);
		raw_spin_lock_init(&msc->inner_mon_sel_lock);
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

static void mpam_enable_once(void)
{
	mpam_register_cpuhp_callbacks(mpam_cpu_online, mpam_cpu_offline);

	pr_info("MPAM enabled\n");
}

void mpam_disable(struct work_struct *ignored)
{
	struct mpam_msc *msc, *tmp;

	mutex_lock(&mpam_cpuhp_state_lock);
	if (mpam_cpuhp_state) {
		cpuhp_remove_state(mpam_cpuhp_state);
		mpam_cpuhp_state = 0;
	}
	mutex_unlock(&mpam_cpuhp_state_lock);

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
subsys_initcall(mpam_msc_driver_init);
