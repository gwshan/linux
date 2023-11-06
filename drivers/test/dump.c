// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver to create /proc/dump, which receives various items to be dumped
 * by writing. The specified item will be dumped on reading /proc/dump.
 */

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/bitfield.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define DRIVER_VERSION	"0.1"
#define DRIVER_AUTHOR	"Gavin Shan, Redhat Inc"
#define DRIVER_DESC	"Dump items through procfs"

#define TEST_DUMP_OPT_HELP		0
#define TEST_DUMP_OPT_CACHE		1
#define TEST_DUMP_OPT_PROCESS		2
#define TEST_DUMP_OPT_MM		3
#define TEST_DUMP_OPT_MM_MT		4
#define TEST_DUMP_OPT_MAX		5

static struct proc_dir_entry *pde;
static int dump_option = TEST_DUMP_OPT_CACHE;
static const char * const dump_options[] = {
	"help",
	"cache",
	"process",
	"mm",
};

static void dump_show_help(struct seq_file *m)
{
	int i;

	seq_puts(m, "\n");
	seq_puts(m, "Available options:\n");
	seq_puts(m, "\n");
	for (i = 0; i < ARRAY_SIZE(dump_options); i++)
		seq_printf(m, "%s\n", dump_options[i]);

	seq_puts(m, "\n");
}

static void dump_show_cache(struct seq_file *m)
{
	unsigned long val, val1;
	unsigned int type, i;
	bool has_ccidx;

	seq_puts(m, "\n");

	/* CTR_EL0: Cache Type Register */
	val = read_sysreg(ctr_el0);
	seq_printf(m, "CTR_EL0:         0x%016lx\n", val);
	seq_puts  (m, "-----------------------------------\n");
	seq_printf(m, "37:32 TminLine   0x%lx\n",    FIELD_GET(GENMASK(37, 32), val));
	seq_printf(m, "   29 DIC        0x%lx\n",    FIELD_GET(GENMASK(29, 29), val));
	seq_printf(m, "   28 IDC        0x%lx\n",    FIELD_GET(GENMASK(28, 28), val));
	seq_printf(m, "27:24 CWG        0x%lx\n",    FIELD_GET(GENMASK(27, 24), val));
	seq_printf(m, "23:20 ERG        0x%lx\n",    FIELD_GET(GENMASK(23, 20), val));
	seq_printf(m, "19:16 DminLine   0x%lx\n",    FIELD_GET(GENMASK(19, 16), val));
	seq_printf(m, "15:14 L1Ip       0x%lx\n",    FIELD_GET(GENMASK(15, 14), val));
	seq_printf(m, "03:00 IminLine   0x%lx\n",    FIELD_GET(GENMASK(3, 0), val));
	seq_puts  (m, "\n");

	/* CLIDR_EL1: Cache Level ID Register */
	val = read_sysreg(clidr_el1);
	seq_printf(m, "CLIDR_EL1:       0x%016lx\n", val);
	seq_puts  (m, "-----------------------------------\n");
	seq_printf(m, "46:45 Ttype7     0x%lx\n",    FIELD_GET(GENMASK(46, 45), val));
	seq_printf(m, "44:43 Ttype6     0x%lx\n",    FIELD_GET(GENMASK(44, 43), val));
	seq_printf(m, "42:41 Ttype5     0x%lx\n",    FIELD_GET(GENMASK(42, 41), val));
	seq_printf(m, "40:39 Ttype4     0x%lx\n",    FIELD_GET(GENMASK(40, 39), val));
	seq_printf(m, "38:37 Ttype3     0x%lx\n",    FIELD_GET(GENMASK(38, 37), val));
	seq_printf(m, "36:35 Ttype2     0x%lx\n",    FIELD_GET(GENMASK(36, 35), val));
	seq_printf(m, "34:33 Ttype1     0x%lx\n",    FIELD_GET(GENMASK(34, 33), val));
	seq_printf(m, "32:30 ICB        0x%lx\n",    FIELD_GET(GENMASK(32, 30), val));
	seq_printf(m, "29:27 LoUU       0x%lx\n",    FIELD_GET(GENMASK(29, 27), val));
	seq_printf(m, "26:24 LoC        0x%lx\n",    FIELD_GET(GENMASK(26, 24), val));
	seq_printf(m, "23:21 LoUIS      0x%lx\n",    FIELD_GET(GENMASK(23, 21), val));
	seq_printf(m, "20:18 Ctype7     0x%lx\n",    FIELD_GET(GENMASK(20, 18), val));
	seq_printf(m, "17:15 Ctype6     0x%lx\n",    FIELD_GET(GENMASK(17, 15), val));
	seq_printf(m, "14:12 Ctype5     0x%lx\n",    FIELD_GET(GENMASK(14, 12), val));
	seq_printf(m, "11:09 Ctype4     0x%lx\n",    FIELD_GET(GENMASK(11, 9), val));
	seq_printf(m, "08:06 Ctype3     0x%lx\n",    FIELD_GET(GENMASK(8, 6), val));
	seq_printf(m, "05:03 Ctype2     0x%lx\n",    FIELD_GET(GENMASK(5, 3), val));
	seq_printf(m, "02:00 Ctype1     0x%lx\n",    FIELD_GET(GENMASK(2, 0), val));
	seq_puts  (m, "\n");

	/* FEAT_CCIDX determines CCSIDR_EL1's format */
	val1 = read_sysreg_s(SYS_ID_AA64MMFR2_EL1);
	has_ccidx = FIELD_GET(GENMASK(23, 20), val1) == 0x1 ? true : false;

	for (i = 1; i < 7; i++) {
		type = (unsigned int)((val >> ((i - 1) * 3)) & 0x7);
		if (!(type >= 0x1 && type <= 0x4))
			continue;

		/* CSSELR_EL1: Cache Size Selection Register */
		val1 = FIELD_PREP(GENMASK(3, 1), i - 1);
		write_sysreg(val1, csselr_el1);

		/* CCSIDR_EL1: Current Cache Size ID Register */
		val1 = read_sysreg(ccsidr_el1);
		seq_printf(m, "CCSIDR_EL1_%d:    0x%016lx\n", i, val1);
		seq_puts  (m, "-----------------------------------\n");
		if (!has_ccidx) {
			seq_printf(m, "NumSets:          0x%lx\n",
				   FIELD_GET(GENMASK(27, 13), val1) + 1);
			seq_printf(m, "Associate:        0x%lx\n",
				   FIELD_GET(GENMASK(12, 3), val1) + 1);
			seq_printf(m, "LineSize:         0x%lx bytes\n",
				   1UL << (FIELD_GET(GENMASK(2, 0), val1) + 4));
		} else {
			seq_printf(m, "NumSets:          0x%lx\n",
				   FIELD_GET(GENMASK(55, 32), val1) + 1);
			seq_printf(m, "Associate:        0x%lx\n",
				   FIELD_GET(GENMASK(23, 3), val1) + 1);
			seq_printf(m, "LineSize:         0x%lx bytes\n",
				   1UL << (FIELD_GET(GENMASK(2, 0), val1) + 4));
		}

		seq_puts(m, "\n");
	}

	seq_puts(m, "\n");
}

