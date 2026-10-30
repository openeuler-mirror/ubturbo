// SPDX-License-Identifier: GPL-2.0
/* A fixed anonymous VA range with two signal-selected hot regions. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { PAGES = 256 };
static volatile sig_atomic_t phase;
static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	if (sig == SIGUSR1)
		phase = 1;
	else if (sig == SIGUSR2)
		phase = 2;
	else if (sig == SIGHUP)
		phase = 3;
	else
		stop = 1;
}

int main(void)
{
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	size_t length = PAGES * page_size;
	volatile unsigned char *pages;
	struct timespec pause = { .tv_sec = 0, .tv_nsec = 2000000 };

	pages = mmap(NULL, length, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pages == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	if (madvise((void *)pages, length, MADV_NOHUGEPAGE)) {
		perror("madvise");
		return 1;
	}
	for (int i = 0; i < PAGES; ++i)
		pages[i * page_size] = 1;
	if (mlock((void *)pages, length)) {
		perror("mlock");
		return 1;
	}
	struct sigaction sa = { .sa_handler = on_signal };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	printf("pid=%ld base=0x%lx pages=%d page_size=%zu\n",
	       (long)getpid(), (unsigned long)pages, PAGES, page_size);
	fflush(stdout);

	while (!stop) {
		if (phase != 2) {
			int first = phase ? 160 : 16;
			int second = phase ? 224 : 96;
			for (int i = 0; i < 32; ++i) {
				pages[(first + i) * page_size]++;
				pages[(second + i) * page_size]++;
			}
		}
		if (phase == 3) {
			unsigned char *churn = mmap(NULL, 32 * page_size,
				PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
				-1, 0);
			if (churn != MAP_FAILED) {
				for (int i = 0; i < 32; ++i)
					churn[i * page_size] = 1;
				munmap(churn, 32 * page_size);
			}
		}
		nanosleep(&pause, NULL);
	}
	munlock((void *)pages, length);
	munmap((void *)pages, length);
	return 0;
}
