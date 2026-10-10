/*
 * mupen64plus-rdp-foir - rdp.h
 * RDP state, command ids and the interfaces between the parser, primitive
 * setup, rasteriser and the pixel kernels.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FOIR_RDP_H
#define FOIR_RDP_H

#include "foir.h"
#include "config.h"

/* ---- RDP constants ---------------------------------------------------- */

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

/* DP status bits */
#define DP_STATUS_XBUS_DMA  0x001

/* MI interrupt bit */
#define MI_INTR_DP          0x20

/* command ids */
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

/* ---- small helper macros ---------------------------------------------- */

#define SIGN(x, n)   ((int32_t)(((uint32_t)(x) & ((1u << (n)) - 1)) | -((uint32_t)(x) & (1u << ((n) - 1)))))
#define SIGN16(x)    ((int16_t)(x))
#define TRELATIVE(x, y) ((x) - ((y) << 3))

/* compress a delta-Z value to its 4-bit exponent form */
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

/* ---- state ------------------------------------------------------------ */

struct color { int32_t r, g, b, a; };

#define COV_N 8      /* entries of the coverage counters (see struct rdp::cov_pix) */

struct tile {
    int format, size, line, tmem, palette;
    int ct, mt, cs, ms;
    int mask_t, shift_t, mask_s, shift_s;
    uint16_t sl, tl, sh, th;
    /* derived (see tile_update_derived) */
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
    /* derived */
    int rgb_alpha_dither;
    int dolod;            /* LOD calculation needed */
    int uses_texel0, uses_texel1; /* combiner references texels */
    int uses_noise;       /* combiner references the noise input */
    int partialreject[2];
    int real_blendshifts; /* blender2b == memory alpha in the active cycle */
};

/* combiner register file layout; each colour group is r,g,b,a */
enum {
    REG_COMB = 0, REG_TEX0 = 4, REG_TEX1 = 8, REG_PRIM = 12, REG_SHADE = 16, REG_ENV = 20,
    REG_ZERO = 24, REG_ONE = 25, REG_NOISE = 26, REG_K4 = 27, REG_K5 = 28,
    REG_KEYC = 29,   /* r,g,b */
    REG_KEYS = 32,   /* r,g,b */
    REG_LODF = 35, REG_PLODF = 36,
    REG_COUNT = 40
};

struct combiner {
    uint8_t rgb[2][4][3];  /* [cycle][suba, subb, mul, add][channel] -> reg index */
    uint8_t alpha[2][4];   /* [cycle][suba, subb, mul, add] -> reg index */
    int raw[16];           /* raw set_combine fields, for debugging */
};

/* one scanline of edge-walker output (walk_row, walk.h) */
struct span {
    int lx, rx, unscrx, validline;
    union {
        struct { int32_t r, g, b, a, s, t, w, z; };
        int32_t att[8];                  /* the same, in this order */
    };
    int32_t majorx[4], minorx[4], invalyscan[4];
};

/* The setup record of a primitive: everything the edge walker needs to produce the span of any one scanline, so
 * that the rows can be computed by whichever thread renders them (setup.c fills it, walk.h consumes it).
 *
 * Scanline j covers the subscanlines k = 4j .. 4j+3. The edge positions of subscanline k are linear in k (wrapping
 * 32-bit arithmetic, as in the sequential walker): xright = xr0 + (k - ycur) * xri; xleft = xm0 + (k - ycur) * xmi
 * below the middle vertex (k < ysw) and xl0 + (k - ysw) * xli from there on. The interpolants of scanline j are
 * a0 + (j - ycur / 4) * de. All arrays are in the order r g b a s t w z. */
#define ALIGN16 __attribute__((aligned(16)))

struct prim_walk {
    int32_t ycur;                  /* first subscanline of the sequential walk (multiple of 4) */
    int32_t ysw;                   /* subscanline at which the left edge switches to xl (INT32_MAX: never) */
    int32_t yhlimit, yllimit;      /* clipped top / bottom subscanlines (rows outside are invalid) */
    int32_t clipxl2, clipxh2;      /* scissor, in the walker's x units (<< 1) */
    uint32_t xr0, xri;             /* right (major, xh) edge */
    uint32_t xm0, xmi;             /* left edge above the middle vertex */
    uint32_t xl0, xli;             /* left edge from the middle vertex on */
    int32_t flip, ldflag;          /* ldflag: subscanline (0 or 3) whose x position adjusts the interpolants */
    int32_t scfield, sckeepodd;    /* interlace field selection (valid rows) */
    ALIGN16 uint32_t a0[8];        /* interpolants at row ycur / 4 */
    ALIGN16 uint32_t de[8];        /* per-row step */
    ALIGN16 uint32_t diff[8];      /* adjustment added to the masked interpolants */
    ALIGN16 uint32_t dxh[8];       /* per-fraction x correction */
};

