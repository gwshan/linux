// SPDX-License-Identifier: GPL-2.0+
/*
 * Test program to measure memory access latency on /dev/test_mem, by
 * which the memory is exposed with the specified caching scheme enabled.
 * The caching scheme must be set by writing to /dev/test_mem before the
 * memory is mapped.
 *
 * Author: Gavin Shan <gshan@redhat.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>

struct test_mem {
	int		fd;
	unsigned long	size;
	unsigned long	page_size;
	void		*addr;

	unsigned long	iterations;
	unsigned long	loops;
	int		cache_mode;
	int		stop;
};

static struct test_mem test;
static const char * const cache_modes[] = {
	"normal", "no_cache", "device", "device_np",
};

static void usage(char *name)
{
	fprintf(stdout, "\n");
	fprintf(stdout, "Usage: %s [-i iter] [-l loops] [-s size] [-c type] [-b] [-h]\n", name);
	fprintf(stdout, "\n");
	fprintf(stdout, "-i: Iterations to access the memory in each loop\n");
	fprintf(stdout, "-l: Loops of the tests to be carried out\n");
	fprintf(stdout, "-s: Size of memory to be mapped\n");
	fprintf(stdout, "-c: Cache type applied to the pagetable entry\n");
	fprintf(stdout, "    Available types: normal, no_cache, device, device_np\n");
	fprintf(stdout, "-b: Stop prior to exit\n");
	fprintf(stdout, "-h: Show help messages\n");
	fprintf(stdout, "\n");
}

static int select_mode(const char *mode)
{
	int i, count = sizeof(cache_modes) / sizeof(cache_modes[0]);

	for (i = 0; i < count; i++) {
		if (!strcmp(cache_modes[i], mode)) {
			return i;
		}
	}

	return -EINVAL;
}

static void access_mem(unsigned long iterations)
{
	void *start, *end = test.addr + test.size;
	unsigned long i;

	for (i = 0; i < iterations; i++) {
		for (start = test.addr; start < end; start += test.page_size)
			memset(start, 0, 1);
	}
}

int main(int argc, char **argv)
{
	struct timespec tstart, tend;
	unsigned long loop, elapsed;
	int index, opt, ret;

	test.fd = -1;
	test.page_size = getpagesize();
	test.size = test.page_size * (test.page_size / 8);
	test.addr = (void *)-1;
	test.iterations = 100;
	test.loops = 1;
	test.cache_mode = 0;
	test.stop = 0;

	while ((opt = getopt(argc, argv, "i:l:s:c:bh")) != -1) {
		switch (opt) {
		case 'i':
			test.iterations = strtol(optarg, NULL, 0);
			break;
		case 'l':
			test.loops = strtol(optarg, NULL, 0);
			break;
		case 's':
			test.size = strtol(optarg, NULL, 0);
			test.size = (test.size + test.page_size - 1) &
				    ~(test.page_size - 1);
			break;
		case 'c':
			index = select_mode(optarg);
			if (index < 0) {
				usage(argv[0]); return index;
			}

			test.cache_mode = index;
			break;
		case 'b':
			test.stop = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return 0;
		}
	}

	test.fd = open("/dev/test_mem", O_RDWR);
	if (test.fd < 0) {
		fprintf(stderr, "Unable to open </dev/test_mem>\n");
		return -ENOENT;
	}

	/* Set up the mode */
	ret = write(test.fd, (void *)(cache_modes[test.cache_mode]),
		    strlen(cache_modes[test.cache_mode]));
	if (ret != strlen(cache_modes[test.cache_mode])) {
		fprintf(stdout, "Unable to configure mode (%d)\n", ret);
		goto cleanup;
	}

	test.addr = mmap(NULL, test.size, PROT_READ | PROT_WRITE, MAP_SHARED,
			 test.fd, 0);
	if (test.addr == (void *)-1) {
		fprintf(stderr, "Unable to mmap </dev/test_mem>\n");
		return -EFAULT;
	}

	/* Warm it up */
	access_mem(1);

	/* Test */
	for (loop = 0; loop < test.loops; loop++) {
		clock_gettime(CLOCK_MONOTONIC, &tstart);

		access_mem(test.iterations);

		clock_gettime(CLOCK_MONOTONIC, &tend);
		elapsed = (tend.tv_nsec + 1000000000UL * tend.tv_sec) -
			  (tstart.tv_nsec + 1000000000UL * tstart.tv_sec);
		fprintf(stdout, "Loop %02d: %ldns\n", loop, elapsed);
	}

	if (test.stop) {
		fprintf(stdout, "Press any key to exit...\n");
		scanf("%c", &opt);
	}

cleanup:
	if (test.addr != (void *)-1)
		munmap(test.addr, test.size);
	if (test.fd > 0)
		close(test.fd);
	return 0;
}
