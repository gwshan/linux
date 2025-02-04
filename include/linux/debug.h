/* SPDX-License-Identifier: GPL-2.0 */
/*
 * debug.h - debugging relevant helpers
 *
 * Copyright 2024 Gavin Shan <gshan@redhat.com> Redhat Inc.
 */

#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <linux/mm.h>

#define KERN_DBG_PROCESS	"test"
#define KERN_DBG_FILENAME	"data"
#define KERN_DBG_VMA_LENGTH	PMD_SIZE

#define KERN_DBG(enabled, args...)		\
	do {					\
		if ((enabled))			\
			pr_info(args);		\
	} while (0)

static inline bool is_debug_process(void)
{
	int ret;

	if (!current || !current->comm ||
	    strlen(current->comm) != strlen(KERN_DBG_PROCESS)
		return false;

	return !strncmp(current->comm, KERN_DBG_PROCESS,
			strlen(KERN_DBG_PROCESS));
}

static inline bool is_debug_file(struct file *file)
{
	struct dentry *dentry = file ? file->f_path.dentry : NULL;
	int ret;

	if (!dentry || !dentry->d_name.name ||
	    strlen(dentry->d_name.name) != strlen(KERN_DBG_FILENAME))
		return false;

	return !strncmp(dentry->d_name.name, KERN_DBG_FILENAME,
			strlen(KERN_DBG_FILENAME));
}

static inline bool is_debug_vma(struct vm_area_struct *vma,
				bool is_anon_vma)
{
	if (!vma)
		return false;

	if (vma->vm_end - vma->vm_start != KERN_DBG_VMA_LENGTH)
		return false;

	if (is_anon_vma && vma_is_anonymous(vma))
		return true;

	if (!is_anon_vma && is_debug_file(vma->vm_file))
		return true;

	return false;
}

#endif /* _DEBUG_H_ */
