/* ref-angrylion dump.fdump -o out.rdram [-p out.ppm] [-V vi_status]...: runs a dump through the angrylion-rdp-plus n64video core (MAME-licensed, linked from the sibling checkout, kept out of the plugin) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "n64video.h"
#include "fdump.h"

void msg_error(const char *err, ...)   { va_list a; va_start(a, err); fprintf(stderr, "angrylion error: "); vfprintf(stderr, err, a); fputc('\n', stderr); va_end(a); }
void msg_warning(const char *err, ...) { va_list a; va_start(a, err); fprintf(stderr, "angrylion warning: "); vfprintf(stderr, err, a); fputc('\n', stderr); va_end(a); }
void msg_debug(const char *err, ...)   { (void)err; }

static void intr_cb(void) {}

#define RDRAM_TOTAL 0x1000000u
#define SCRATCH     0x800000u
#define h_origin_unset 0xffffffffu

int main(int argc, char **argv)
{
    const char *in = NULL, *out_rdram = NULL, *out_ppm = NULL;
    uint32_t vstat[64], vorg[64];
    int nvstat = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_rdram = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) out_ppm = argv[++i];
        else if (!strcmp(argv[i], "-V") && i + 1 < argc && nvstat < 64) { char *c = strchr(argv[i + 1], ':'); vorg[nvstat] = c ? (uint32_t)strtoul(c + 1, NULL, 0) : h_origin_unset; vstat[nvstat++] = (uint32_t)strtoul(argv[++i], NULL, 0); }
        else in = argv[i];
    }
    if (!in) { fprintf(stderr, "usage: ref-angrylion dump.fdump -o out.rdram [-p out.ppm]\n"); return 2; }

    FILE *fp = fopen(in, "rb");
    if (!fp) { perror(in); return 1; }
    struct fdump_hdr h;
    if (fread(&h, sizeof(h), 1, fp) != 1 || memcmp(h.magic, "FOIRDMP1", 8)) { fprintf(stderr, "bad dump\n"); return 1; }
    uint8_t *rdram = calloc(1, RDRAM_TOTAL);
    if (fread(rdram, 1, h.rdram_size, fp) != h.rdram_size) return 1;
    long cmd_off = ftell(fp);
    fseek(fp, 0, SEEK_END);
    size_t nbytes = (size_t)(ftell(fp) - cmd_off);
    fseek(fp, cmd_off, SEEK_SET);
    uint32_t *cmds = malloc(nbytes ? nbytes : 4);
    if (fread(cmds, 1, nbytes, fp) != nbytes) return 1;
    fclose(fp);

    uint32_t vi_regs[VI_NUM_REG] = { 0 };
    uint32_t *vi_ptr[VI_NUM_REG];
    vi_regs[VI_STATUS] = h.vi[0]; vi_regs[VI_ORIGIN] = h.vi[1]; vi_regs[VI_WIDTH] = h.vi[2];
    vi_regs[VI_H_START] = h.vi[3]; vi_regs[VI_V_START] = h.vi[4];
    vi_regs[VI_X_SCALE] = h.vi[5]; vi_regs[VI_Y_SCALE] = h.vi[6];
    vi_regs[VI_V_SYNC] = 525;   /* NTSC; without it the oracle's VI draws nothing */
    for (int i = 0; i < VI_NUM_REG; i++) vi_ptr[i] = &vi_regs[i];
    uint32_t dp_regs[DP_NUM_REG] = { 0 };
    uint32_t *dp_ptr[DP_NUM_REG];
    for (int i = 0; i < DP_NUM_REG; i++) dp_ptr[i] = &dp_regs[i];
    uint32_t mi_intr = 0;
    static uint8_t dmem[0x1000];

    struct n64video_config cfg;
    n64video_config_init(&cfg);
    cfg.parallel = false;
    cfg.gfx.rdram = rdram;
    cfg.gfx.rdram_size = RDRAM_TOTAL;
    cfg.gfx.dmem = dmem;
    cfg.gfx.mi_intr_reg = &mi_intr;
    cfg.gfx.mi_intr_cb = intr_cb;
    cfg.gfx.vi_reg = vi_ptr;
    cfg.gfx.dp_reg = dp_ptr;
    cfg.vi.mode = VI_MODE_NORMAL;
    n64video_init(&cfg);

    const uint32_t *p = cmds, *end = cmds + nbytes / 4;
    while (p < end) {
        uint32_t *dst = (uint32_t *)(rdram + SCRATCH);
        size_t used = 0;
        while (p < end) {
            uint32_t n = *p;
            if ((used + n) * 4 > RDRAM_TOTAL - SCRATCH) break;
            memcpy(dst + used, p + 1, n * 4);
            used += n;
            p += 1 + n;
        }
        dp_regs[DP_START] = dp_regs[DP_CURRENT] = SCRATCH;
        dp_regs[DP_END] = SCRATCH + (uint32_t)(used * 4);
        dp_regs[DP_STATUS] = 0;
        n64video_process_list();
    }

    if (out_rdram) {
        FILE *o = fopen(out_rdram, "wb");
        fwrite(rdram, 1, h.rdram_size, o);
        fclose(o);
    }
    /* the frame is rendered once; each -V shows it again with another VI_STATUS (the filter bits), to out.ppm.<k>.ppm */
    for (int k = (nvstat ? 0 : -1); k < nvstat && out_ppm; k++) {
        char name[1024];
        if (k >= 0) { vi_regs[VI_STATUS] = vstat[k]; vi_regs[VI_ORIGIN] = vorg[k] == h_origin_unset ? h.vi[1] : vorg[k]; snprintf(name, sizeof name, "%s.%d.ppm", out_ppm, k); }
        else snprintf(name, sizeof name, "%s", out_ppm);
        struct n64video_frame_buffer fb = { 0 };
        n64video_update_screen(&fb);
        FILE *o = fopen(name, "wb");
        int w = fb.valid ? (int)fb.width : 320, hh = fb.valid ? (int)fb.height : 240;
        fprintf(o, "P6\n%d %d\n255\n", w, hh);
        for (int y = 0; y < hh; y++)
            for (int x = 0; x < w; x++) {
                struct n64video_pixel px = fb.valid ? fb.pixels[y * fb.pitch + x] : (struct n64video_pixel){ 0 };
                fputc(px.r, o); fputc(px.g, o); fputc(px.b, o);
            }
        fclose(o);
    }
    n64video_close();
    return 0;
}
