// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver to export memory, which will be mapped to user space using the
 * specified caching scheme. With it, the user space is able to measure
 * the benchmarks when different caching scheme is used. At present, the
 * supported caching schmes are like below.
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

// #define TEST_MEM_DEBUG
#define TEST_MEM_NODE	0

/* Memory attribute in the page-table entry */
#define TEST_MEM_MODE_NORMAL		0
#define TEST_MEM_MODE_NO_CACHE		1
#define TEST_MEM_MODE_DEVICE		2
#define TEST_MEM_MODE_DEVICE_NP		3
#define TEST_MEM_MODE_MAX		4

struct test_mem {
	struct mutex	mutex;
	bool		opened;

	unsigned int	mode;
};

static struct test_mem *test;
static const char * const test_mem_modes[] = {
	"normal", "no_cache", "device", "device_np"
};

static int test_mem_open(struct inode *inode, struct file *filp)
{
	/* Multiple writers aren't allowed */
	mutex_lock(&test->mutex);
	if (test->opened) {
		mutex_unlock(&test->mutex);
		return -EPERM;
	}

	test->opened = true;

	mutex_unlock(&test->mutex);
	return 0;
}

static ssize_t test_mem_read(struct file *filp, char __user *buf,
			     size_t size, loff_t *off)
{
	const char *p = test_mem_modes[test->mode];
	size_t count = min(size, strlen(p));

	if (copy_to_user(buf, p, count))
		return -EFAULT;

	*off += count;
	return count;
}

static ssize_t test_mem_write(struct file *filp, const char __user *buf,
			      size_t size, loff_t *off)
{
	char *p = NULL;
	ssize_t ret = size;
	unsigned int index;

	if (size > PAGE_SIZE)
		return -ENOSPC;

	p = kzalloc(size, GFP_KERNEL);
	if (!p) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(p, buf, size)) {
		ret = -EFAULT;
		goto out;
	}

	for (index = 0; index < ARRAY_SIZE(test_mem_modes); index++) {
		if (!strcmp(p, test_mem_modes[index])) {
			test->mode = index;
			break;
		}
	}

out:
	kfree(p);
	if (ret > 0)
		*off += ret;
	return ret;
}

#ifdef TEST_MEM_DEBUG
static void test_mem_dump_pte(struct vm_area_struct *vma,
			      unsigned long addr)
{
	struct mm_struct *mm = vma->vm_mm;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if (!(addr >= vma->vm_start && addr < vma->vm_end)) {
		pr_info("Address 0x%lx out of range [0x%lx  0x%lx]\n",
			addr, vma->vm_start, vma->vm_end);
		return;
	}

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || pgd_bad(*pgd)) {
		pr_info("Invalid PGD 0x%016llx at address 0x%lx\n",
			pgd_val(*pgd), addr);
		return;
	}

	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || p4d_bad(*p4d)) {
		pr_info("Invalid P4D 0x%016llx at address 0x%lx\n",
			p4d_val(*p4d), addr);
		return;
	}

	/* 1GB transparent huge page isn't available to ARM64 yet */
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || pud_bad(*pud)) {
		pr_info("Invalid PUD 0x%016llx at address 0x%lx\n",
			pud_val(*pud), addr);
		return;
	}

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd) || pmd_bad(*pmd)) {
		pr_info("Invalid PMD 0x%016llx at address 0x%lx\n",
			pmd_val(*pmd), addr);
		return;
	}

	/* PMD entry may be corresponding to a huge page */
	if (pmd_trans_huge(*pmd)) {
		pr_info("PMD=0x%016llx at address 0x%lx\n",
			pmd_val(*pmd), addr);
		return;
	}

	pte = pte_offset_kernel(pmd, addr);
	pr_info("PTE=0x%016llx at address 0x%lx\n", pte_val(*pte), addr);
}
#endif /* TEST_MEM_DEBUG */

static vm_fault_t test_mem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct page *page = NULL;
	unsigned long addr = ALIGN_DOWN(vmf->address, PAGE_SIZE);
	pteval_t prot = pgprot_val(vma->vm_page_prot);
	int ret;

	/* Page table entry protocol */
	prot &= ~PTE_ATTRINDX_MASK;
	switch (test->mode) {
	case TEST_MEM_MODE_NORMAL:
		prot |= PTE_ATTRINDX(MT_NORMAL);
		break;
	case TEST_MEM_MODE_NO_CACHE:
		prot |= PTE_ATTRINDX(MT_NORMAL_NC);
		break;
	case TEST_MEM_MODE_DEVICE:
		prot |= PTE_ATTRINDX(MT_DEVICE_nGnRE);
		break;
	case TEST_MEM_MODE_DEVICE_NP:
		prot |= PTE_ATTRINDX(MT_DEVICE_nGnRnE);
		break;
	default:
		prot = pgprot_val(vma->vm_page_prot);
	}

	/* Allocate physical memory */
	page = alloc_pages_node(TEST_MEM_NODE, GFP_KERNEL, 0);
	if (!page)
		return VM_FAULT_OOM;

	/* Map the physical memory */
#ifdef TEST_MEM_DEBUG
	test_mem_dump_pte(vma, addr);
#endif

	ret = remap_pfn_range(vma, addr, page_to_pfn(page),
			      PAGE_SIZE, __pgprot(prot));
	if (ret)
		return VM_FAULT_SIGSEGV;

#ifdef TEST_MEM_DEBUG
	test_mem_dump_pte(vma, addr);
#endif

	return VM_FAULT_NOPAGE;
}

static const struct vm_operations_struct test_mem_vm_ops = {
	.fault = test_mem_fault,
};

static int test_mem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	vma->vm_ops = &test_mem_vm_ops;

	return 0;
}

static int test_mem_release(struct inode *inode, struct file *filp)
{
	/* Mark the file has been closed */
	mutex_lock(&test->mutex);
	test->opened = false;
	mutex_unlock(&test->mutex);

	return 0;
}

static const struct file_operations test_mem_fops = {
	.open      = test_mem_open,
        .read      = test_mem_read,
	.write     = test_mem_write,
	.mmap      = test_mem_mmap,
	.release   = test_mem_release,
};

static struct miscdevice test_mem_dev = {
	.minor     = MISC_DYNAMIC_MINOR,
	.name      = "test_mem",
	.fops      = &test_mem_fops,
};

static int __init test_mem_init(void)
{
	int ret = 0;

	test = kzalloc(sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;

	mutex_init(&test->mutex);
	test->opened = false;
	test->mode = TEST_MEM_MODE_NORMAL;

	ret = misc_register(&test_mem_dev);
	if (ret)
		kfree(test);

	return ret;
}

static void __exit test_mem_exit(void)
{
	misc_deregister(&test_mem_dev);
	kfree(test);
}

module_init(test_mem_init);
module_exit(test_mem_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
