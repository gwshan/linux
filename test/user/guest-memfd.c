// SPDX-License-Identifier: GPL-2.0+
/*
 * Test program to test functionalities of guest-memfd
 *
 * Author: Gavin Shan <gshan@redhat.com>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/kvm.h>

struct kvm_vm {
	int		kvm_fd;
	int		fd;
	int		guest_memfd;
	void		*host_addr;
	unsigned long	slot_size;
	unsigned long	guest_phys_addr;
};

static void usage(void)
{
	fprintf(stdout, "\n");
	fprintf(stdout, "./guest_memfd <option>\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "Supported options:\n");
	fprintf(stdout, "\n");
	fprintf(stdout, "help   Show the usage messages\n");
	fprintf(stdout, "none   None of operations will be executed\n");
	fprintf(stdout, "read   Read one page from the beginning of the guest memfd\n");
	fprintf(stdout, "write  Write one page to the beginning of the guest memfd\n");
	fprintf(stdout, "mmap:  Map one page on the beginning of the guest memfd\n");
	fprintf(stdout, "\n");
}

static void vm_destroy(struct kvm_vm *vm)
{
	struct kvm_userspace_memory_region2 region;
	int ret;

	if (vm->guest_phys_addr != -1UL) {
		region.slot = 0;
		region.flags = KVM_MEM_GUEST_MEMFD;
		region.guest_phys_addr = 0;
		region.memory_size = 0;
		region.userspace_addr = (unsigned long)(vm->host_addr);
		region.guest_memfd_offset = 0;
		region.guest_memfd = vm->guest_memfd;
		ret = ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION2, &region);
		if (ret)
			fprintf(stderr, "%s: Error %d to remove memory slot\n", __func__);
	}

	if (vm->guest_memfd >= 0)
		close(vm->guest_memfd);

	if (vm->host_addr != MAP_FAILED)
		munmap(vm->host_addr, vm->slot_size);

	if (vm->fd > 0)
		close(vm->fd);

	if (vm->kvm_fd > 0)
		close(vm->kvm_fd);
}

static struct kvm_vm *vm_create(void)
{
	struct kvm_vm *vm;
	struct kvm_create_guest_memfd guest_memfd;
	struct kvm_userspace_memory_region2 region;
	int ret;

	vm = calloc(1, sizeof(*vm));
	if (!vm) {
		fprintf(stderr, "%s: Unable to allocate memory\n", __func__);
		return NULL;
	}

	vm->kvm_fd = -1;
	vm->fd = -1;
	vm->guest_memfd = -1;
	vm->host_addr = MAP_FAILED;
	vm->slot_size = 0x40000000;	/* 1GB */
	vm->guest_phys_addr = -1UL;

	/* Open /dev/kvm */
	vm->kvm_fd = open("/dev/kvm", O_RDWR);
	if (vm->kvm_fd < 0) {
		fprintf(stderr, "%s: Unable to open </dev/kvm>\n", __func__);
		goto error;
	}

	/* Create VM */
	vm->fd = ioctl(vm->kvm_fd, KVM_CREATE_VM, 36);
	if (vm->fd < 0) {
		fprintf(stderr, "%s: Unable to create VM\n", __func__);
		goto error;
	}

	/* Create the shared space */
	vm->host_addr = mmap(NULL, vm->slot_size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (vm->host_addr == MAP_FAILED) {
		fprintf(stderr, "%s: Unable to mmap anonymous space\n", __func__);
		goto error;
	}

	ret = madvise(vm->host_addr, vm->slot_size, MADV_NOHUGEPAGE);
	if (ret != 0) {
		fprintf(stderr, "%s: Error %d to disable THP\n", __func__, ret);
		goto error;
	}

	/* Create the private space */
	guest_memfd.size = vm->slot_size;
	guest_memfd.flags = 0;
	vm->guest_memfd = ioctl(vm->fd, KVM_CREATE_GUEST_MEMFD, &guest_memfd);
	if (vm->guest_memfd < 0) {
		fprintf(stderr, "%s: Error %d to create guest-memfd\n",
			__func__, vm->guest_memfd);
		goto error;
	}

	ret = fallocate(vm->guest_memfd, FALLOC_FL_KEEP_SIZE, 0, vm->slot_size);
	if (ret) {
		fprintf(stderr, "%s: Error %d to fallocate guest-memfd\n",
			__func__, ret);
		goto error;
	}

	/* Create the memory slot */
	region.slot = 0;
	region.flags = KVM_MEM_GUEST_MEMFD;
	region.guest_phys_addr = 0;
	region.memory_size = vm->slot_size;
	region.userspace_addr = (unsigned long)(vm->host_addr);
	region.guest_memfd_offset = 0;
	region.guest_memfd = vm->guest_memfd;
	ret = ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION2, &region);
	if (ret) {
		fprintf(stderr, "%s: Error %d to add memory slot\n", __func__);
		goto error;
	}

	vm->guest_phys_addr = 0;
	return vm;
error:
	vm_destroy(vm);
	return NULL;
}

static void test_read(struct kvm_vm *vm)
{
	int pagesz = getpagesize();
	void *buf;
	ssize_t ret;

	buf = malloc(pagesz);
	if (!buf) {
		fprintf(stderr, "%s: Unable to allocate buffer\n", __func__);
		return;
	}

	memset(buf, 0, pagesz);

	ret = read(vm->guest_memfd, buf, 0);
	if (ret != pagesz)
		fprintf(stderr, "%s: Error %ld to read\n", __func__, ret);
	else
		fprintf(stdout, "%s: Error %ld to read\n", __func__, ret);

	free(buf);
}

static void test_write(struct kvm_vm *vm)
{
	int pagesz = getpagesize();
	void *buf;
	ssize_t ret;

	buf = malloc(pagesz);
	if (!buf) {
		fprintf(stderr, "%s: Unable to allocate buffer\n", __func__);
		return;
	}

	memset(buf, 0, pagesz);

	ret = write(vm->guest_memfd, buf, pagesz);
	if (ret != pagesz)
		fprintf(stderr, "%s: Error %ld to write\n", __func__, ret);
	else
		fprintf(stdout, "%s: Succeed\n", __func__);

	free(buf);
}

static void test_mmap(struct kvm_vm *vm)
{
	int pagesz = getpagesize();
	void *buf;

	buf = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_SHARED,
		   vm->guest_memfd, 0);
	if (buf == MAP_FAILED) {
		fprintf(stderr, "%s: Failed\n", __func__);
		return;
	}

	fprintf(stdout, "%s: Succeed\n", __func__);
	munmap(buf, pagesz);		
}

int main(int argc, char **argv)
{
	struct kvm_vm *vm;
	int c;

	if (argc != 2) {
		usage();
		return -EINVAL;
	}

	if (!strcmp(argv[1], "help")) {
		usage();
		return 0;
	}

	if (strcmp(argv[1], "none") &&
	    strcmp(argv[1], "read") &&
	    strcmp(argv[1], "write") &&
	    strcmp(argv[1], "mmap")) {
		usage();
		return -EINVAL;
	}

	vm = vm_create();
	if (!vm)
		return -EFAULT;

	if (!strcmp(argv[1], "none"))
		;
	else if (!strcmp(argv[1], "read"))
		test_read(vm);
	else if (!strcmp(argv[1], "write"))
		test_write(vm);
	else if (!strcmp(argv[1], "mmap"))
		test_mmap(vm);

	fprintf(stdout, "Press any key to exit...\n");
	scanf("%c", &c);

	vm_destroy(vm);

	return 0;
}
