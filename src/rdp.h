/*
 * mupen64plus-rdp-foir - rdp.h
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FOIR_RDP_H
#define FOIR_RDP_H

#include "foir.h"
#include "config.h"

#define PIXEL_SIZE_4BIT   0
#define PIXEL_SIZE_8BIT   1
#define PIXEL_SIZE_16BIT  2
#define PIXEL_SIZE_32BIT  3

#define CYCLE_TYPE_1      0
#define CYCLE_TYPE_2      1
#define CYCLE_TYPE_COPY   2
#define CYCLE_TYPE_FILL   3

#define FORMAT_RGBA       0
#define FORMAT_YUV        1
#define FORMAT_CI         2
#define FORMAT_IA         3
#define FORMAT_I          4

#define ZMODE_OPAQUE           0
#define ZMODE_INTERPENETRATING 1
#define ZMODE_TRANSPARENT      2
#define ZMODE_DECAL            3

#define CVG_CLAMP  0
#define CVG_WRAP   1
#define CVG_ZAP    2
#define CVG_SAVE   3

#define DP_STATUS_XBUS_DMA  0x001

#define MI_INTR_DP          0x20

enum {
    CMD_NOOP = 0x00,
    CMD_FILL_TRI = 0x08, CMD_FILL_ZBUF_TRI = 0x09,
    CMD_TEX_TRI = 0x0a,  CMD_TEX_ZBUF_TRI = 0x0b,
    CMD_SHADE_TRI = 0x0c, CMD_SHADE_ZBUF_TRI = 0x0d,
    CMD_SHADE_TEX_TRI = 0x0e, CMD_SHADE_TEX_ZBUF_TRI = 0x0f,
    CMD_TEX_RECT = 0x24, CMD_TEX_RECT_FLIP = 0x25,
    CMD_SYNC_LOAD = 0x26, CMD_SYNC_PIPE = 0x27, CMD_SYNC_TILE = 0x28, CMD_SYNC_FULL = 0x29,
    CMD_SET_KEY_GB = 0x2a, CMD_SET_KEY_R = 0x2b, CMD_SET_CONVERT = 0x2c,
    CMD_SET_SCISSOR = 0x2d, CMD_SET_PRIM_DEPTH = 0x2e, CMD_SET_OTHER_MODES = 0x2f,
    CMD_LOAD_TLUT = 0x30, CMD_SET_TILE_SIZE = 0x32, CMD_LOAD_BLOCK = 0x33,
    CMD_LOAD_TILE = 0x34, CMD_SET_TILE = 0x35, CMD_FILL_RECT = 0x36,
    CMD_SET_FILL_COLOR = 0x37, CMD_SET_FOG_COLOR = 0x38, CMD_SET_BLEND_COLOR = 0x39,
    CMD_SET_PRIM_COLOR = 0x3a, CMD_SET_ENV_COLOR = 0x3b, CMD_SET_COMBINE = 0x3c,
    CMD_SET_TEXTURE_IMAGE = 0x3d, CMD_SET_MASK_IMAGE = 0x3e, CMD_SET_COLOR_IMAGE = 0x3f
};

#define CMD_MAX_BYTES 176
#define CMD_MAX_WORDS (CMD_MAX_BYTES / 4)

#define SIGN(x, n)   ((int32_t)(((uint32_t)(x) & ((1u << (n)) - 1)) | -((uint32_t)(x) & (1u << ((n) - 1)))))
#define SIGN16(x)    ((int16_t)(x))
#define TRELATIVE(x, y) ((x) - ((y) << 3))

FOIR_INLINE uint32_t dz_compress(uint32_t value)
{
    uint32_t j = 0;
    if (value & 0xff00) j |= 8;
    if (value & 0xf0f0) j |= 4;
    if (value & 0xcccc) j |= 2;
    if (value & 0xaaaa) j |= 1;
    return j;
}

FOIR_INLINE int32_t foir_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct color { int32_t r, g, b, a; };

#define COV_N 8

struct tile {
    int format, size, line, tmem, palette;
    int ct, mt, cs, ms;
    int mask_t, shift_t, mask_s, shift_s;
    uint16_t sl, tl, sh, th;
    int clampdiffs, clampdifft;
    int clampens, clampent;
    int masksclamped, masktclamped;
};

struct other_modes {
    int cycle_type, persp_tex_en, detail_tex_en, sharpen_tex_en, tex_lod_en;
    int en_tlut, tlut_type, sample_type, mid_texel, bi_lerp0, bi_lerp1, convert_one, key_en;
    int rgb_dither_sel, alpha_dither_sel;
    int blend_m1a[2], blend_m1b[2], blend_m2a[2], blend_m2b[2];
    int force_blend, alpha_cvg_select, cvg_times_alpha, z_mode, cvg_dest, color_on_cvg;
    int image_read_en, z_update_en, z_compare_en, antialias_en, z_source_sel;
    int dither_alpha_en, alpha_compare_en;
    int rgb_alpha_dither;
    int dolod;
    int uses_texel0, uses_texel1;
    int uses_noise;
    int partialreject[2];
    int real_blendshifts;
};

/* combiner register file layout; each colour group is r,g,b,a */
enum {
    REG_COMB = 0, REG_TEX0 = 4, REG_TEX1 = 8, REG_PRIM = 12, REG_SHADE = 16, REG_ENV = 20,
    REG_ZERO = 24, REG_ONE = 25, REG_NOISE = 26, REG_K4 = 27, REG_K5 = 28,
    REG_KEYC = 29,
    REG_KEYS = 32,
    REG_LODF = 35, REG_PLODF = 36,
    REG_COUNT = 40
};

