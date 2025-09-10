/* SPDX-License-Identifier: GPL-2.0 */
// Copyright (C) 2025 Arm Ltd.

#ifndef MPAM_INTERNAL_H
#define MPAM_INTERNAL_H

#include <linux/arm_mpam.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/mailbox_client.h>
#include <linux/mutex.h>
#include <linux/resctrl.h>
#include <linux/sizes.h>

struct mpam_msc {
	/* member of mpam_all_msc */
	struct list_head        all_msc_list;

	int			id;
	struct platform_device *pdev;

	/* Not modified after mpam_is_enabled() becomes true */
	enum mpam_msc_iface	iface;
	u32			pcc_subspace_id;
	struct mbox_client	pcc_cl;
	struct pcc_mbox_chan	*pcc_chan;
	u32			nrdy_usec;
	cpumask_t		accessibility;

	/*
	 * probe_lock is only taken during discovery. After discovery these
	 * properties become read-only and the lists are protected by SRCU.
	 */
	struct mutex		probe_lock;
	unsigned long		ris_idxs;
	u32			ris_max;

	/* mpam_msc_ris of this component */
	struct list_head	ris;

	/*
	 * part_sel_lock protects access to the MSC hardware registers that are
	 * affected by MPAMCFG_PART_SEL. (including the ID registers that vary
	 * by RIS).
	 * If needed, take msc->probe_lock first.
	 */
	struct mutex		part_sel_lock;

	/*
	 * mon_sel_lock protects access to the MSC hardware registers that are
	 * affected by MPAMCFG_MON_SEL.
	 * If needed, take msc->probe_lock first.
	 */
	struct mutex		outer_mon_sel_lock;
	raw_spinlock_t		inner_mon_sel_lock;
	unsigned long		inner_mon_sel_flags;

	void __iomem		*mapped_hwpage;
	size_t			mapped_hwpage_sz;
};

int mpam_get_cpumask_from_cache_id(unsigned long cache_id, u32 cache_level,
				   cpumask_t *affinity);

#endif /* MPAM_INTERNAL_H */