/* Pixels per batch handed to the kernels. A batch collects pixels of one
 * primitive, possibly from several consecutive scanlines. */
#ifndef FOIR_BATCH
#define FOIR_BATCH 64
#endif
#define FOIR_LANES (FOIR_BATCH + 16)  /* arrays carry slack so vector stores may overrun */
#define SK_NKC 24                     /* splatted constant arrays of the shape combiner (per draw) */

/* channel order of the [4][lanes] colour arrays */
enum { CH_R = 0, CH_G = 1, CH_B = 2, CH_A = 3 };

/* Per-primitive interpolation constants (valid while a primitive is drawn) */
struct prim_ctx {
    int flip;
    int tilenum;
    int max_level;
    /* per-pixel increments (x ascending); the two groups of four are loaded as vectors */
    ALIGN16 int32_t dr;
    int32_t dg, db, da, ds, dt, dw, dz;
    /* the same in the hardware iteration direction (flip ? +d : -d): "next pixel" of the RDP is at coordinate + dxinc */
    ALIGN16 int32_t drinc;
    int32_t dginc, dbinc, dainc, dsinc, dtinc, dwinc, dzinc;
    int32_t drdy, dgdy, dbdy, dady, dzdy;     /* y gradients (shade correction) */
    int32_t cdr, cdg, cdb, cda, cdz;          /* x gradient (shade correction) */
    int32_t dsdy, dtdy, dwdy;
    int32_t dzpix;
    int32_t dzpixenc;
    uint32_t seed;                 /* per-primitive seed for the pseudo random pixel noise */
};

/* A batch of pixels from one primitive handed to the kernels. Lanes are in
 * ascending memory order within each scanline run. All stage arrays are
 * struct-of-arrays so NEON kernels can load 8 lanes at a time. */
struct px_batch {
    int n;                      /* number of pixels */
    int all_full;               /* every lane has full (8/8) coverage */
    int fbrange;                /* fbmin / fbmax are valid (set by the batch builder; else the memory kernels scan fbidx) */
    uint32_t fbmin, fbmax;      /* lowest / highest value in fbidx[0 .. n) */
    /* geometry */
    ALIGN16 uint32_t fbidx[FOIR_LANES];   /* fb_width * y + x */
    ALIGN16 uint8_t  ditidx[FOIR_LANES];  /* dither matrix index ((y >> scfield) & 3) * 4 + (x & 3) */
    ALIGN16 int16_t  xpos[FOIR_LANES];    /* x coordinate of the pixel (row-local; vector stores must not touch other rows) */
    /* raw interpolants */
    ALIGN16 int32_t s[FOIR_LANES], t[FOIR_LANES], w[FOIR_LANES];
    ALIGN16 int32_t cr[FOIR_LANES], cg[FOIR_LANES], cb[FOIR_LANES], ca[FOIR_LANES], cz[FOIR_LANES];
    /* coverage */
    ALIGN16 uint8_t cvg[FOIR_LANES], cvbit[FOIR_LANES], offx[FOIR_LANES], offy[FOIR_LANES];
    /* texture stage outputs. tex0n is the colour of TEXEL0 at the "next" pixel (+d*inc); only
     * produced for two-cycle combiners whose second cycle reads TEXEL1. */
    ALIGN16 int16_t tex0[4][FOIR_LANES], tex1[4][FOIR_LANES], tex0n[4][FOIR_LANES];
    ALIGN16 int16_t lodfrac[FOIR_LANES];
    /* combiner stage */
    ALIGN16 int16_t pixel[4][FOIR_LANES];       /* final combiner colour (0..255, alpha 0..256) */
    ALIGN16 int16_t shade_alpha[FOIR_LANES];    /* blender shade alpha */
    ALIGN16 int16_t acalpha[FOIR_LANES];        /* alpha used for alpha-compare */
    ALIGN16 int16_t cdith[FOIR_LANES];
    ALIGN16 int8_t  adith[FOIR_LANES];
    /* z / memory stage */
    ALIGN16 int32_t sz[FOIR_LANES];
    ALIGN16 int16_t mem[4][FOIR_LANES];
    ALIGN16 uint8_t memcvg[FOIR_LANES];
    ALIGN16 uint8_t blend_en[FOIR_LANES], prewrap[FOIR_LANES], wen[FOIR_LANES];
    ALIGN16 int8_t  blshifta[FOIR_LANES], blshiftb[FOIR_LANES];
    /* blender output */
    ALIGN16 int16_t fcol[3][FOIR_LANES];
    /* shape-specialised combiner (spec.h, kspec_shape.inc): scratch arrays the operand table can point at, and the
     * per-draw splatted constants (written by foir_spec_select, constant while a state is in use) */
    ALIGN16 int16_t shade[4][FOIR_LANES];       /* shade colour (the channels the combiner reads) */
    ALIGN16 int16_t noise_s[FOIR_LANES];        /* noise input, already in the 9-bit extended form of an A/B/D operand */
    ALIGN16 int16_t c0a[FOIR_LANES];            /* cycle-0 alpha result, 9-bit sign extended (RGB multiply input COMBINED alpha) */
    ALIGN16 int16_t kc[SK_NKC][FOIR_LANES];     /* splatted constants (PRIM, ENV, K4, ...) in the extended form of their use */
};

