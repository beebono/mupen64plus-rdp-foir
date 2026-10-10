/*
 * mupen64plus-rdp-foir - capture.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include "capture.h"
#include "../tools/fdump.h"

/* FOIR_DUMP=/path/prefix writes prefix_<frame>.fdump (RDRAM snapshot + every RDP command since) after FOIR_DUMP_FRAMES frames */
static struct {
    bool enabled, recording, done;
    char prefix[512];
    int frame, start_frame, nframes;
    uint8_t *snapshot;
    uint32_t snap_size;
    uint32_t last[64][8][2];
    bool have[64][8];
    uint32_t *cmds;
    size_t ncap, cap, nwords_total;
    uint32_t nprelude, ncmds;
} c;

static void trace_cb(void *ctx, const uint32_t *w, uint32_t n)
{
    (void)ctx;
    unsigned id = (w[0] >> 24) & 0x3f;
    bool is_set = (id >= 0x2a && id <= 0x3f) && id != 0x30 && id != 0x33 && id != 0x34 && id != 0x36;
    if (is_set && n == 2) {
        unsigned slot = (id == 0x35 || id == 0x32) ? ((w[1] >> 24) & 7) : 0;
        c.last[id][slot][0] = w[0];
        c.last[id][slot][1] = w[1];
        c.have[id][slot] = true;
    }
    if (!c.recording) return;
    if (c.nwords_total + n + 1 > c.cap) {
        c.cap = c.cap ? c.cap * 2 : (1u << 20);
        c.cmds = (uint32_t *)realloc(c.cmds, c.cap * sizeof(uint32_t));
    }
    c.cmds[c.nwords_total++] = n;
    for (uint32_t i = 0; i < n; i++) c.cmds[c.nwords_total++] = w[i];
    c.ncmds++;
}

void capture_init(struct rdp *r)
{
    const char *p = getenv("FOIR_DUMP");
    memset(&c, 0, sizeof(c));
    if (!p || !*p) return;
    snprintf(c.prefix, sizeof(c.prefix), "%s", p);
    const char *f = getenv("FOIR_DUMP_FRAME"), *k = getenv("FOIR_DUMP_FRAMES");
    c.start_frame = f ? atoi(f) : 200;
    c.nframes = k ? atoi(k) : 2;
    if (c.nframes < 1) c.nframes = 1;
    c.enabled = true;
    r->trace = trace_cb;
    foir_log(FOIR_LOG_INFO, "capture enabled: frame %d for %d frames -> %s_%d.fdump", c.start_frame, c.nframes, c.prefix, c.start_frame);
}

static void write_dump(struct rdp *r, const struct vi_regs *vi)
{
    char path[640];
    snprintf(path, sizeof(path), "%s_%d.fdump", c.prefix, c.start_frame);
    FILE *fp = fopen(path, "wb");
    if (!fp) { foir_log(FOIR_LOG_ERROR, "cannot write %s", path); return; }
    struct fdump_hdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "FOIRDMP1", 8);
    h.rdram_size = c.snap_size;
    h.vi[0] = vi->status; h.vi[1] = vi->origin; h.vi[2] = vi->width; h.vi[3] = vi->h_start;
    h.vi[4] = vi->v_start; h.vi[5] = vi->x_scale; h.vi[6] = vi->y_scale;
    h.nprelude = c.nprelude;
    h.ncmds = c.ncmds;
    fwrite(&h, sizeof(h), 1, fp);
    fwrite(c.snapshot, 1, c.snap_size, fp);
    fwrite(c.cmds, sizeof(uint32_t), c.nwords_total, fp);
    fclose(fp);
    foir_log(FOIR_LOG_INFO, "wrote %s (%u commands)", path, h.ncmds);
    (void)r;
}

void capture_frame(struct rdp *r, const struct vi_regs *vi)
{
    if (!c.enabled || c.done) return;
    c.frame++;
    if (!c.recording && c.frame == c.start_frame) {
        c.snap_size = r->rdram_size;
        c.snapshot = (uint8_t *)malloc(c.snap_size);
        memcpy(c.snapshot, r->rdram, c.snap_size);
        c.recording = true;
        c.ncmds = 0;
        for (int id = 0; id < 64; id++)
            for (int sl = 0; sl < 8; sl++)
                if (c.have[id][sl]) {
                    uint32_t tmp[2] = { c.last[id][sl][0], c.last[id][sl][1] };
                    trace_cb(NULL, tmp, 2);
                }
        c.nprelude = c.ncmds;
    } else if (c.recording && c.frame >= c.start_frame + c.nframes) {
        c.recording = false;
        write_dump(r, vi);
        c.done = true;
        r->trace = NULL;
        free(c.snapshot); c.snapshot = NULL;
        free(c.cmds); c.cmds = NULL;
    }
}

void capture_screenshot(const struct vi_frame *f)
{
    static int inited, frame, nshots, shots[64];
    static char dir[512];
    if (!inited) {
        inited = 1;
        const char *d = getenv("FOIR_SHOT_DIR"), *l = getenv("FOIR_SHOT_FRAMES");
        if (!d || !l) return;
        snprintf(dir, sizeof(dir), "%s", d);
        char *tmp = strdup(l), *save = NULL;
        for (char *t = strtok_r(tmp, ",", &save); t && nshots < 64; t = strtok_r(NULL, ",", &save))
            shots[nshots++] = atoi(t);
        free(tmp);
    }
    if (!nshots) return;
    frame++;
    for (int i = 0; i < nshots; i++) {
        if (shots[i] != frame) continue;
        char path[640];
        snprintf(path, sizeof(path), "%s/frame_%05d.ppm", dir, frame);
        FILE *fp = fopen(path, "wb");
        if (!fp) return;
        int w = f->valid ? f->width : 320, h = f->valid ? f->height : 240;
        fprintf(fp, "P6\n%d %d\n255\n", w, h);
        for (int k = 0; k < w * h; k++) {
            uint32_t px = 0;
            if (f->valid && f->raw16) {
                uint16_t p = ((const uint16_t *)f->pixels)[k];
                uint32_t cr = (p >> 11) & 31, cg = (p >> 6) & 31, cb = (p >> 1) & 31;
                cr = (cr << 3) | (cr >> 2); cg = (cg << 3) | (cg >> 2); cb = (cb << 3) | (cb >> 2);
                px = 0xff000000u | (cb << 16) | (cg << 8) | cr;
            } else if (f->valid) {
                px = f->pixels[k];
            }
            fputc(px & 0xff, fp); fputc((px >> 8) & 0xff, fp); fputc((px >> 16) & 0xff, fp);
        }
        fclose(fp);
    }
}

void capture_shutdown(struct rdp *r)
{
    r->trace = NULL;
    free(c.snapshot); c.snapshot = NULL;
    free(c.cmds); c.cmds = NULL;
    c.enabled = false;
}
