/*
 * foir-replay: offline driver for the foir renderer.
 *
 *   foir-replay dump.fdump [-o out.rdram] [-p out.ppm] [-n repeats] [-q]
 *
 * Loads an RDRAM snapshot and the recorded RDP commands (see fdump.h), runs
 * them through the scalar reference pipeline, and writes the final RDRAM and a
 * PPM of the VI output. With -n it repeats the replay and reports timing.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/rdp.h"
#include "../src/vi.h"
#include "fdump.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out_rdram = NULL, *out_ppm = NULL;
    int repeats = 1;
    bool neon = true, nopix = false, fast = false, weave = false, spec = true;
    int threads = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_rdram = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) out_ppm = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) repeats = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--neon")) neon = true;
        else if (!strcmp(argv[i], "--scalar")) neon = false;
        else if (!strcmp(argv[i], "--nospec")) spec = false;
        else if (!strcmp(argv[i], "--setup-only")) nopix = true;
        else if (!strcmp(argv[i], "--fast")) fast = true;
        else if (!strcmp(argv[i], "--weave")) weave = true;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (argv[i][0] != '-') in = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!in) { fprintf(stderr, "usage: %s dump.fdump [-o out.rdram] [-p out.ppm] [-n repeats] [--scalar] [--nospec] [-t threads] [--fast] [--weave]\n", argv[0]); return 2; }

    FILE *fp = fopen(in, "rb");
    if (!fp) { perror(in); return 1; }
    struct fdump_hdr h;
    if (fread(&h, sizeof(h), 1, fp) != 1 || memcmp(h.magic, "FOIRDMP1", 8)) { fprintf(stderr, "bad dump\n"); return 1; }
    uint8_t *snap = malloc(h.rdram_size);
    uint8_t *rdram = malloc(h.rdram_size);
    if (fread(snap, 1, h.rdram_size, fp) != h.rdram_size) { fprintf(stderr, "short read\n"); return 1; }
    fseek(fp, 0, SEEK_END);
    long total = ftell(fp);
    long cmd_off = (long)(sizeof(h) + h.rdram_size);
    fseek(fp, cmd_off, SEEK_SET);
    size_t nbytes = (size_t)(total - cmd_off);
    uint32_t *cmds = malloc(nbytes ? nbytes : 4);
    if (fread(cmds, 1, nbytes, fp) != nbytes) { fprintf(stderr, "short read\n"); return 1; }
    fclose(fp);

    struct foir_config cfg;
    foir_config_defaults(&cfg);
    cfg.neon = neon;
    cfg.specialize = spec;
    cfg.debug_nopixels = nopix;
    /* the reference comparison runs with the accuracy-preserving knob settings; --fast uses the plugin defaults */
    if (!fast) cfg.relaxed_precision = false;
    if (weave) cfg.interlace_hack = false;

    static struct rdp r;
#ifdef FOIR_STAGETIME
    uint64_t st_tk[8] = { 0 }, st_n[8] = { 0 }, st_cpix[3] = { 0 };
#endif
    double t_total = 0, t_submit = 0;
    uint32_t prims = 0, pixels = 0;
    for (int rep = 0; rep < repeats; rep++) {
        memcpy(rdram, snap, h.rdram_size);
        rdp_init(&r, &cfg);
        rdp_attach_memory(&r, rdram, h.rdram_size);
        rdp_start_workers(&r, threads > 0 ? threads : 0);
        const uint32_t *p = cmds;
        /* run prelude (state only) untimed, then time the frame commands */
        double t0 = now_ms();
        for (uint32_t i = 0; i < h.ncmds; i++) {
            if (i == h.nprelude) t0 = now_ms();
            uint32_t n = *p++;
            rdp_run_command(&r, p);
            p += n;
        }
        double t_sub = now_ms();
        rdp_sync(&r);
        double t1 = now_ms();
        t_total += t1 - t0;
        t_submit += t_sub - t0;
        prims = r.stat_prims;
        pixels = rdp_pixels(&r);
#ifdef FOIR_STAGETIME
        for (int i = 0; i < 8; i++) { st_tk[i] += r.st_tk[i]; st_n[i] += r.st_n[i]; }
        for (int i = 0; i < 3; i++) st_cpix[i] += r.st_cpix[i];
#endif
        if (rep + 1 < repeats) rdp_destroy(&r);
    }
    if (repeats > 0)
        fprintf(stderr, "foir-replay: %u cmds (%u prelude), %u prims, %u pixels, %.3f ms/run (kernels=%s%s)\n",
                h.ncmds, h.nprelude, prims, pixels, t_total / repeats, r.k->name,
                threads > 0 ? "" : "");
    if (repeats > 0 && r.spec_ok_mask) {
        uint64_t cov[COV_N], texall;
        rdp_coverage(&r, cov, &texall);
        double all = (double)cov[5];
        fprintf(stderr, "foir-replay: specialised pixels: texture %.1f%% (of %.0f%% textured), combine %.1f%%, zcmp %.1f%%, blend %.1f%%, store %.1f%%\n",
                texall ? 100.0 * (double)cov[0] / (double)texall : 0.0, all > 0 ? 100.0 * (double)texall / all : 0.0,
                all > 0 ? 100.0 * (double)cov[1] / all : 0.0, all > 0 ? 100.0 * (double)cov[2] / all : 0.0,
                all > 0 ? 100.0 * (double)cov[3] / all : 0.0, all > 0 ? 100.0 * (double)cov[4] / all : 0.0);
        fprintf(stderr, "foir-replay: textured pixels: exact variants %.1f%%, \"any format\" kernels %.1f%%, generic kernel %.1f%%\n",
                texall ? 100.0 * (double)(cov[0] - cov[7]) / (double)texall : 0.0, texall ? 100.0 * (double)cov[7] / (double)texall : 0.0,
                texall ? 100.0 * (double)(texall - cov[0]) / (double)texall : 0.0);
        fprintf(stderr, "foir-replay: combiner pixels: exact variants %.1f%%, shape kernels %.1f%%, generic kernel %.1f%%\n",
                all > 0 ? 100.0 * (double)(cov[1] - cov[6]) / all : 0.0, all > 0 ? 100.0 * (double)cov[6] / all : 0.0,
                all > 0 ? 100.0 * (double)(cov[5] - cov[1]) / all : 0.0);
    }