/* Flags derived from other_modes + combiner whenever the state changes */
struct pipe_flags {
    uint8_t need_tex;          /* texture stage has to run (texels / lod referenced) */
    uint8_t next_texel;        /* second (or only) combiner cycle reads TEXEL1: needs tex0n */
    uint8_t need_t0;           /* TEXEL0 of the first texture cycle is read */
    uint8_t need_t1;           /* the second texture cycle (tile + 1) is read: two-cycle, TEXEL1 in cycle 0 or TEXEL0 in cycle 1 */
    uint8_t shade_rgb;         /* combiner reads SHADE rgb */
    uint8_t shade_a;           /* combiner or blender reads shade alpha */
    uint8_t need_z;            /* Z buffer read and/or written */
    uint8_t need_cz;           /* Z interpolant needed */
    uint8_t rng;               /* some stage draws random numbers (noise / dither / alpha dither) */
};

/* Operand table of the shape-specialised combiner kernels (spec.h): where each variable operand of the current
 * state comes from. Offsets are bytes from the start of the px_batch the kernel is run on (per-pixel arrays and
 * the splatted constants live in the batch, so the table needs no pointers and is valid for any batch). */
struct sk_tab {
    uint16_t off[2][4][4];     /* [cycle][A, B, multiply, add][R, G, B, alpha]: array of a variable (V) operand */
    uint8_t sh_rgb;            /* shade colour is computed and stored (b->shade[0..2]) */
    uint8_t sh_a;              /* shade alpha is needed (b->shade[3], blender / combiner) */
    uint8_t noise;             /* the noise input is computed and stored (b->noise_s) */
    uint8_t c0a;               /* the cycle-0 alpha result is stored in b->c0a */
    uint8_t nkc;               /* constant arrays in use */
};

struct rdp;

/* Pixel kernels. A kernel set is a table of function pointers operating on
 * px_batch; the scalar C11 set is the reference. The NEON set replaces entries, and the per-state
 * specialised variants (spec.h) replace NEON entries for the state shapes they were compiled for. */
struct foir_kernels {
    const char *name;
    void (*texture)(struct rdp *r, struct px_batch *b);  /* tex0/tex1/lodfrac from s,t,w */
    void (*combine)(struct rdp *r, struct px_batch *b);  /* shade correction + colour combiner -> pixel */
    void (*zcmp)(struct rdp *r, struct px_batch *b);     /* fb read, Z compare -> wen, blend_en */
    void (*blend)(struct rdp *r, struct px_batch *b);    /* alpha compare + blender + dither -> fcol */
    void (*store)(struct rdp *r, struct px_batch *b);    /* FB / Z writeback */
    void (*fill)(struct rdp *r, int y, int x0, int x1);  /* FILL cycle span, inclusive x0..x1 (x0<=x1) */
    void (*copy)(struct rdp *r, int y, const struct span *sp, int flip); /* COPY cycle span */
};