static void dump_show_process(struct seq_file *m)
{
	struct task_struct *p;

	seq_puts(m, "\n");
	seq_puts(m, "Available processes\n");
	seq_puts(m, "\n");

	for_each_process(p)
		seq_printf(m, "pid: %d  comm: %s\n", p->pid, p->comm);

	seq_puts(m, "\n");
}

static void dump_show_mm(struct seq_file *m)
{
	struct task_struct *p, *task = NULL;
	struct mm_struct *mm;

	for_each_process(p) {
		if (!strcmp(p->comm, "test")) {
			task = p;
			break;
		}
	}

	if (!task) {
		seq_puts(m, "\n");
		seq_puts(m, "No available process\n");
		seq_puts(m, "\n");
		return;
	}

	/* Process */
	seq_puts(m, "\n");
	seq_printf(m, "pid: %d  comm: %s\n", task->pid, task->comm);
	seq_puts(m, "\n");

	/* mm_struct */
	mm = task->mm;
	seq_puts(m, "-------------------- mm_struct --------------------\n");
	seq_printf(m, "mm_count:                %d\n",    atomic_read(&mm->mm_count));
	seq_puts(  m, "mm_mt:                   -\n");
#ifdef CONFIG_MMU
	seq_printf(m, "get_unmapped_area:       0x%p\n",  mm->get_unmapped_area);
#endif
	seq_printf(m, "mmap_base:               0x%lx\n", mm->mmap_base);
	seq_printf(m, "mmap_legacy_base:        0x%lx\n", mm->mmap_legacy_base);
#ifdef CONFIG_HAVE_ARCH_COMPAT_MMAP_BASES
	seq_printf(m, "mmap_compat_base:        0x%lx\n", mm->mmap_compat_base);
	seq_printf(m, "mmap_compat_legacy_base: 0x%lx\n", mm->mmap_compat_legacy_base);
#endif
	seq_printf(m, "task_size:               0x%lx\n", mm->task_size);
	seq_printf(m, "pgd:                     0x%p\n",  mm->pgd);
#ifdef CONFIG_MEMBARRIER
	seq_printf(m, "membarrier_state:        %d\n",    atomic_read(&mm->membarrier_state));
#endif
	seq_printf(m, "mm_users:                %d\n",    atomic_read(&mm->mm_users));
#ifdef CONFIG_SCHED_MM_CID
	seq_printf(m, "pcpu_cid:                0x%p\n",  mm->pcpu_cid);
	seq_printf(m, "mm_cid_next_scan:        0x%lx\n", mm->mm_cid_next_scan);
#endif
#ifdef CONFIG_MMU
	seq_printf(m, "pgtables_bytes:          0x%lx\n",
		   atomic_long_read(&mm->pgtables_bytes));
#endif
	seq_printf(m, "map_count:               %d\n",    mm->map_count);
	seq_printf(m, "hiwater_rss:             0x%lx\n", mm->hiwater_rss);
	seq_printf(m, "hiwater_vm:              0x%lx\n", mm->hiwater_vm);
	seq_printf(m, "total_vm:                0x%lx\n", mm->total_vm);
	seq_printf(m, "locked_vm:               0x%lx\n", mm->locked_vm);
	seq_printf(m, "pinned_vm:               0x%llx\n", atomic64_read(&mm->pinned_vm));
	seq_printf(m, "data_vm:                 0x%lx\n", mm->data_vm);
	seq_printf(m, "exec_vm:                 0x%lx\n", mm->exec_vm);
	seq_printf(m, "stack_vm:                0x%lx\n", mm->stack_vm);
	seq_printf(m, "def_flags:               0x%lx\n", mm->def_flags);
	seq_printf(m, "start_code:              0x%lx\n", mm->start_code);
	seq_printf(m, "end_code:                0x%lx\n", mm->end_code);
	seq_printf(m, "start_data:              0x%lx\n", mm->start_data);
	seq_printf(m, "end_data:                0x%lx\n", mm->end_data);
	seq_printf(m, "start_brk:               0x%lx\n", mm->start_brk);
	seq_printf(m, "brk:                     0x%lx\n", mm->brk);
	seq_printf(m, "start_stack:             0x%lx\n", mm->start_stack);
	seq_printf(m, "arg_start:               0x%lx\n", mm->arg_start);
	seq_printf(m, "arg_end:                 0x%lx\n", mm->arg_end);
	seq_printf(m, "env_start:               0x%lx\n", mm->env_start);
	seq_printf(m, "env_end:                 0x%lx\n", mm->env_end);
	seq_puts(  m, "saved_auxv:              -\n");
	seq_printf(m, "rss_stat[FILE]:          0x%lx\n", get_mm_counter(mm, MM_FILEPAGES));
	seq_printf(m, "rss_stat[ANON]:          0x%lx\n", get_mm_counter(mm, MM_ANONPAGES));
	seq_printf(m, "rss_stat[SWAP]:          0x%lx\n", get_mm_counter(mm, MM_SWAPENTS));
	seq_printf(m, "rss_stat[SHMEM]:         0x%lx\n", get_mm_counter(mm, MM_SHMEMPAGES));
	seq_printf(m, "binfmt:                  0x%p\n",  mm->binfmt);
	seq_puts(  m, "context:                 -\n");
	seq_printf(m, "flags:                   0x%lx\n", mm->flags);
#ifdef CONFIG_MEMCG
	seq_printf(m, "owner:                   0x%p\n",  mm->owner);
#endif
	seq_printf(m, "user_ns:                 0x%p\n",  mm->user_ns);
	seq_printf(m, "exe_file:                0x%p\n",  mm->exe_file);
#ifdef CONFIG_MMU_NOTIFIER
	seq_printf(m, "notifier_subscriptions:  0x%p\n",  mm->notifier_subscriptions);
#endif
#ifdef CONFIG_NUMA_BALANCING
	seq_printf(m, "numa_next_scan:          0x%lx\n", mm->numa_next_scan);
	seq_printf(m, "numa_scan_offset:        0x%lx\n", mm->numa_scan_offset);
	seq_printf(m, "numa_scan_seq:           0x%x\n",  mm->numa_scan_seq);
#endif
	seq_printf(m, "tlb_flush_pending:       %d\n",    atomic_read(&mm->tlb_flush_pending));
#ifdef CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH
	seq_printf(m, "tlb_flush_batched:       %d\n",    atomic_read(&mm->tlb_flush_batched));
#endif
#ifdef CONFIG_HUGETLB_PAGE
	seq_printf(m, "hugetlb_usage:           0x%lx\n",
		   atomic_long_read(&mm->hugetlb_usage));
#endif
	seq_puts(  m, "async_put_work:          -\n");
#ifdef CONFIG_IOMMU_SVA
	seq_printf(m, "pasid:                   0x%x\n",  mm->pasid);
#endif
#ifdef CONFIG_KSM
	seq_printf(m, "kvm_merging_pages:       0x%lx\n", mm->ksm_merging_pages);
	seq_printf(m, "ksm_rmap_items:          0x%lx\n", mm->ksm_rmap_items);
	seq_printf(m, "ksm_zero_pages:          0x%lx\n", mm->ksm_zero_pages);
#endif
	seq_puts(  m, "\n");
}

