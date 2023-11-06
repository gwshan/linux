// SPDX-License-Identifier: GPL-2.0-only

/* mtest: Measure the benchmark of accessing memory, with various backends
 *
 * The test measures the needed time of accessing memory, which are backed
 * by various backends. For example, the memory can be backed up by the
 * regular anonymous virtual memory or hugeTLBfs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <linux/arm-smccc.h>
#include <asm/kvm.h>
#include <kvm_util.h>

#include "processor.h"

struct test_data {
	uint64_t	phys_addr;	/* Physical address of the extra memory  */
	uint64_t	virt_addr;	/* Virtual address of the extra memory   */
	uint64_t	len;		/* Length of the extra memory            */
	int		type;		/* Type of the extra memory's backend    */
	int		iterations;	/* Iterations to access the extra memory */
	int		loops;		/* Loops of the tests                    */
};

static struct test_data data;	/* Tested data, shared by host/guest */
static struct ucall my_ucall;	/* ucall data, owned by guest */

static void guest_code(void)
{
	uint64_t cur, addr, len;
	int i, iterations;

	addr = READ_ONCE(data.virt_addr);
	len = READ_ONCE(data.len);

	while (true) {
		iterations = READ_ONCE(data.iterations);

		/* Assume the smallest page size is 4KB */
		for (i = 0; i < iterations; i++) {
			for (cur = addr; cur < (addr + len); cur += 0x1000) {
				*(int *)cur = 0;
			}
		}

		/*
		 * The regular GUEST_SYNC() and ucall() can be called here
		 * because implementaion defined data abort is raised on
		 * instruction ldrx/strx, originating from test_and_set_bit(),
		 * called by ucall_alloc() and ucall(). So we have to force
		 * KVM exit due to MMIO access here and the written data is
		 * used to identify the exact scenario.
		 */
		WRITE_ONCE(my_ucall.args[0], 0UL);
		WRITE_ONCE(my_ucall.args[1], 0UL);
		WRITE_ONCE(my_ucall.args[2], 0UL);
		WRITE_ONCE(my_ucall.args[3], 0UL);
		WRITE_ONCE(my_ucall.args[4], 0UL);
		WRITE_ONCE(my_ucall.args[5], 0UL);
		WRITE_ONCE(my_ucall.args[6], 0UL);
		WRITE_ONCE(my_ucall.cmd, UCALL_SYNC);
		ucall_arch_do_ucall((vm_vaddr_t)&my_ucall);
	}
}

static void help(char *name)
{
	puts("");
	printf("Usage: %s [-h] [-i iterations] [-l loops] [-s type] [-c type] \n", name);
	puts("");
	printf(" -i: Iterations to access the memory in each test\n");
	printf(" -l: Loops of the tests to be done\n");
	backing_src_help("-s");
	printf(" -c: Cache type to be applied for data access.\n");
	printf("     Available types: nocache, writeback, writethrough, device\n");
	printf(" -b: Break prior to exit\n");
	puts("");
	exit(0);
}

static bool reserve_memory(bool release)
{
	FILE *f;
	uint64_t page_size, pages;
	char name[128], content[64];

	switch (data.type) {
	case VM_MEM_SRC_ANONYMOUS_HUGETLB ... VM_MEM_SRC_ANONYMOUS_HUGETLB_16GB:
	case VM_MEM_SRC_SHARED_HUGETLB:
		break;
	default:
		return true;
	}

	page_size = get_backing_src_pagesz(data.type);
	pages = release ? 0UL: align_up(data.len, page_size) / page_size;

	/*
	 * We have the assumption that node-1 has more available memory
	 * than node-0. So we want to reserve the huge pages from node-1
	 * so that they can be reserved successfully.
	 */
	memset(name, 0, sizeof(name));
	memset(content, 0, sizeof(content));
	snprintf(name, sizeof(name),
		 "/sys/devices/system/node/node1/hugepages/hugepages-%ldkB/nr_hugepages",
		 page_size >> 10);
	snprintf(content, sizeof(content), "%ld", pages);

	f = fopen(name, "w");
	if (!f) {
		pr_info("%s: unable to open hugetlbfs file\n", __func__);
		return false;
	}

	if (fputs(content, f) < 0) {
		pr_info("%s: unable to write hugetlbfs file\n", __func__);
		fclose(f);
		return false;
	}

	fclose(f);
	return true;
}