extern const struct foir_kernels foir_kernels_scalar;
const struct foir_kernels *foir_kernels_neon(void);   /* NULL when not built */
void foir_neon_verify_report(char *buf, size_t n);    /* FOIR_NEON_VERIFY=1 summary for the log (empty otherwise) */
unsigned foir_kernels_neon_mask(void);                /* bit per stage (texture, combine, zcmp, blend, store) taken from the NEON set */

/* One rendering context. The parser owns one; every worker thread owns another
 * whose "render state" region (rs_begin .. rs_end) is refreshed from snapshots
 * the parser takes when state changes, so the pixel stages can keep reading
 * r->om, r->tile[] ... regardless of which thread they run on. */
struct rdp {
    /* --- memory (shared by all contexts) --- */
    uint8_t *rdram;
    uint32_t rdram_size;
    uint8_t *hidden;           /* per 16-bit RDRAM word: bit3 valid, bit2 lsb copy, bits1:0 hidden bits */
    uint8_t tmem[0x1000];      /* big-endian byte layout */

    /* --- host interface (DP command registers etc.), parser only --- */
    uint32_t *dp_start, *dp_end, *dp_current, *dp_status;
    uint8_t *dmem;
    uint32_t *mi_intr;
    void (*check_interrupts)(void);

    /* --- command assembly, parser only --- */
    uint32_t cmd_buf[CMD_MAX_WORDS];
    uint32_t cmd_pos, cmd_len;
    bool crashed;
    uint64_t frame_cmds;
    void (*trace)(void *ctx, const uint32_t *words, uint32_t nwords); /* optional command tap */
    void *trace_ctx;

    /* --- texture image + TMEM load state, parser only --- */
    int ti_format, ti_size, ti_width;
    uint32_t ti_address;
    struct rectangle { uint16_t xl, yl, xh, yh; } clip;   /* scissor used by setup */

    /* ===== render state: copied to workers as one block ===== */
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
    struct pipe_flags pf;      /* derived per-state flags for the kernels */
    uint8_t rs_end[0];

    bool state_dirty;          /* other_modes/combine derivations need refresh */
    bool rs_dirty;             /* render state changed since the last worker snapshot */
    bool tmem_dirty;           /* TMEM changed since the last worker snapshot */

    /* --- primitive / span state --- */
    struct prim_ctx pc;
    uint8_t cvgbuf[1024 + 32];

    /* --- misc --- */
    struct foir_config cfg;
    const struct foir_kernels *k;
    struct foir_kernels ktab;      /* the kernel table in use: kbase with the specialised variants of the current state */
    struct foir_kernels kbase;     /* the generic kernels selected by the config (NEON / scalar, knobs) */
    unsigned spec_ok_mask;         /* stages (SPEC_BIT) whose generic kernel is the NEON one, i.e. may be replaced by a variant */
    unsigned spec_mask;            /* stages currently running a specialised variant */
    bool spec_dirty;               /* render state or sampled tile changed since the last variant selection */
    int spec_tile;                 /* tile number the selection was made for */
    void (*spec_cur[5])(struct rdp *, struct px_batch *);   /* the selected variants (also in verify mode) */
    bool spec_comb_shape;          /* the selected combine kernel is a shape kernel (operand table r->sk) */
    bool spec_tex_any;             /* the selected texture variant is an "any format" one */
    struct sk_tab sk;              /* its operand table */
#ifdef FOIR_STAGETIME
    uint64_t st_tk[8], st_n[8];    /* ticks / calls per stage (0..4), combine by kind: [5] exact, [6] shape, [7] generic */
    uint64_t st_cpix[3];           /* combine pixels by kind */
#endif
    uint64_t cov_pix[COV_N];       /* pixels per batch stage that ran: [0..4] in a specialised variant, [5] all pixels,
                                    * [6] combine pixels in a shape kernel (a subset of [1]), [7] texture pixels in an
                                    * "any format" variant (a subset of [0]) */
    uint64_t cov_tex_all;          /* pixels that went through the texture stage */
    bool skip_pixels;              /* frame skipping: parse and set up but do not render */
    double sync_wait_ms;           /* time the parser spent waiting for the workers at SYNC_FULL */
    void (*frame_end_cb)(void *ctx);   /* called at every SYNC_FULL (parser thread) */
    void *frame_end_ctx;
    struct px_batch batch;
    uint32_t stat_prims, stat_pixels;
#ifdef FOIR_FRAMELOG
    uint32_t stat_loads;           /* LOAD_BLOCK / LOAD_TILE / LOAD_TLUT commands */
#endif
    uint32_t frame_seed;
    struct rdp_mt *mt;             /* worker pool (NULL when single threaded) */
    int worker_id, nworkers;       /* row ownership when this context is a worker */
};

