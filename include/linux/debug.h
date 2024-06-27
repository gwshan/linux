// SPDX-License-Identifier: GPL-2.0
/*
 * debug.h - debugging relavant helpers
 *
 * Copyright 2024 Gavin Shan <gshan@redhat.com> Redhat Inc.
 */

#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <linux/mm.h>

#define KERN_DBG_PROCESS	"test"
#define KERN_DBG_FILENAME	"data"
#define KERN_DBG_VMA_LENGTH	PMD_SIZE

#define KERN_DBG(on, args...)			\
	do {					\
		if ((on))			\
			printk(KERN_INFO args);	\
	} while (0)

static inline bool is_debug_process(void)
{
	int ret;

	if (!current)
		return false;

	ret = strncmp(current->comm, KERN_DBG_PROCESS,
		      strlen(KERN_DBG_PROCESS));
	if (ret)
		return false;

	return true;
}

static inline bool is_debug_file(struct file *file)
{
	struct dentry *dentry = file ? file->f_path.dentry : NULL;
	int ret;

	if (!dentry || !dentry->d_name.name)
		return false;

	ret = strncmp(dentry->d_name.name, KERN_DBG_FILENAME,
		      strlen(KERN_DBG_FILENAME));
	if (ret)
		return false;

	return true;
}

static inline bool is_debug_vma(struct vm_area_struct *vma, bool anon_vma)
{
	if (!vma)
		return false;

	if (vma->vm_end - vma->vm_start != KERN_DBG_VMA_LENGTH)
		return false;

	if (anon_vma && vma_is_anonymous(vma))
		return true;

	if (!anon_vma && is_debug_file(vma->vm_file))
		return true;

	return false;
}

#endif /* _DEBUG_H_ */
