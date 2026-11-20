/**
 * @file wcstress.c
 * @brief Write-combined store stress (tests only): streams CPU writes into an off-screen part of /dev/fb0 (mapped
 *        write-combined, like the GPU's texture memory the games upload into), to reproduce the DE FIFO underflow
 *        without the GPU or a game. The region used (screens 6-9 of fb0's 10 640x480x32 screens) is never displayed:
 *        the PVR display class flips among the first 3.
 *
 *    wcstress SECONDS [--mode set|copy] [--kb N] [--gap-us N] [--cpu C]
 *
 *  --mode set   stores only (memset-like, NEON-width via the compiler's memset)    (default)
 *  --mode copy  memcpy from a cached 1 MB source (reads cached, writes WC: like a texture upload)
 *  --kb N       burst size in KB per iteration (default 1024)
 *  --gap-us N   sleep between bursts (default 0: continuous)
 *  --cpu C      pin to CPU C (default: not pinned)
 * Prints the achieved write rate. Underflows are counted by the caller from dmesg.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SCREEN (640u * 480u * 4u)
#define FIRST_SCREEN 6u
#define LAST_SCREEN 10u

/** @brief Monotonic time in seconds. */
static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s SECONDS [--mode set|copy] [--kb N] [--gap-us N] [--cpu C]\n", argv[0]);
        return 2;
    }
    const double secs = atof(argv[1]);
    int copy = 0, cpu = -1;
    size_t kb = 1024;
    long gap = 0;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--mode")) copy = !strcmp(argv[i + 1], "copy");
        else if (!strcmp(argv[i], "--kb")) kb = strtoul(argv[i + 1], NULL, 0);
        else if (!strcmp(argv[i], "--gap-us")) gap = atol(argv[i + 1]);
        else if (!strcmp(argv[i], "--cpu")) cpu = atoi(argv[i + 1]);
    }
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        sched_setaffinity(0, sizeof set, &set);
    }
    const int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("/dev/fb0");
        return 1;
    }
    const size_t mapLen = (size_t)LAST_SCREEN * SCREEN;
    uint8_t* fb = mmap(NULL, mapLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    uint8_t* region = fb + (size_t)FIRST_SCREEN * SCREEN;
    const size_t regionLen = (size_t)(LAST_SCREEN - FIRST_SCREEN) * SCREEN;
    const size_t burst = kb << 10;
    uint8_t* src = malloc(1u << 20);
    if (!src || burst > regionLen) return 1;
    memset(src, 0x5a, 1u << 20);

    size_t off = 0;
    unsigned long long bytes = 0;
    const double t0 = now();
    while (now() - t0 < secs) {
        for (size_t done = 0; done < burst;) {
            size_t n = burst - done;
            if (n > (1u << 20)) n = 1u << 20;
            if (off + n > regionLen) off = 0;
            if (copy) memcpy(region + off, src, n);
            else memset(region + off, (int)(bytes >> 20) & 0xff, n);
            off += n;
            done += n;
        }
        bytes += burst;
        if (gap > 0) usleep(gap);
    }
    const double t = now() - t0;
    printf("wcstress: %s %zu KB bursts, gap %ld us: %.0f MB in %.1f s = %.0f MB/s\n", copy ? "copy" : "set", kb, gap,
           bytes / 1048576.0, t, bytes / 1048576.0 / t);
    munmap(fb, mapLen);
    close(fd);
    return 0;
}
