// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver to export the contiguous memory block, which will be mapped
 * to userspace. With it, the userspace program can come up with cache
 * evictions by reading or writing to the memory block and then measure
 * the benchmarks.
 */

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>

#define DRIVER_VERSION	"0.1"
#define DRIVER_AUTHOR	"Gavin Shan, Redhat Inc"
#define DRIVER_DESC	"Export Memory for Read/Write"

/*
 * The reserved memory size is 256MB, expected to be larger than
 * two times of LLC size. It means LLC size shouldn't exceed 128MB.
 * Otherwise, this size should be enlarged accordingly. It should
 * be sufficient for the available machines.
 *
 * grace-hopper01	114MB LLC size
 * octeon10		 48MB LLC size
 */
#define TEST_CACHE_MEM_SIZE	0x10000000	/* 256MB */

struct test_cache {
	atomic_t	users;
	bool		contig_pages;
	int		nid;
	int		nr_pages;
	struct page	*page;
};

static struct test_cache *test;

static int test_cache_open(struct inode *inode, struct file *filp)
{
	/* Multiple accessors aren't allowed */
	if (atomic_dec_if_positive(&test->users) < 0) {
		pr_warn("%s: Device has been opened by other users\n", __func__);
		return -EIO;
	}

	return 0;
}

static vm_fault_t test_cache_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;

	pr_warn("%s: unhandled page fault\n", __func__);
	pr_warn("\n");
	pr_warn("%s: fault address 0x%lx, flags 0x%x\n",
		__func__, vmf->address, vmf->flags);
	pr_warn("%s: vma=[0x%lx  0x%lx] flags=0x%lx prot=0x%llx\n",
		__func__, vma->vm_start, vma->vm_end,
		vma->vm_flags, pgprot_val(vma->vm_page_prot));

	return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct test_cache_vm_ops = {
	.fault = test_cache_fault,
};

static int test_cache_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	int ret;

	if (!IS_ALIGNED(vma->vm_start, PAGE_SIZE)) {
		pr_warn("%s: start address 0x%lx isn't PAGE_SIZE aligned\n",
			__func__, vma->vm_start);
		return -EINVAL;
	}

	if (!IS_ALIGNED(size, size)) {
		pr_warn("%s: address range (0x%lx 0x%lx) isn't properly aligned\n",
			__func__, vma->vm_start, vma->vm_end);
		return -EINVAL;
	}

	if (size > (test->nr_pages << PAGE_SHIFT)) {
		pr_warn("%s: address range size 0x%lx exceeds limit 0x%lx\n",
			__func__, size, ((unsigned long)(test->nr_pages) << PAGE_SHIFT));
		return -EINVAL;
	}

	/*
	 * The vma's flags will be modified by remap_pfn_range() and
	 * it requires mm->mmap_lock is taken as a writer. The condition
	 * is false when CONFIG_PER_VMA_LOCK is enabled. This will lead
	 * to unexpected warning when remap_pfn_range() is called in the
	 * fault handler.
	 *
	 * Since mm->mmap_lock has been taken as a writer in the mapping
	 * path, so the vma is populated by remap_pfn_range() in this
	 * path.
	 */
	vma->vm_ops = &test_cache_vm_ops;
	ret = remap_pfn_range(vma, vma->vm_start, page_to_pfn(test->page),
			      size, vma->vm_page_prot);
	if (ret) {
		pr_warn("%s: Error %d from remap_pfn_range()\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

static int test_cache_release(struct inode *inode, struct file *filp)
{
	/* Mark the file has been closed */
	atomic_inc(&test->users);

	return 0;
}

static const struct file_operations test_cache_fops = {
	.open      = test_cache_open,
	.read      = NULL,
	.write     = NULL,
	.mmap      = test_cache_mmap,
	.release   = test_cache_release,
};

static struct miscdevice test_cache_dev = {
	.minor     = MISC_DYNAMIC_MINOR,
	.name      = "test_cache",
	.fops      = &test_cache_fops,
};

static int __init test_cache_init(void)
{
	int ret = 0;

	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test) {
		pr_warn("%s: Unable to alloc memory\n", __func__);
		return -ENOMEM;
	}

	/*
	 * Initialize test struct. Only one user is allowed to
	 * open the device at once.
	 */
	atomic_set(&test->users, 1);
	test->nid = 0;
	test->nr_pages = TEST_CACHE_MEM_SIZE / PAGE_SIZE;
	test->page = NULL;
	test->contig_pages = (test->nr_pages > MAX_ORDER_NR_PAGES) ? true : false;

	/* Allocate memory */
	if (test->contig_pages) {
		test->page = alloc_contig_pages(test->nr_pages,
					GFP_KERNEL | __GFP_THISNODE | __GFP_NOWARN,
					test->nid, NULL);
	} else {
		test->page = alloc_pages_node(test->nid,
					GFP_HIGHUSER_MOVABLE,
					ilog2(test->nr_pages));
	}

	if (!test->page) {
		pr_warn("%s: Unable to alloc memory\n", __func__);
		goto free_test_struct;
	}

	ret = misc_register(&test_cache_dev);
	if (ret) {
		pr_warn("%s: Error %d to register device\n", __func__, ret);
		goto free_pages;
	}

	return 0;

free_pages:
	if (test->contig_pages)
		free_contig_range(page_to_pfn(test->page), test->nr_pages);
	else
		__free_pages(test->page, ilog2(test->nr_pages));
free_test_struct:
	kfree(test);
	return ret;
}

static void __exit test_cache_exit(void)
{
	misc_deregister(&test_cache_dev);

	if (test->contig_pages)
		free_contig_range(page_to_pfn(test->page), test->nr_pages);
	else
		__free_pages(test->page, ilog2(test->nr_pages));

	kfree(test);
}

module_init(test_cache_init);
module_exit(test_cache_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