struct combiner {
    uint8_t rgb[2][4][3];  /* [cycle][suba, subb, mul, add][channel] -> reg index */
    uint8_t alpha[2][4];   /* [cycle][suba, subb, mul, add] -> reg index */
    int raw[16];
};

struct span {
    int lx, rx, unscrx, validline;
    union {
        struct { int32_t r, g, b, a, s, t, w, z; };
        int32_t att[8];
    };
    int32_t majorx[4], minorx[4], invalyscan[4];
};

/* Everything the edge walker needs for the span of any scanline. Scanline j covers subscanlines k = 4j .. 4j+3; edge positions are linear in k
 * (wrapping 32-bit): xright = xr0 + (k - ycur) * xri; xleft = xm0 + (k - ycur) * xmi for k < ysw, else xl0 + (k - ysw) * xli;
 * interpolants of scanline j = a0 + (j - ycur / 4) * de. Arrays are in the order r g b a s t w z. */
#define ALIGN16 __attribute__((aligned(16)))

struct prim_walk {
    int32_t ycur;
    int32_t ysw;                   /* subscanline at which the left edge switches to xl (INT32_MAX: never) */
    int32_t yhlimit, yllimit;
    int32_t clipxl2, clipxh2;      /* scissor, in the walker's x units (<< 1) */
    uint32_t xr0, xri;
    uint32_t xm0, xmi;
    uint32_t xl0, xli;
    int32_t flip, ldflag;          /* ldflag: subscanline (0 or 3) whose x position adjusts the interpolants */
    int32_t scfield, sckeepodd;
    ALIGN16 uint32_t a0[8];        /* interpolants at row ycur / 4 */
    ALIGN16 uint32_t de[8];
    ALIGN16 uint32_t diff[8];
    ALIGN16 uint32_t dxh[8];
};

#ifndef FOIR_BATCH
#define FOIR_BATCH 64
#endif
#define FOIR_LANES (FOIR_BATCH + 16)  /* arrays carry slack so vector stores may overrun */
#define SK_NKC 24

enum { CH_R = 0, CH_G = 1, CH_B = 2, CH_A = 3 };

struct prim_ctx {
    int flip;
    int tilenum;
    int max_level;
    ALIGN16 int32_t dr;
    int32_t dg, db, da, ds, dt, dw, dz;
    /* the same in the hardware iteration direction (flip ? +d : -d): the RDP's "next pixel" is at coordinate + dxinc */
    ALIGN16 int32_t drinc;
    int32_t dginc, dbinc, dainc, dsinc, dtinc, dwinc, dzinc;
    int32_t drdy, dgdy, dbdy, dady, dzdy;
    int32_t cdr, cdg, cdb, cda, cdz;
    int32_t dsdy, dtdy, dwdy;
    int32_t dzpix;
    int32_t dzpixenc;
    uint32_t seed;
};