int main(int argc, char **argv)
{
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct ucall uc;
	uint64_t cache_type = MT_NORMAL_NC;
	bool enabled_break = false;
	struct timespec start, elapsed;
	int iterations = 100, opt;
	uint64_t phys, virt, page_size, val, i;
	char c;

	/* Initialize the global data */
	data.virt_addr = 0xc0000000;
	data.len = 0x40000000;	/* 1GB */
	data.type = VM_MEM_SRC_ANONYMOUS;
	data.iterations = iterations;
	data.loops = 2;

	while ((opt = getopt(argc, argv, "i:l:s:c:bh")) != -1) {
		switch (opt) {
		case 'i':
			iterations = atoi(optarg);
			TEST_ASSERT(iterations > 0,
				    "Iterations must be greater than zero");
			break;
		case 'l':
			data.loops = atoi(optarg);
			TEST_ASSERT(data.loops > 1,
				    "Loops must be greater than one");
			break;
		case 's':
			data.type = parse_backing_src_type(optarg);
			break;
		case 'c':
			if (!strcmp(optarg, "nocache"))
				cache_type = MT_NORMAL_NC;
			else if (!strcmp(optarg, "writeback"))
				cache_type = MT_NORMAL;
			else if (!strcmp(optarg, "writethrough"))
				cache_type = MT_NORMAL_WT;
			else if (!strcmp(optarg, "device"))
				cache_type = MT_DEVICE_nGnRnE;
			else
				TEST_ASSERT(false, "Invalid cache type <%s>\n", optarg);
			break;
		case 'b':
			enabled_break = true;
			break;
		case 'h':
		default:
			help(argv[0]);
		}
	}

	/* Create VM */
	vm = __vm_create_with_one_vcpu(&vcpu, 0x100000, guest_code);
#if 0
	vcpu_get_reg(vcpu, KVM_ARM64_SYS_REG(SYS_SCTLR_EL1), &val);
	if (cache_type == MT_NORMAL_NC || cache_type == MT_DEVICE_nGnRnE)
		val &= ~(1UL << 2);
	else
		val |= (1UL << 2);
	vcpu_set_reg(vcpu, KVM_ARM64_SYS_REG(SYS_SCTLR_EL1), val);
#endif

	/*
	 * Figure out the physcial address of the extra memory, which
	 * seats on the end of the possible physical address space.
	 */
	data.phys_addr = (vm->max_gfn - (data.len >> vm->page_shift)) <<
			 vm->page_shift;
	page_size = get_backing_src_pagesz(data.type);
	data.phys_addr = align_down(data.phys_addr, page_size);

	/* Reserve the extra memory */
	if (!reserve_memory(false)) {
		pr_info("Unable to reserve memory\n");
		return 0;
	}

	/* Add the extra memory */
	vm_userspace_mem_region_add(vm, data.type, data.phys_addr, 1,
				    data.len >> vm->page_shift, 0);

	/*
	 * Map the extra memory. The cache could be disabled
	 * according to the specified arguments by user
	 */
	phys = data.phys_addr;
	virt = data.virt_addr;
	for (i = 0; i < (data.len >> vm->page_shift); i++) {
		__virt_pg_map(vm, virt, phys, cache_type);
		sparsebit_set(vm->vpages_mapped, virt >> vm->page_shift);
		phys += vm->page_size;
		virt += vm->page_size;
	}

	/*
	 * Start the test. In the first loop, the memory needs to be
	 * faulted in and settled completely. So it's not sensible to
	 * measure the benchmark in the first loop. No extra iterations
	 * of accessing the memory is executed to save time.
	 */
	for (i = 0; i < data.loops; i++) {
		data.iterations = (i == 0) ? 1 : iterations;

		sync_global_to_guest(vm, data);
		clock_gettime(CLOCK_MONOTONIC, &start);
		vcpu_run(vcpu);
		elapsed = timespec_elapsed(start);

		sync_global_from_guest(vm, my_ucall);
		switch (get_ucall(vcpu, &uc)) {
		case UCALL_SYNC:
			pr_info("Iteration %ld: \t%ldms\n",
				i, elapsed.tv_sec * 1000 + elapsed.tv_nsec / 1000000);
			break;
		default:
			TEST_FAIL("Unexpected guest exit code\n");
		}
	}

	/* Hold to check */
	if (enabled_break) {
		pr_info("Press any key to exit...\n");
		scanf("%c", &c);
	}

	/* Release the reserved memory */
	reserve_memory(true);

	return 0;
}
