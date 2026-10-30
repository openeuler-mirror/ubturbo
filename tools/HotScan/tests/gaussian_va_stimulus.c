// SPDX-License-Identifier: GPL-2.0
/* Periodic Gaussian VA accesses for validating HotScan on a real VM. */
#define _GNU_SOURCE
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { PAGES = 256, TICK_MS = 20, PHASE_TICKS = 100 };
static volatile sig_atomic_t started;
static volatile sig_atomic_t stopped;

static void handle_signal(int sig)
{
	if (sig == SIGUSR1)
		started = 1;
	else
		stopped = 1;
}

static uint64_t random_state = 0x6a09e667f3bcc909ULL;
static double random_unit(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 7;
	random_state ^= random_state << 17;
	return (random_state >> 11) * (1.0 / 9007199254740992.0);
}

static uint64_t monotonic_ns(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

int main(int argc, char **argv)
{
	int seconds = argc > 1 ? atoi(argv[1]) : 24;
	const char *prefix = argc > 2 ? argv[2] : "/tmp/gaussian_va";
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	volatile unsigned char *pages;
	uint64_t totals[PAGES] = { 0 }, start_ns;
	double probability[PAGES];
	char filename[512];
	FILE *trace, *truth;
	struct sigaction sa = { .sa_handler = handle_signal };

	if (seconds < 4 || seconds > 300) {
		fprintf(stderr, "duration must be 4..300 seconds\n");
		return 2;
	}
	pages = mmap(NULL, PAGES * page_size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (pages == MAP_FAILED || madvise((void *)pages, PAGES * page_size,
					 MADV_NOHUGEPAGE)) {
		perror("mmap/madvise");
		return 1;
	}
	for (int i = 0; i < PAGES; i++)
		pages[i * page_size] = 1;
	if (mlock((void *)pages, PAGES * page_size)) {
		perror("mlock");
		return 1;
	}
	for (int i = 0; i < PAGES; i++) {
		double z = (i - 127.5) / 35.0;
		probability[i] = 0.16 * exp(-0.5 * z * z);
	}
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	printf("pid=%ld base=0x%lx pages=%d page_size=%zu tick_ms=%d "
	       "center=127.5 sigma=35 probability_peak=0.16 seconds=%d\n",
	       (long)getpid(), (unsigned long)pages, PAGES, page_size,
	       TICK_MS, seconds);
	fflush(stdout);
	while (!started && !stopped)
		pause();
	if (stopped)
		return 0;
	snprintf(filename, sizeof(filename), "%s_ticks.csv", prefix);
	trace = fopen(filename, "w");
	if (!trace) {
		perror(filename);
		return 1;
	}
	fprintf(trace, "tick,monotonic_ns,active,touches\n");
	start_ns = monotonic_ns();
	for (int tick = 0; tick < seconds * 1000 / TICK_MS && !stopped; tick++) {
		struct timespec deadline;
		uint64_t target = start_ns + (uint64_t)tick * TICK_MS * 1000000ULL;
		int active = (tick / PHASE_TICKS) % 2 == 0;
		int touches = 0;
		deadline.tv_sec = target / 1000000000ULL;
		deadline.tv_nsec = target % 1000000000ULL;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
		if (active) {
			for (int i = 0; i < PAGES; i++) {
				if (random_unit() < probability[i]) {
					pages[i * page_size]++;
					totals[i]++;
					touches++;
				}
			}
		}
		fprintf(trace, "%d,%llu,%d,%d\n", tick,
			(unsigned long long)monotonic_ns(), active, touches);
	}
	fclose(trace);
	snprintf(filename, sizeof(filename), "%s_truth.csv", prefix);
	truth = fopen(filename, "w");
	if (!truth) {
		perror(filename);
		return 1;
	}
	fprintf(truth, "page,probability,touches\n");
	for (int i = 0; i < PAGES; i++)
		fprintf(truth, "%d,%.12g,%llu\n", i, probability[i],
			(unsigned long long)totals[i]);
	fclose(truth);
	printf("start_monotonic_ns=%llu end_monotonic_ns=%llu\n",
	       (unsigned long long)start_ns,
	       (unsigned long long)monotonic_ns());
	return 0;
}
