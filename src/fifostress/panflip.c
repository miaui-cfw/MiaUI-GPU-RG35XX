/**
 * @file panflip.c
 * @brief D7 "same-address flip" test (tests only, no GL): drives fb0 flips by hand, once per vsync, to tell whether
 *        the per-frame overlay apply + FCR "go" latch is what makes the DE FIFO fragile, or the change of scan-out
 *        address.
 *
 *    panflip SECONDS none|same|alt|tri [--at US] [--setvar] [--base S]
 *
 *  none  wait for each vsync, never pan (static display: nothing reprograms the DE)
 *  same  pan to yoffset 0 every frame (owlfb_pan_display re-applies overlay 0 + sets FCR, same address)
 *  alt   pan to yoffset 0 / 480 alternately (a real page flip)
 *  tri   rotate yoffset 0 / 480 / 960, the GPU driver's 3-buffer swap chain
 *
 *  --at US   busy-wait US microseconds after the vsync before flipping, so the flip lands mid-scan, as the GPU
 *            driver's flips do (OMAPLFB flips from a work queue when the GPU finishes; its vsync wait is a stub)
 *  --setvar  flip the way OMAPLFBFlip does: FBIOPUT_VSCREENINFO (fb_set_var) with yres_virtual = yoffset + yres and
 *            FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE, i.e. owlfb_set_par + owlfb_pan_display: two overlay applies and two
 *            FCR "go" writes per flip, instead of FBIOPAN_DISPLAY's one
 *  --base S  add S screens to every y offset: scan fb0 screens S, S+1, ... (fb0 holds 10; the GPU driver's swap
 *            chain renders into screens 0-2, wcstress writes screens 6-9, so S = 3 or 4 is a part nobody writes)
 *
 * Underflows are counted the same way in every mode: the sticky DE_OUTPUT_STAT (DE 0xB02E0000 + 0x100C) is read
 * through /dev/mem (read only) right after each vsync and before this frame's pan, i.e. before de_path_fcr_set reads
 * and clears it. A frame counts once. Run with MinUI parked and wcstress beside it (d7.sh does both).
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FBIO_OWL_WAITFORVSYNC _IOW('O', 57, long long) /* include/video/owlfb.h OWLFB_WAITFORVSYNC */

/** @brief include/video/owlfb.h struct owlfb_sync_info (OWLFB_VSYNC_EVENT_EN's argument). */
struct OwlSyncInfo
{
    uint8_t enabled;
    uint8_t dispId;
    uint16_t reserved;
};
#define FBIO_OWL_VSYNC_EVENT_EN _IOW('O', 67, struct OwlSyncInfo)

#define DE_PAGE 0xB02E1000u    /* page holding DE_OUTPUT_STAT */
#define DE_OUTPUT_STAT_OFF 0x00Cu

/** @brief Monotonic time in seconds. */
static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char** argv)
{
    if (argc < 3 || (strcmp(argv[2], "none") && strcmp(argv[2], "same") && strcmp(argv[2], "alt") &&
                     strcmp(argv[2], "tri"))) {
        fprintf(stderr, "usage: %s SECONDS none|same|alt|tri [--at US] [--setvar] [--base S]\n", argv[0]);
        return 2;
    }
    const double secs = atof(argv[1]);
    const char* mode = argv[2];
    long atUs = 0;
    int setvar = 0;
    unsigned base = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--at") && i + 1 < argc) atUs = atol(argv[++i]);
        else if (!strcmp(argv[i], "--setvar")) setvar = 1;
        else if (!strcmp(argv[i], "--base") && i + 1 < argc) base = (unsigned)atoi(argv[++i]);
    }

    const int fb = open("/dev/fb0", O_RDWR);
    const int mem = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fb < 0 || mem < 0) {
        perror(fb < 0 ? "/dev/fb0" : "/dev/mem");
        return 1;
    }
    volatile const uint32_t* de = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mem, DE_PAGE);
    if (de == MAP_FAILED) {
        perror("mmap DE");
        return 1;
    }
    struct fb_var_screeninfo var;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &var) < 0) {
        perror("FBIOGET_VSCREENINFO");
        return 1;
    }
    const unsigned need = !strcmp(mode, "tri") ? 3 : !strcmp(mode, "alt") ? 2 : 1;
    if (!setvar && var.yres_virtual < (base + need) * var.yres) {
        fprintf(stderr, "panflip: yres_virtual %u < %u screens: run fbset first or use --setvar\n", var.yres_virtual,
                base + need);
        return 1;
    }
    /* without this the driver's vsync wait returns at once (as in fifostress) */
    struct OwlSyncInfo on = { 1, 0, 0 };
    ioctl(fb, FBIO_OWL_VSYNC_EVENT_EN, &on);

    var.xoffset = 0;
    var.yoffset = base * var.yres;
    if (setvar) {
        var.yres_virtual = var.yoffset + var.yres;
        var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
        ioctl(fb, FBIOPUT_VSCREENINFO, &var);
    } else {
        ioctl(fb, FBIOPAN_DISPLAY, &var);
    }

    long frames = 0, under = 0;
    const double t0 = now();
    while (now() - t0 < secs) {
        long long ts = 0;
        ioctl(fb, FBIO_OWL_WAITFORVSYNC, &ts);
        if (de[DE_OUTPUT_STAT_OFF / 4])
            under++;    /* sticky since the last clear (a flip, or never in mode none) */
        if (!strcmp(mode, "none")) {
            /* nothing clears the sticky bit without a flip (and this tool never writes it): only "seen or not"
               and when it was first seen are meaningful */
            if (under == 1) printf("panflip: first underflow at %.2f s\n", now() - t0);
        } else {
            if (atUs > 0) {
                /* busy-wait: a sleep's wake-up jitter would blur the flip position */
                const double until = now() + atUs * 1e-6;
                while (now() < until) {}
            }
            var.yoffset = (base + (unsigned)(frames % need)) * var.yres;   /* same: S; alt: S/S+1; tri: S..S+2 */
            if (setvar) {
                /* as OMAPLFBFlip with PVR_OMAPLFB_DONT_USE_FB_PAN_DISPLAY */
                var.xres_virtual = var.xres;
                var.yres_virtual = var.yoffset + var.yres;
                var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
                ioctl(fb, FBIOPUT_VSCREENINFO, &var);
            } else {
                ioctl(fb, FBIOPAN_DISPLAY, &var);
            }
        }
        frames++;
    }
    const double t = now() - t0;
    if (!strcmp(mode, "none"))
        printf("RESULT panflip none: underflow %s, set at %ld of %ld vsyncs fps %.1f\n", under ? "SEEN" : "never",
               under, frames, frames / t);
    else
        printf("RESULT panflip %s%s at %ld us base %u: underflow frames %ld of %ld fps %.1f\n", mode,
               setvar ? " setvar" : "", atUs, base, under, frames, frames / t);
    var.yoffset = 0;
    ioctl(fb, FBIOPAN_DISPLAY, &var);
    return 0;
}