#ifdef FOIR_STAGETIME
    if (repeats > 0) {
        /* the cost of reading the timer itself, taken off every interval */
        uint64_t freq, a, b2, sum = 0;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
        for (int i = 0; i < 200000; i++) {
            __asm__ volatile("mrs %0, cntvct_el0" : "=r"(a) :: "memory");
            __asm__ volatile("mrs %0, cntvct_el0" : "=r"(b2) :: "memory");
            sum += b2 - a;
        }
        const double ovh = (double)sum / 200000.0, tms = 1000.0 / (double)freq / repeats;
        static const char *nm[] = { "texture", "combine", "zcmp", "blend", "store" };
        fprintf(stderr, "foir-replay: stage time per run (ms, timer %.1f MHz, read overhead %.3f ticks):", (double)freq / 1e6, ovh);
        for (int i = 0; i < 5; i++) {
            if (i == 1) {
                uint64_t tk = st_tk[5] + st_tk[6] + st_tk[7], nn = st_n[5] + st_n[6] + st_n[7];
                fprintf(stderr, " combine %.3f", ((double)tk - ovh * (double)nn) * tms);
            } else {
                fprintf(stderr, " %s %.3f", nm[i], ((double)st_tk[i] - ovh * (double)(i == 0 ? st_n[0] : st_n[5] + st_n[6] + st_n[7])) * tms);
            }
        }
        fprintf(stderr, "\n");
        static const char *kn[] = { "exact", "shape", "generic" };
        fprintf(stderr, "foir-replay: combine by kernel kind:");
        for (int i = 0; i < 3; i++) {
            double ms = ((double)st_tk[5 + i] - ovh * (double)st_n[5 + i]) * tms;
            fprintf(stderr, " %s %.3f ms / %.0f px (%.2f ns/px, %.1f px/batch);", kn[i], ms, (double)st_cpix[i] / repeats,
                    st_cpix[i] ? ms * 1e6 * repeats / (double)st_cpix[i] : 0.0, st_n[5 + i] ? (double)st_cpix[i] / (double)st_n[5 + i] : 0.0);
        }
        fprintf(stderr, "\n");
    }
#endif
    if (repeats > 0 && threads > 0)
        fprintf(stderr, "foir-replay: parser thread %.3f ms/run until the last primitive was queued, %.3f ms waiting for workers (%d workers)\n",
                t_submit / repeats, (t_total - t_submit) / repeats, threads);

    if (out_rdram) {
        FILE *o = fopen(out_rdram, "wb");
        if (!o) { perror(out_rdram); return 1; }
        fwrite(rdram, 1, h.rdram_size, o);
        fclose(o);
    }
    if (out_ppm) {
        static struct vi_frame fr;
        struct vi_regs v = { h.vi[0], h.vi[1], h.vi[2], h.vi[3], h.vi[4], h.vi[5], h.vi[6] };
        vi_render(&r, &v, &fr);
        FILE *o = fopen(out_ppm, "wb");
        if (!o) { perror(out_ppm); return 1; }
        int w = fr.valid ? fr.width : 320, hh = fr.valid ? fr.height : 240;
        fprintf(o, "P6\n%d %d\n255\n", w, hh);
        for (int i = 0; i < w * hh; i++) {
            uint32_t px = fr.valid ? fr.pixels[i] : 0;
            fputc(px & 0xff, o); fputc((px >> 8) & 0xff, o); fputc((px >> 16) & 0xff, o);
        }
        fclose(o);
    }
    rdp_destroy(&r);
    free(snap); free(rdram); free(cmds);
    return 0;
}
