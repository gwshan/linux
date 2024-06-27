// SPDX-License-Identifier: GPL-2.0
/*
 * debug.h - debugging relative helpers
 *
 * Copyright 2024 Gavin Shan <gshan@redhat.com> Redhat Inc.
 */

#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <linux/mm.h>

#define KERN_DBG_PROCESS	"testsuite"
#define KERN_DBG_FILENAME	"data"
#define KERN_DBG_VMA_LENGTH	0x10000

#define KERN_DBG(on, args...)			\
	do {					\
		if ((on))			\
			printk(KERN_INFO args);	\
	} while (0)

static inline bool is_debug_process(void)
{
	if (current && !strcmp(current->comm, KERN_DBG_PROCESS))
		return true;

	return false;
}

static inline bool is_debug_file(struct file *file)
{
	struct dentry *dentry = file ? file->f_path.dentry : NULL;

	if (!dentry)
		return false;

	if (!dentry->d_name.name ||
	    strcmp(dentry->d_name.name, KERN_DBG_FILENAME))
		return false;

	return true;
}

static inline bool is_debug_vma(struct vm_area_struct *vma)
{
	if (!vma)
		return false;

	if (vma_is_anonymous(vma))
		return false;

	if (vma->vm_end - vma->vm_start != KERN_DBG_VMA_LENGTH)
		return false;

	return is_debug_file(vma->vm_file);
}

#endif /* _DEBUG_H_ */