struct px_batch {
    int n;
    int all_full;
    int fbrange;                /* fbmin / fbmax are valid (set by the batch builder; else the memory kernels scan fbidx) */
    uint32_t fbmin, fbmax;
    ALIGN16 uint32_t fbidx[FOIR_LANES];
    ALIGN16 uint8_t  ditidx[FOIR_LANES];  /* dither matrix index ((y >> scfield) & 3) * 4 + (x & 3) */
    ALIGN16 int16_t  xpos[FOIR_LANES];    /* row-local; vector stores must not touch other rows */
    ALIGN16 int32_t s[FOIR_LANES], t[FOIR_LANES], w[FOIR_LANES];
    ALIGN16 int32_t cr[FOIR_LANES], cg[FOIR_LANES], cb[FOIR_LANES], ca[FOIR_LANES], cz[FOIR_LANES];
    ALIGN16 uint8_t cvg[FOIR_LANES], cvbit[FOIR_LANES], offx[FOIR_LANES], offy[FOIR_LANES];
    /* tex0n: TEXEL0 at the "next" pixel (+d*inc); only produced for two-cycle combiners whose second cycle reads TEXEL1 */
    ALIGN16 int16_t tex0[4][FOIR_LANES], tex1[4][FOIR_LANES], tex0n[4][FOIR_LANES];
    ALIGN16 int16_t lodfrac[FOIR_LANES];
    ALIGN16 int16_t pixel[4][FOIR_LANES];       /* final combiner colour (0..255, alpha 0..256) */
    ALIGN16 int16_t shade_alpha[FOIR_LANES];
    ALIGN16 int16_t acalpha[FOIR_LANES];
    ALIGN16 int16_t cdith[FOIR_LANES];
    ALIGN16 int8_t  adith[FOIR_LANES];
    ALIGN16 int32_t sz[FOIR_LANES];
    ALIGN16 int16_t mem[4][FOIR_LANES];
    ALIGN16 uint8_t memcvg[FOIR_LANES];
    ALIGN16 uint8_t blend_en[FOIR_LANES], prewrap[FOIR_LANES], wen[FOIR_LANES];
    ALIGN16 int8_t  blshifta[FOIR_LANES], blshiftb[FOIR_LANES];
    ALIGN16 int16_t fcol[3][FOIR_LANES];
    /* shape combiner: scratch arrays the operand table can point at, and the per-draw splatted constants */
    ALIGN16 int16_t shade[4][FOIR_LANES];
    ALIGN16 int16_t noise_s[FOIR_LANES];        /* noise input, already in the 9-bit extended form of an A/B/D operand */
    ALIGN16 int16_t c0a[FOIR_LANES];            /* cycle-0 alpha result, 9-bit sign extended (RGB multiply input COMBINED alpha) */
    ALIGN16 int16_t kc[SK_NKC][FOIR_LANES];     /* splatted constants (PRIM, ENV, K4, ...) in the extended form of their use */
};

/* The Z compare can run before texture / combine: it reads the batch's coverage, interpolated Z and the Z / colour buffers; the combiner rewrites the coverage
 * only for cvg*alpha and the Z compare rewrites it only in the interpenetrating mode, and the compare result (wen) is never an input of texture / combine. */
FOIR_INLINE int om_early_z_safe(const struct other_modes *om)
{
    return om->z_compare_en && !om->cvg_times_alpha && om->z_mode != ZMODE_INTERPENETRATING;
}

struct pipe_flags {
    uint8_t need_tex;
    uint8_t next_texel;        /* second (or only) combiner cycle reads TEXEL1: needs tex0n */
    uint8_t need_t0;
    uint8_t need_t1;           /* the second texture cycle (tile + 1) is read */
    uint8_t shade_rgb;
    uint8_t shade_a;
    uint8_t need_z;
    uint8_t need_cz;
    uint8_t rng;
    uint8_t early_z;           /* the Z compare may run before texture / combine and a batch with no live lane skip them (see batch_flush) */
};

/* Where each variable operand comes from, as byte offsets into the px_batch the kernel runs on (so the table is valid for any batch). */
struct sk_tab {
    uint16_t off[2][4][4];     /* [cycle][A, B, multiply, add][R, G, B, alpha] */
    uint8_t sh_rgb;
    uint8_t sh_a;
    uint8_t noise;
    uint8_t c0a;
    uint8_t nkc;
};

struct rdp;

struct foir_kernels {
    const char *name;
    void (*texture)(struct rdp *r, struct px_batch *b);
    void (*combine)(struct rdp *r, struct px_batch *b);
    void (*zcmp)(struct rdp *r, struct px_batch *b);
    void (*blend)(struct rdp *r, struct px_batch *b);
    void (*store)(struct rdp *r, struct px_batch *b);
    void (*fill)(struct rdp *r, int y, int x0, int x1);  /* inclusive x0..x1 */
    void (*copy)(struct rdp *r, int y, const struct span *sp, int flip);
};

extern const struct foir_kernels foir_kernels_scalar;
const struct foir_kernels *foir_kernels_neon(void);   /* NULL when not built */
void foir_neon_verify_report(char *buf, size_t n);
unsigned foir_kernels_neon_mask(void);

/* Every worker owns a context whose render state (rs_begin .. rs_end) is refreshed from the snapshots the parser takes when state changes, so the pixel stages read r->om, r->tile[] ... on any thread. */
#define FOIR_FB_HIST 16

