/*
 * foir-replay dump.fdump [-o out.rdram] [-p out.ppm] [-n repeats] [-q]
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

static struct rdp *g_skip_r;
static void skip_cb_end(void *ctx) { (void)ctx; if (g_skip_r->cfg.skip_legacy) rdp_skip_decide(g_skip_r); }
static void skip_cb_start(void *ctx) { (void)ctx; rdp_skip_decide(g_skip_r); }

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
    bool neon = true, nopix = false, fast = false, weave = false, spec = true, crop_h = false, crop_v = false;
    unsigned vi_opts = 0;   /* VI filter options allowed: 1 anti-aliasing, 2 divot, 4 dither restore, 8 gamma */
    const char *vi_variants = NULL;
    int vi_bench = 0, skip_n = 0;
    bool skip_legacy = false;
    int skip_opts = 0;
    uint32_t scan_orig[8];
    int n_scan = 0;
    int force_field = -1;
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
        else if (!strcmp(argv[i], "--field") && i + 1 < argc) force_field = atoi(argv[++i]) & 1;
        else if (!strcmp(argv[i], "--vi-filters")) vi_opts = 0xf;
        else if (!strcmp(argv[i], "--vi-opts") && i + 1 < argc) vi_opts = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--skip") && i + 1 < argc) skip_n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--skip-legacy")) skip_legacy = true;
        else if (!strcmp(argv[i], "--skip-opts") && i + 1 < argc) skip_opts = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scan") && i + 1 < argc) {
            char *l = strdup(argv[++i]);
            for (char *t = strtok(l, ","); t && n_scan < 8; t = strtok(NULL, ",")) scan_orig[n_scan++] = (uint32_t)strtoul(t, NULL, 0);
            free(l);
        }
        else if (!strcmp(argv[i], "--vi-bench") && i + 1 < argc) vi_bench = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--vi-variants") && i + 1 < argc) vi_variants = argv[++i];
        else if (!strcmp(argv[i], "--crop-h")) crop_h = true;
        else if (!strcmp(argv[i], "--crop-v")) crop_v = true;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (argv[i][0] != '-') in = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (!in) { fprintf(stderr, "usage: %s dump.fdump [-o out.rdram] [-p out.ppm] [-n repeats] [--scalar] [--nospec] [-t threads] [--fast] [--weave] [--crop-h] [--crop-v] [--field 0|1] [--vi-filters | --vi-opts mask] [--vi-variants s0[:origin[:mask]],..] [--vi-bench N] [--skip N [--skip-legacy] [--skip-opts bits]] [--scan a,b,..]\n", argv[0]); return 2; }

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
    /* the reference comparison runs with the accuracy-preserving knob settings; --fast turns RelaxedPrecision on */
    cfg.relaxed_precision = fast;
    if (weave) cfg.interlace_hack = false;
    cfg.vi_aa = vi_opts & 1; cfg.vi_divot = vi_opts & 2; cfg.vi_dedither = vi_opts & 4; cfg.vi_gamma = vi_opts & 8;
    cfg.frame_skip = skip_n;
    cfg.skip_legacy = skip_legacy;
    cfg.skip_opts = skip_opts;
    cfg.crop_h = crop_h; cfg.crop_v = crop_v;

    static struct rdp r;
#ifdef FOIR_STAGETIME
    uint64_t st_tk[8] = { 0 }, st_n[8] = { 0 }, st_cpix[3] = { 0 };
