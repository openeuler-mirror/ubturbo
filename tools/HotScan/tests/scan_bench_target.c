// SPDX-License-Identifier: GPL-2.0
/* Stable anonymous-memory target for comparing HotScan worker counts. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_target(int signal_number) { (void)signal_number; stopping = 1; }

int main(int argc, char **argv)
{
    const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    char *end;
    unsigned long mib;
    size_t length;
    volatile unsigned char *memory;
    struct timespec pause = { .tv_nsec = 1000000 };

    if (argc != 2) { fprintf(stderr, "usage: %s MiB\n", argv[0]); return 2; }
    errno = 0;
    mib = strtoul(argv[1], &end, 10);
    if (errno || *end || mib < 1 || mib > 16384) return 2;
    length = mib * 1024UL * 1024UL;
    memory = mmap(NULL, length, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) { perror("mmap"); return 1; }
    if (madvise((void *)memory, length, MADV_NOHUGEPAGE)) { perror("madvise"); return 1; }
    for (size_t offset = 0; offset < length; offset += page_size) memory[offset] = 1;
    signal(SIGTERM, stop_target);
    signal(SIGINT, stop_target);
    printf("pid=%ld mib=%lu base=%p\n", (long)getpid(), mib, (void *)memory);
    fflush(stdout);
    while (!stopping) {
        for (size_t offset = 0; offset < length; offset += 128 * page_size)
            memory[offset]++;
        nanosleep(&pause, NULL);
    }
    munmap((void *)memory, length);
    return 0;
}