static void dump_show_mm_mt(struct seq_file *m)
{
	struct task_struct *p, *task = NULL;
	struct mm_struct *mm;

	for_each_process(p) {
		if (!strcmp(p->comm, "test")) {
			task = p;
			break;
		}
	}

	if (!task) {
		seq_puts(m, "\n");
		seq_puts(m, "No available process\n");
		seq_puts(m, "\n");
		return;
	}

	/* Process */
	seq_puts(m, "\n");
	seq_printf(m, "pid: %d  comm: %s\n", task->pid, task->comm);
	seq_puts(m, "\n");

	/* mm_struct */
	mm = task->mm;
	seq_puts(m, "-------------------- mm_struct::mm_mt --------------------\n");
	seq_puts(m, "\n");
}

static int dump_show(struct seq_file *m, void *v)
{
	switch (dump_option) {
	case TEST_DUMP_OPT_HELP:
		dump_show_help(m);
		break;
	case TEST_DUMP_OPT_CACHE:
		dump_show_cache(m);
		break;
	case TEST_DUMP_OPT_PROCESS:
		dump_show_process(m);
		break;
	case TEST_DUMP_OPT_MM:
		dump_show_mm(m);
		break;
	case TEST_DUMP_OPT_MM_MT:
		dump_show_mm_mt(m);
		break;
	default:
		seq_puts(m, "\n");
		seq_printf(m, "Unsupported option %d\n", dump_option);
		seq_puts(m, "\n");
	}

	return 0;
}

static int dump_open(struct inode *inode, struct file *file)
{
	return single_open(file, dump_show, pde_data(inode));
}

static ssize_t dump_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *pos)
{
	int i;
	char option[64];

	memset(option, 0, sizeof(option));
	if (count < 1 ||
	    count > sizeof(option) - 1 ||
	    copy_from_user(option, buf, count))
		return -EFAULT;

	option[count - 1] = '\0';
	for (i = 0; i < ARRAY_SIZE(dump_options); i++) {
		if (!strcmp(option, dump_options[i])) {
			dump_option = i;
			break;
		}
	}

	if (i >= ARRAY_SIZE(dump_options))
		return -EINVAL;

	return count;
}

static const struct proc_ops dump_fops = {
	.proc_open    = dump_open,
	.proc_read    = seq_read,
	.proc_write   = dump_write,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

static int __init test_dump_init(void)
{
	pde = proc_create("dump", 0444, NULL, &dump_fops);
	if (!pde)
		return -ENOMEM;

	return 0;
}

static void __exit test_dump_exit(void)
{
	proc_remove(pde);
}

module_init(test_dump_init);
module_exit(test_dump_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