struct rdp {
    uint8_t *rdram;
    uint32_t rdram_size;
    uint8_t *hidden;           /* per 16-bit RDRAM word: bit3 valid, bit2 lsb copy, bits1:0 hidden bits */
    uint8_t tmem[0x1000];

    uint32_t *dp_start, *dp_end, *dp_current, *dp_status;
    uint8_t *dmem;
    uint32_t *mi_intr;
    void (*check_interrupts)(void);

    uint32_t cmd_buf[CMD_MAX_WORDS];
    uint32_t cmd_pos, cmd_len;
    bool crashed;
    uint64_t frame_cmds;
    void (*trace)(void *ctx, const uint32_t *words, uint32_t nwords);
    void *trace_ctx;

    int ti_format, ti_size, ti_width;
    uint32_t ti_address;
    struct rectangle { uint16_t xl, yl, xh, yh; } clip;

    uint8_t rs_begin[0];
    struct other_modes om;
    struct combiner comb;
    struct tile tile[8];
    int scfield, sckeepodd;
    int fb_format, fb_size, fb_width;
    uint32_t fb_address, zb_address, fill_color;
    uint32_t primitive_z;
    uint16_t primitive_delta_z;
    struct color blend_color, fog_color, prim_color, env_color;
    struct color key_scale, key_center, key_width;
    int32_t primitive_lod_frac, min_level;
    int32_t k0_tf, k1_tf, k2_tf, k3_tf, k4, k5;
    struct pipe_flags pf;
    uint8_t rs_end[0];

    bool state_dirty;
    bool rs_dirty;
    bool tmem_dirty;

    struct prim_ctx pc;
    uint8_t cvgbuf[1024 + 32];

    struct foir_config cfg;
    const struct foir_kernels *k;
    struct foir_kernels ktab;
    struct foir_kernels kbase;
    unsigned spec_ok_mask;
    unsigned spec_mask;
    bool spec_dirty;
    int spec_tile;
    void (*spec_cur[5])(struct rdp *, struct px_batch *);
    bool spec_comb_shape;
    bool spec_tex_any;
    struct sk_tab sk;
#ifdef FOIR_STAGETIME
    uint64_t st_tk[8], st_n[8];
    uint64_t st_cpix[3];
#endif
    uint64_t cov_pix[COV_N];       /* [0..4] pixels in a specialised variant per stage, [5] all, [6] combine pixels in a shape kernel, [7] texture pixels in an "any format" variant */
    uint64_t cov_tex_all;
    bool skip_pixels;
    unsigned skip_frames, stat_skipped;   /* frames counted for the FrameSkip pattern, and skipped ones */
    int skip_level;                 /* FrameSkip=auto: 0 none, 1 skip every other frame, 2 render one in three */
    /* FrameSkip: the colour images drawn lately and whether their last drawing was skipped, so that the VI does not show
     * a buffer that still holds a frame from before skipping began */
    struct fb_hist_ent {
        uint32_t addr;
        bool skipped;               /* the last frame that drew into it was a skipped one */
        int width, size;
        uint32_t drawn, skip_tri, skip_fill, skip_copy;   /* primitives since the last VI scan-out (FOIR_SKIPLOG) */
        uint32_t period;            /* the VI period of its last drawing */
    } fb_hist[FOIR_FB_HIST];
    int fb_hist_last, fb_hist_next;
    uint32_t scan_addr[8];          /* VI origins seen lately: a colour image holding one is scanned out, anything else is an off-screen target */
    int scan_next;
    double sync_wait_ms;
    void (*frame_end_cb)(void *ctx);
    void *frame_end_ctx;
    void (*frame_start_cb)(void *ctx);   /* the first primitive of a frame into a scanned-out buffer (FrameSkip decides here) */
    uint32_t good_addr;             /* the scanned-out colour image of the last frame that was rendered, or 0xffffffff */
    uint32_t stat_repairs;          /* buffers refilled from it (FOIR_SKIPLOG) */
    bool skip_eligible;             /* the previous frame into this buffer was heavy enough to be worth skipping */
    uint32_t period;                /* VI periods (scan-outs that were followed by drawing into a scanned-out buffer) so far */
    uint32_t period_prims, last_rendered;   /* primitives into scanned-out buffers this period; the latest such buffer drawn while rendering */
    bool period_rendered;
    /* buffers drawn in fields (SET_SCISSOR field bit), for InterlaceHack: the parity of the rows drawn last and the width the buffer was drawn with */
    struct fld_buf { uint32_t addr; int width, rowbytes; int odd; } fld[4];
    int fld_n, fld_next;
    bool fld_dirty;                 /* the colour image or the scissor changed since fld was last updated */
    bool vi_since;                  /* the VI has scanned out since that frame began */
    struct px_batch batch;
    uint32_t stat_prims, stat_pixels;
#ifdef FOIR_FRAMELOG
    uint32_t stat_loads;
#endif
    uint32_t frame_seed;
    struct rdp_mt *mt;
    int worker_id, nworkers;
};