#endif
    double t_total = 0, t_submit = 0, t_wait = 0;
    uint32_t prims = 0, pixels = 0;
    for (int rep = 0; rep < repeats; rep++) {
        memcpy(rdram, snap, h.rdram_size);
        rdp_init(&r, &cfg);
        rdp_attach_memory(&r, rdram, h.rdram_size);
        rdp_start_workers(&r, threads > 0 ? threads : 0);
        g_skip_r = &r;
        r.frame_end_cb = skip_cb_end;
        r.frame_start_cb = skip_cb_start;
        rdp_fb_history_reset(&r);
        int sim_frame = 0;
#ifdef FOIR_OVERDRAW
        od_reset(&r, h.vi[1]);
#endif
        const uint32_t *p = cmds;
        double t0 = now_ms();
        for (uint32_t i = 0; i < h.ncmds; i++) {
            if (i == h.nprelude) t0 = now_ms();
            uint32_t n = *p++;
            rdp_run_command(&r, p);
            if (n_scan && ((p[0] >> 24) & 0x3f) == 0x29) {
                /* FrameSkip test: a SYNC_FULL ends a frame; the VI scans out the next origin of the list.  One line per frame: a hash of the
                 * buffer it shows and whether FrameSkip would hold the previous picture instead */
                rdp_sync(&r);
                const uint32_t org = scan_orig[sim_frame % n_scan];
                rdp_note_scanout(&r, org);
                const bool stale = rdp_fb_skipped(&r, org);
                uint32_t hsh = 2166136261u;
                for (uint32_t k = 0; k < 320 * 240 * 2 && (org & 0xffffff) + k + 4 <= h.rdram_size; k += 4) hsh = (hsh ^ *(const uint32_t *)(rdram + (org & 0xffffff) + k)) * 16777619u;
                printf("skipframe %d origin %06x hash %08x %s\n", sim_frame, org, hsh, stale ? "stale" : "shown");
                rdp_vi_scanned(&r);
                sim_frame++;
            }
            p += n;
        }
        double t_sub = now_ms();
        rdp_sync(&r);
        double t1 = now_ms();
        t_total += t1 - t0;
        t_submit += t_sub - t0;
        t_wait += r.sync_wait_ms;
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
#ifdef FOIR_OVERDRAW
    { extern void od_report(FILE *); od_report(stderr); }
#endif
#ifdef FOIR_STAGETIME
    if (repeats > 0) {
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
    if (repeats > 0 && threads > 0) {
        fprintf(stderr, "foir-replay: parser thread %.3f ms/run until the last primitive was queued, %.3f ms waiting for workers (%d workers)\n",
                t_submit / repeats, (t_total - t_submit) / repeats, threads);
        fprintf(stderr, "foir-replay: emulation thread %.3f ms/run: own work %.3f, waiting at SYNC_FULL %.3f\n",
                t_submit / repeats, (t_submit - t_wait) / repeats, t_wait / repeats);
    }

    if (out_rdram) {
        FILE *o = fopen(out_rdram, "wb");
        if (!o) { perror(out_rdram); return 1; }
        fwrite(rdram, 1, h.rdram_size, o);
        fclose(o);
    }
    if (out_ppm) {
        static struct vi_frame fr;
        if (force_field >= 0) { r.scfield = 1; r.sckeepodd = force_field; }   /* test hook: a field-rendered frame for InterlaceHack */
        struct vi_regs v = { h.vi[0], h.vi[1], h.vi[2], h.vi[3], h.vi[4], h.vi[5], h.vi[6] };
        vi_render(&r, &v, &fr);
        FILE *o = fopen(out_ppm, "wb");
        if (!o) { perror(out_ppm); return 1; }
        int w = fr.valid ? fr.width : 320, hh = fr.valid ? fr.height : 240;
        fprintf(stderr, "foir-replay: vi frame %dx%d screen %.4f x %.4f\n", w, hh, fr.screen_w, fr.screen_h);
        fprintf(o, "P6\n%d %d\n255\n", w, hh);
        for (int i = 0; i < w * hh; i++) {
            uint32_t px = fr.valid ? fr.pixels[i] : 0;
            fputc(px & 0xff, o); fputc((px >> 8) & 0xff, o); fputc((px >> 16) & 0xff, o);
        }
        fclose(o);
    }
    if (vi_variants && out_ppm) {
        /* VI filter test hook: show the rendered frame again with each VI_STATUS in the list, with the reference code (written to
         * out.ppm.<k>.ppm) and with the NEON code, and count the differing pixels between the two */
        static struct vi_frame fa, fb;
        char list[16384];
        snprintf(list, sizeof list, "%s", vi_variants);
        int k = 0, bad = 0;
        for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ","), k++) {
            char *colon = strchr(tok, ':');   /* status[:origin[:options]] */
            char *colon2 = colon ? strchr(colon + 1, ':') : NULL;
            unsigned mask = colon2 ? (unsigned)strtoul(colon2 + 1, NULL, 0) : 0xf;
            struct vi_regs v = { (uint32_t)strtoul(tok, NULL, 0), colon ? (uint32_t)strtoul(colon + 1, NULL, 0) : h.vi[1], h.vi[2], h.vi[3], h.vi[4], h.vi[5], h.vi[6] };
            r.cfg.vi_aa = mask & 1; r.cfg.vi_divot = mask & 2; r.cfg.vi_dedither = mask & 4; r.cfg.vi_gamma = mask & 8;
            struct vi_filter_ctx fc;
            const bool pass = mask && vi_filter_setup(&r, &v, &fc);
            r.cfg.neon = false; vi_render(&r, &v, &fa);
            r.cfg.neon = true;  vi_render(&r, &v, &fb);
            int diff = fa.width != fb.width || fa.height != fb.height;
            for (int i = 0; !diff && i < fa.width * fa.height; i++) diff += fa.pixels[i] != fb.pixels[i];
            bad += diff != 0;
            printf("vi variant %d status %#x options %#x: %dx%d, filter pass %s, NEON vs reference: %d differing\n", k, v.status, mask, fa.width, fa.height, pass ? "yes" : "no", diff);
            char name[1024];
            snprintf(name, sizeof name, "%s.%d.ppm", out_ppm, k);
            FILE *o = fopen(name, "wb");
            if (!o) { perror(name); return 1; }
            fprintf(o, "P6\n%d %d\n255\n", fa.width, fa.height);
            for (int i = 0; i < fa.width * fa.height; i++) { uint32_t px = fa.pixels[i]; fputc(px & 0xff, o); fputc((px >> 8) & 0xff, o); fputc((px >> 16) & 0xff, o); }
            fclose(o);
        }
        if (bad) { fprintf(stderr, "foir-replay: %d VI variants differ between NEON and the reference\n", bad); return 1; }
        out_ppm = NULL;
    }
    if (vi_bench > 0) {
        /* time of the VI conversion of the rendered frame with the options listed in modes[] (the game status has every filter on), NEON build
         * and (r.cfg.neon off) the reference code */
        static struct vi_frame fr;
        struct vi_regs v0 = { h.vi[0], h.vi[1], h.vi[2], h.vi[3], h.vi[4], h.vi[5], h.vi[6] };
        {
            int w = (int)((((h.vi[3] & 0x3ff) - ((h.vi[3] >> 16) & 0x3ff)) * (h.vi[5] & 0xfff)) >> 10), hh = (int)(((((h.vi[4] & 0x3ff) - ((h.vi[4] >> 16) & 0x3ff)) >> 1) * (h.vi[6] & 0xfff)) >> 10);
            long part = 0, tot = 0, grp = 0;
            for (int y = 0; y < hh; y++)
                for (int x = 0; x < w; x += 16) {
                    int any = 0;
                    for (int k = 0; k < 16 && x + k < w; k++) {
                        uint16_t p; uint8_t hv;
                        rdram_rd_pair16(&r, ((h.vi[1] & 0xffffff) >> 1) + (uint32_t)(y * (h.vi[2] & 0xfff) + x + k), &p, &hv);
                        int partial = (((p & 1) << 2) | hv) != 7;
                        part += partial; tot++; any |= partial;
                    }
                    grp += any;
                }
            printf("vi bench coverage: %.2f %% of the pixels below full coverage, %.1f %% of the 16-pixel groups hold one\n", 100.0 * part / tot, 100.0 * grp / ((tot + 15) / 16));
        }
        /* the game status has every filter on (aa_mode 0, divot, dither restore, gamma); the options choose which of them run */
        static const struct { const char *name; unsigned opts; bool neon; } modes[] = {
            { "unfiltered (no option on)", 0x0, true },
            { "AA", 0x1, true },
            { "divot", 0x2, true },
            { "dither restore", 0x4, true },
            { "gamma", 0x8, true },
            { "AA + divot", 0x3, true },
            { "AA + dither restore", 0x5, true },
            { "divot + dither restore", 0x6, true },
            { "AA + divot + dither restore", 0x7, true },
            { "all four", 0xf, true },
            { "reference code, all four", 0xf, false },
        };
        for (unsigned mode = 0; mode < sizeof modes / sizeof modes[0]; mode++) {
            r.cfg.vi_aa = modes[mode].opts & 1; r.cfg.vi_divot = modes[mode].opts & 2; r.cfg.vi_dedither = modes[mode].opts & 4; r.cfg.vi_gamma = modes[mode].opts & 8;
            r.cfg.neon = modes[mode].neon;
            struct vi_regs v = v0;
            v.status = (h.vi[0] & ~0x1031cu) | 0x10018u;
            struct timespec t0, t1;
            vi_render(&r, &v, &fr);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (int k = 0; k < vi_bench; k++) vi_render(&r, &v, &fr);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6) / vi_bench;
            printf("vi bench %-32s %dx%d  %.3f ms/frame\n", modes[mode].name, fr.width, fr.height, ms);
        }
    }
    rdp_destroy(&r);
    free(snap); free(rdram); free(cmds);
    return 0;
}
