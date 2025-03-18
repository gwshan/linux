// SPDX-License-Identifier: GPL-2.0+
/*
 * Test program to measure memory access latency due to L1/L2 cache line
 * eviction. 
 *
 * Author: Gavin Shan <gshan@redhat.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>

#define TEST_DEFAULT_LOOPS	1UL
#define TEST_DEFAULT_ITERATIONS	100000UL
#define TEST_MEM_SIZE		0x20000000UL
#define TEST_MEM_MASK		((TEST_MEM_SIZE / 2) - 1)

struct test_cache {
	unsigned long	page_size;
	unsigned long	size;
	int		fd;
	void		*addr;

	unsigned long	iterations;
	unsigned long	loops;
	int		test_case;
};

struct test_cache_case {
	const char	*name;
	unsigned long	cache_line_size;
	unsigned long	num_of_sets;
	unsigned long	steps;
};

static struct test_cache test;
static struct test_cache_case test_cases[] = {
	{ .name = "L1 cache base",
	  .cache_line_size = 64,
	  .num_of_sets = 0x100,
	  .steps = 4,
	},
	{ .name = "L1 cache miss",
	  .cache_line_size = 64,
	  .num_of_sets = 0x100,
	  .steps = 8,
	},
	{ .name = "L2 cache base",
	  .cache_line_size = 64,
	  .num_of_sets = 0x800,
	  .steps = 8,
	},
	{ .name = "L2 cache miss",
	  .cache_line_size = 64,
	  .num_of_sets = 0x800,
	  .steps = 16,
	},
	{ .name = "L3 cache base",
	  .cache_line_size = 64,
	  .num_of_sets = 0x800,
	  .steps = 16,
	},
	{ .name = "L3 cache miss",
	  .cache_line_size = 64,
	  .num_of_sets = 0x8000,
	  .steps = 32,
	},

};

static void usage(char *name)
{
	fprintf(stdout, "\n");
	fprintf(stdout, "Usage: %s [-i iter] [-l loops] [-t case] [-h]\n", name);
	fprintf(stdout, "\n");
	fprintf(stdout, "-i: Iterations to access the memory in each loop\n");
	fprintf(stdout, "-l: Loops of the tests to be carried out\n");
	fprintf(stdout, "-t: Specified test case to run\n");
	fprintf(stdout, "-h: Show help messages\n");
	fprintf(stdout, "\n");
}

static void init(void)
{
	test.page_size = getpagesize();
	test.size       = TEST_MEM_SIZE;
	test.fd         = -1;
	test.addr       = (void *)-1;
	test.iterations = TEST_DEFAULT_ITERATIONS;
	test.loops      = TEST_DEFAULT_LOOPS;
	test.test_case  = -1;	/* all test cases */
}

static void do_test(struct test_cache_case *tcase)
{
	struct timespec tstart, tend;
	unsigned long addr, elapsed, iteration, loop, step;
	unsigned long sets = tcase->num_of_sets;
	unsigned long cl_size = tcase->cache_line_size;
	int *pval;

	fprintf(stdout, "---> %s\n", tcase->name);
	addr = ((unsigned long)test.addr + TEST_MEM_MASK) & ~TEST_MEM_MASK;
	pval = (int *)addr;

	for (loop = 0; loop < test.loops; loop++) {
		clock_gettime(CLOCK_MONOTONIC, &tstart);

		for (iteration = 0; iteration < test.iterations; iteration++) {
			for (step = 0; step < tcase->steps; step++) {
				step += *(pval + (step * sets * cl_size) / 4);
				step -= *(pval + (step * sets * cl_size) / 4);
			}
		}

		clock_gettime(CLOCK_MONOTONIC, &tend);
		elapsed = (tend.tv_nsec + 1000000000UL * tend.tv_sec) -
			   (tstart.tv_nsec + 1000000000UL * tstart.tv_sec);
		fprintf(stdout, "    Loop %02d: %ldns\n", loop, elapsed);
	}
}

int main(int argc, char **argv)
{
	int opt, i, ret = 0;

	init();

	while ((opt = getopt(argc, argv, "i:l:t:h")) != -1) {
		switch (opt) {
		case 'i':
			test.iterations = strtol(optarg, NULL, 0);
			break;
		case 'l':
			test.loops = strtol(optarg, NULL, 0);
			break;
		case 't':
			test.test_case = atoi(optarg);
			if (test.test_case < 0 ||
			    test.test_case >= sizeof(test_cases) / sizeof(test_cases[0])) {
				fprintf(stdout, "Invalid test case %d\n", test.test_case);
				return -EINVAL;
			}

			break;
		case 'h':
		default:
			usage(argv[0]);
			return 0;
		}
	}

	/* Open the device file */
	test.fd = open("/dev/test_cache", O_RDWR);
	if (test.fd < 0) {
		fprintf(stderr, "Unable to open </dev/test_cache>\n");
		ret = -EIO;
		goto out;
	}

	/* Allocate memory */
	test.addr = mmap(NULL, test.size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, test.fd, 0);
	if (test.addr == (void *)-1) {
		fprintf(stdout, "Unable to allocate memory\n");
		ret = -ENOMEM;
		goto out;
	}

	/* Populate the memory */
	memset(test.addr, 0, test.size);

	/* Tests */
	for (i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
		if (test.test_case >= 0 && test.test_case != i)
			continue;

		do_test(&test_cases[i]);
	}

out:
	if (test.addr != (void *)-1)
		munmap(test.addr, test.size);
	if (test.fd > 0)
		close(test.fd);

	return ret;
}