#define RSTATE_BYTES ((size_t)(offsetof(struct rdp, rs_end) - offsetof(struct rdp, rs_begin)))

bool rdp_start_workers(struct rdp *r, int nworkers);
void rdp_sync(struct rdp *r);
uint32_t rdp_pixels(struct rdp *r);
/* pix[0..4] = pixels in a specialised texture / combine / zcmp / blend / store variant, pix[5] = all pixels; *tex_all = pixels that needed the texture stage */
void rdp_coverage(struct rdp *r, uint64_t pix[COV_N], uint64_t *tex_all);

void rdp_init(struct rdp *r, const struct foir_config *cfg);
void rdp_attach_memory(struct rdp *r, uint8_t *rdram, uint32_t size);
void rdp_destroy(struct rdp *r);

void rdp_process_list(struct rdp *r);
void rdp_run_command(struct rdp *r, const uint32_t *cmd);
uint32_t rdp_command_words(uint32_t first_word);
void rdp_update_derived(struct rdp *r);

void setup_triangle(struct rdp *r, const uint32_t *cmd);
void setup_texrect(struct rdp *r, const uint32_t *cmd, bool flip);
void setup_fillrect(struct rdp *r, const uint32_t *cmd);
/* false when no scanline is visible; otherwise rows start..end (inclusive, <= 1023) are the ones to render */
bool setup_walk(struct rdp *r, const int32_t *ew, struct prim_walk *pw, int *start, int *end);

#ifdef FOIR_OVERDRAW
void od_reset(struct rdp *r, uint32_t vi_origin);
void od_stages(struct rdp *r, struct px_batch *b);
void od_fill(struct rdp *r, int y, int x0, int x1);
void od_copy(struct rdp *r, int y, const struct span *sp);
#endif
void foir_stats_batch(const struct rdp *r, int npix);
void foir_stats_flush(void);   /* write the statistics to $FOIR_STATE_STATS_FILE (if set) */

void raster_draw(struct rdp *r, const struct prim_walk *pw, int start, int end);
void rdp_fb_history_reset(struct rdp *r);
void rdp_skip_decide(struct rdp *r);                            /* a frame starts: skip it (r->skip_pixels) according to FrameSkip? */
void rdp_fld_update(struct rdp *r);                            /* record / forget the current colour image as field-rendered */
void rdp_fld_note(struct rdp *r, uint32_t addr, int width, int rowbytes, int odd);
const struct fld_buf *rdp_fld_find(const struct rdp *r, uint32_t origin, int *row_off);   /* the field-rendered buffer a VI origin points into, with the row offset */
void rdp_vi_scanned(struct rdp *r);                            /* a scan-out happened (the next frame into the same buffer is a new one) */
void rdp_note_scanout(struct rdp *r, uint32_t vi_origin);      /* the VI scanned out this origin */
void rdp_fb_skip_log(struct rdp *r, uint32_t vi_origin, bool stale);   /* FOIR_SKIPLOG: one line per colour image, then the counters restart */
bool rdp_fb_skipped(const struct rdp *r, uint32_t vi_origin);   /* the scanned-out buffer's last drawing was skipped */
void raster_render_walk(struct rdp *w, const struct prim_walk *pw, int start, int end);
void raster_render_job(struct rdp *w, const struct span *rows, int jlo, int start, int end);
void raster_render_job_ref(struct rdp *w, const struct span *rows, int jlo, int start, int end);
void raster_init_tables(void);
void kernels_scalar_init_tables(void);

void tmem_load_block(struct rdp *r, const uint32_t *cmd);
void tmem_load_tile(struct rdp *r, const uint32_t *cmd);
void tmem_load_tlut(struct rdp *r, const uint32_t *cmd);
void tile_update_derived(struct tile *t);
void tmem_fetch_texel(const struct rdp *r, const struct tile *t, int s, int tt, struct color *c);
void tmem_fetch_tlut(const struct rdp *r, uint32_t idx, struct color *c);
uint16_t tmem_tlut_raw(const struct rdp *r, uint32_t idx);
uint16_t tmem_raw16(const struct rdp *r, const struct tile *t, int s, int tt);

#include "rdram.h"

#endif
