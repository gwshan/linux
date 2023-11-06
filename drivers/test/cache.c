// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver to export the contiguous memory, which will be mapped to user
 * space. With it, the user space is able to measure the benchmarks,
 * affected by page-to-cache coloring.
 *
 * Author: Gavin Shan <gshan@redhat.com>
 */

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>

#define DRIVER_VERSION	"0.1"
#define DRIVER_AUTHOR	"Gavin Shan, Redhat Inc"
#define DRIVER_DESC	"Export Memory for Read/Write"

struct test_cache {
	struct mutex	mutex;
	bool		opened;
	int		nid;
};

static struct test_cache *test;

static int test_cache_open(struct inode *inode, struct file *filp)
{
	/* Multiple accessors aren't allowed */
	mutex_lock(&test->mutex);
	if (test->opened) {
		mutex_unlock(&test->mutex);
		return -EPERM;
	}

	test->opened = true;

	mutex_unlock(&test->mutex);
	return 0;
}

static vm_fault_t test_cache_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct page *page = NULL;
	unsigned long addr = vma->vm_start;
	unsigned long size = ALIGN(vma->vm_end - vma->vm_start, PAGE_SIZE);
	pteval_t prot = pgprot_val(vma->vm_page_prot);
	int ret;

	/* Allocate physical memory */
	page = alloc_pages_node(test->nid,
				GFP_HIGHUSER_MOVABLE, ilog2(size) - PAGE_SHIFT);
	if (!page)
		return VM_FAULT_OOM;

	ret = remap_pfn_range(vma, addr, page_to_pfn(page), size, __pgprot(prot));
	if (ret)
		return VM_FAULT_SIGSEGV;

	return VM_FAULT_NOPAGE;
}

static const struct vm_operations_struct test_cache_vm_ops = {
	.fault = test_cache_fault,
};

static int test_cache_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long size = ALIGN(vma->vm_end - vma->vm_start, PAGE_SIZE);
	unsigned long max_size = PAGE_SIZE * (1 << MAX_ORDER);

	if (size > max_size) {
		pr_warn("%s: Mapped size 0x%lx exceeds the allowed size 0x%lx\n",
			__func__, size, max_size);
		return -ERANGE;
	}

	if (!IS_ALIGNED(vma->vm_start, PAGE_SIZE) || !IS_ALIGNED(size, size)) {
		pr_warn("%s: vma range (0x%lx, 0x%lx) not aligned\n",
			__func__, vma->vm_start, size);
		return -EINVAL;
	}

	vma->vm_ops = &test_cache_vm_ops;

	return 0;
}

static int test_cache_release(struct inode *inode, struct file *filp)
{
	/* Mark the file has been closed */
	mutex_lock(&test->mutex);
	test->opened = false;
	mutex_unlock(&test->mutex);

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
	int nid = 1;	/* node 1 is preferred */
	int ret = 0;

	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test) {
		pr_warn("%s: Unable to alloc memory\n", __func__);
		return -ENOMEM;
	}

	/*
	 * The preferred node may be offline. In this case, we fall
	 * back to node-0, which is always online.
	 */
	mutex_init(&test->mutex);
	test->opened = false;
	if (nid < MAX_NUMNODES && node_online(nid)) {
		test->nid = nid;
	} else {
		pr_info("%s: Invalid or offline node %d, fall back to nid 0\n",
			__func__, nid);
		test->nid = 0;
	}

	ret = misc_register(&test_cache_dev);
	if (ret) {
		pr_warn("%s: Error %d to register device\n", __func__, ret);
		kfree(test);
	}

	return ret;
}

static void __exit test_cache_exit(void)
{
	misc_deregister(&test_cache_dev);
	kfree(test);
}

module_init(test_cache_init);
module_exit(test_cache_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