#define RSTATE_BYTES ((size_t)(offsetof(struct rdp, rs_end) - offsetof(struct rdp, rs_begin)))

/* worker threads (mt.c); nworkers <= 0 keeps everything on the calling thread */
bool rdp_start_workers(struct rdp *r, int nworkers);
void rdp_sync(struct rdp *r);                          /* wait until all queued primitives are rendered */
uint32_t rdp_pixels(struct rdp *r);                    /* pixels rendered so far (all threads) */
/* specialisation coverage so far (all threads): pix[0..4] = pixels in a specialised texture / combine / zcmp / blend /
 * store variant, pix[5] = all pixels; *tex_all = pixels that needed the texture stage */
void rdp_coverage(struct rdp *r, uint64_t pix[COV_N], uint64_t *tex_all);

/* rdp lifecycle */
void rdp_init(struct rdp *r, const struct foir_config *cfg);
void rdp_attach_memory(struct rdp *r, uint8_t *rdram, uint32_t size);
void rdp_destroy(struct rdp *r);

/* parser.c */
void rdp_process_list(struct rdp *r);                 /* reads the DP registers */
void rdp_run_command(struct rdp *r, const uint32_t *cmd); /* execute one decoded command */
uint32_t rdp_command_words(uint32_t first_word);    /* length in 32-bit words of a command */
void rdp_update_derived(struct rdp *r);

/* setup.c: primitive decode. Fills r->pc and the walker record, then calls raster_draw(). */
void setup_triangle(struct rdp *r, const uint32_t *cmd);
void setup_texrect(struct rdp *r, const uint32_t *cmd, bool flip);
void setup_fillrect(struct rdp *r, const uint32_t *cmd);
/* Decode the canonical 44-word edge record `ew` (words 0-7 edges, 8-23 shade, 24-39 texture, 40-43 depth): sets
 * r->pc and *pw; false when no scanline is visible. Otherwise rows start..end (inclusive, <= 1023) are the ones to render. */
bool setup_walk(struct rdp *r, const int32_t *ew, struct prim_walk *pw, int *start, int *end);

/* stats.c (debug statistics, -DFOIR_STATS) */
void foir_stats_batch(const struct rdp *r, int npix);
void foir_stats_flush(void);   /* write the statistics to $FOIR_STATE_STATS_FILE (if set) */

/* raster.c */
/* Draw scanlines start..end of the primitive described by r->pc and *pw (rows are computed on the fly, walk.h). */
void raster_draw(struct rdp *r, const struct prim_walk *pw, int start, int end);
/* Render the rows of a job on context w: rows y with (y / FOIR_STRIPE) % nworkers == worker_id. */
void raster_render_walk(struct rdp *w, const struct prim_walk *pw, int start, int end);
/* The same from ready-made spans (`rows[y - jlo]`), tests only: kfuzz drives the batch builder with them. */
void raster_render_job(struct rdp *w, const struct span *rows, int jlo, int start, int end);
void raster_render_job_ref(struct rdp *w, const struct span *rows, int jlo, int start, int end);   /* portable span builder (tests) */
void raster_init_tables(void);
void kernels_scalar_init_tables(void);

/* tmem.c */
void tmem_load_block(struct rdp *r, const uint32_t *cmd);
void tmem_load_tile(struct rdp *r, const uint32_t *cmd);
void tmem_load_tlut(struct rdp *r, const uint32_t *cmd);
void tile_update_derived(struct tile *t);
void tmem_fetch_texel(const struct rdp *r, const struct tile *t, int s, int tt, struct color *c);
void tmem_fetch_tlut(const struct rdp *r, uint32_t idx, struct color *c);
uint16_t tmem_tlut_raw(const struct rdp *r, uint32_t idx);
uint16_t tmem_raw16(const struct rdp *r, const struct tile *t, int s, int tt);

/* rdram.c helpers (inline in rdram.h) */
#include "rdram.h"

#endif
