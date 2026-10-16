/*
 * mupen64plus-rdp-foir - plugin.c
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#define M64P_PLUGIN_PROTOTYPES 1
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#include "m64p_types.h"
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_plugin.h"

#include "rdp.h"
#include "spec.h"
#include "vi.h"
#include "gl_out.h"
#include "capture.h"
#include "foir_suxty.h"
#ifdef FOIR_FRAMELOG
#include "framelog.h"
#include "mt.h"
#endif
#include <pthread.h>

#define PLUGIN_API_VERSION 0x020500

static ptr_ConfigOpenSection ConfigOpenSection;
static ptr_ConfigSaveSection ConfigSaveSection;
static ptr_ConfigSetParameter ConfigSetParameter;
static ptr_ConfigGetParameterType ConfigGetParameterType;
static ptr_ConfigSetDefaultInt ConfigSetDefaultInt;
static ptr_ConfigSetDefaultBool ConfigSetDefaultBool;
static ptr_ConfigSetDefaultString ConfigSetDefaultString;
static ptr_ConfigGetParamInt ConfigGetParamInt;
static ptr_ConfigGetParamBool ConfigGetParamBool;
static ptr_ConfigGetParamString ConfigGetParamString;
static ptr_PluginGetVersion CoreGetVersion;

static void *l_core;
static bool l_initialized, l_rom_open, l_warned_hle;
static pthread_t l_emu_thread;
static GFX_INFO l_gfx;
static void (*l_render_cb)(int);
static m64p_handle l_cfg_general, l_cfg_foir;
static struct foir_config l_cfg;
static struct rdp l_rdp;
static struct vi_frame l_frame;
static int l_win_w = 640, l_win_h = 480;
static bool l_fullscreen;
static double l_rdp_ms, l_vi_ms, l_sync_ms, l_last_log_ms, l_present_ms, l_syncfull_base;
static unsigned l_stale_frames;
static bool l_skiplog;                   /* FOIR_SKIPLOG */
static bool l_framehash;                 /* FOIR_FRAMEHASH: a hash of every frame as it is presented (tests) */
static double l_win_start_ms, l_last_us_ms;
static unsigned l_win_frames;
static unsigned l_frames, l_lists;
static uint64_t l_cmds_base;
static uint32_t l_prims_base, l_pix_base;
static uint64_t l_cov_base[COV_N], l_covtex_base;
static double l_cpu_all_base, l_cpu_emu_base;

#ifdef FOIR_FRAMELOG
static bool l_fl_on;
static double l_fl_cb_ms, l_fl_cbmax_ms, l_fl_syncfull_base;
static unsigned l_fl_lists, l_fl_loads_base;
static uint64_t l_fl_cmds_base, l_fl_jobs_base, l_fl_mtsync_base, l_fl_help_base, l_fl_busy_base[8];
static uint32_t l_fl_prims_base, l_fl_pix_base;
static uint64_t l_fl_cpu;
static struct rusage l_fl_ru;
#endif

static double cpu_ms(int who)
{
    struct rusage ru;
    if (getrusage(who, &ru) != 0) return 0;
    return (double)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e3 + (double)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e3;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static void (*l_debug_cb)(void *, int, const char *);
static void *l_debug_ctx;

/* the legacy mode decides at every SYNC_FULL, the default at the first primitive of a frame into a scanned-out buffer (raster.c) */
static void on_rdp_frame_end(void *ctx)
{
    (void)ctx;
    if (l_cfg.skip_legacy) rdp_skip_decide(&l_rdp);
}

static void on_rdp_frame_start(void *ctx)
{
    (void)ctx;
    rdp_skip_decide(&l_rdp);
}

static void plugin_log(void *ctx, int level, const char *msg)
{
    (void)ctx;
    if (l_debug_cb) l_debug_cb(l_debug_ctx, level, msg);
}

static void read_vi(struct vi_regs *v)
{
    v->status = *l_gfx.VI_STATUS_REG;
    v->origin = *l_gfx.VI_ORIGIN_REG;
    v->width = *l_gfx.VI_WIDTH_REG;
    v->h_start = *l_gfx.VI_H_START_REG;
    v->v_start = *l_gfx.VI_V_START_REG;
    v->x_scale = *l_gfx.VI_X_SCALE_REG;
    v->y_scale = *l_gfx.VI_Y_SCALE_REG;
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *context, void (*debug_cb)(void *, int, const char *))
{
    if (l_initialized) return M64ERR_ALREADY_INIT;
    l_debug_cb = debug_cb;
    l_debug_ctx = context;
    foir_log_set_callback(plugin_log, NULL);
    l_core = core;

#define SYM(var, type, name) var = (type)dlsym(core, name)
    SYM(ConfigOpenSection, ptr_ConfigOpenSection, "ConfigOpenSection");
    SYM(ConfigSaveSection, ptr_ConfigSaveSection, "ConfigSaveSection");
    SYM(ConfigSetParameter, ptr_ConfigSetParameter, "ConfigSetParameter");
    SYM(ConfigGetParameterType, ptr_ConfigGetParameterType, "ConfigGetParameterType");
    SYM(ConfigSetDefaultInt, ptr_ConfigSetDefaultInt, "ConfigSetDefaultInt");
    SYM(ConfigSetDefaultBool, ptr_ConfigSetDefaultBool, "ConfigSetDefaultBool");
    SYM(ConfigSetDefaultString, ptr_ConfigSetDefaultString, "ConfigSetDefaultString");
    SYM(ConfigGetParamInt, ptr_ConfigGetParamInt, "ConfigGetParamInt");
    SYM(ConfigGetParamBool, ptr_ConfigGetParamBool, "ConfigGetParamBool");
    SYM(ConfigGetParamString, ptr_ConfigGetParamString, "ConfigGetParamString");
    SYM(CoreGetVersion, ptr_PluginGetVersion, "PluginGetVersion");
#undef SYM
    if (!ConfigOpenSection || !ConfigSetDefaultInt || !ConfigGetParamInt) return M64ERR_INCOMPATIBLE;

    foir_config_defaults(&l_cfg);

    ConfigOpenSection("Video-General", &l_cfg_general);
    ConfigSetDefaultBool(l_cfg_general, "Fullscreen", 0, "Use fullscreen mode if True, or windowed mode if False");
    ConfigSetDefaultInt(l_cfg_general, "ScreenWidth", 640, "Width of output window or fullscreen width");
    ConfigSetDefaultInt(l_cfg_general, "ScreenHeight", 480, "Height of output window or fullscreen height");

    ConfigOpenSection("Video-Foir", &l_cfg_foir);
    ConfigSetDefaultBool(l_cfg_foir, "FastCombiner", l_cfg.fast_combiner, "Fast paths for common combiner/blender modes (the NEON kernels always take them)");
    ConfigSetDefaultBool(l_cfg_foir, "RelaxedPrecision", l_cfg.relaxed_precision, "Forces lower accuracy depth values and calculations for speed");
    ConfigSetDefaultString(l_cfg_foir, "FrameSkip", "0", "0 = off, N = fixed frame skip, auto = skip rendering to hold full speed");
    ConfigSetDefaultBool(l_cfg_foir, "InterlaceHack", l_cfg.interlace_hack, "Render one field and line-double instead of full interlace logic");
    ConfigSetDefaultInt(l_cfg_foir, "Threads", l_cfg.threads, "Number of rendering worker threads: 0 = auto (cores - 1), -1 = render on the emulation thread");
    ConfigSetDefaultBool(l_cfg_foir, "SyncHelper", l_cfg.mt_helper, "The emulation thread renders for the workers while it waits for them (exact)");
    ConfigSetDefaultBool(l_cfg_foir, "Specialize", l_cfg.specialize, "Use the precompiled per-state pixel kernels; False = generic NEON kernels only (a stale JIT= entry in the config file is ignored)");
    ConfigSetDefaultBool(l_cfg_foir, "NEON", l_cfg.neon, "Use the NEON kernels when built in; False = scalar reference kernels");
    ConfigSetDefaultBool(l_cfg_foir, "VSync", l_cfg.vsync, "Wait for vertical sync when presenting");
    ConfigSetDefaultBool(l_cfg_foir, "Filter", l_cfg.filter, "Use bilinear filtering when scaling the VI image to the window");
    ConfigSetDefaultBool(l_cfg_foir, "Widescreen", l_cfg.widescreen, "Stretch the image to the whole window instead of keeping 4:3");
    ConfigSetDefaultBool(l_cfg_foir, "VIAntiAliasing", l_cfg.vi_aa, "Apply the VI's anti-aliasing when the game enables it; costs emulation-thread time");
    ConfigSetDefaultBool(l_cfg_foir, "VIDivot", l_cfg.vi_divot, "Apply the VI's divot filter when the game enables it; costs emulation-thread time");
    ConfigSetDefaultBool(l_cfg_foir, "VIDedither", l_cfg.vi_dedither, "Apply the VI's dither restore when the game enables it; costs emulation-thread time");
    ConfigSetDefaultBool(l_cfg_foir, "VIGamma", l_cfg.vi_gamma, "Apply the VI's gamma correction when the game enables it; costs emulation-thread time");
    ConfigSetDefaultBool(l_cfg_foir, "CropOverscanH", l_cfg.crop_h, "Crop 8 VI pixels off the left and right edges of the image (hides edge artefacts; display only)");
    ConfigSetDefaultBool(l_cfg_foir, "CropOverscanV", l_cfg.crop_v, "Crop 12 VI lines off the top and bottom edges of the image (hides edge artefacts; display only)");
    {   /* the single VIFilters key of the first version: if a config file still holds it set, all four new keys are turned on and it is cleared */
        m64p_type t;
        if (ConfigGetParameterType(l_cfg_foir, "VIFilters", &t) == M64ERR_SUCCESS && t == M64TYPE_BOOL) {
            int on = ConfigGetParamBool(l_cfg_foir, "VIFilters") != 0, off = 0;
            if (on) {
                static const char *const keys[] = { "VIAntiAliasing", "VIDivot", "VIDedither", "VIGamma" };
                for (unsigned i = 0; i < 4; i++) ConfigSetParameter(l_cfg_foir, keys[i], M64TYPE_BOOL, &on);
            }
            ConfigSetParameter(l_cfg_foir, "VIFilters", M64TYPE_BOOL, &off);
        }
    }
    ConfigSaveSection("Video-General");
    ConfigSaveSection("Video-Foir");

    l_initialized = true;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    if (!l_initialized) return M64ERR_NOT_INIT;
    foir_log_set_callback(NULL, NULL);
    l_debug_cb = NULL;
    l_initialized = false;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version, int *api, const char **name, int *caps)
{
    if (type) *type = M64PLUGIN_GFX;
    if (version) *version = FOIR_VERSION;
    if (api) *api = PLUGIN_API_VERSION;
    if (name) *name = FOIR_NAME;
    if (caps) *caps = 0;
    return M64ERR_SUCCESS;
}

EXPORT int CALL InitiateGFX(GFX_INFO info)
{
    l_gfx = info;
    return 1;
}

static void read_foir_section(struct foir_config *c)
{
    c->fast_combiner = ConfigGetParamBool(l_cfg_foir, "FastCombiner") != 0;
    c->relaxed_precision = ConfigGetParamBool(l_cfg_foir, "RelaxedPrecision") != 0;
    c->interlace_hack = ConfigGetParamBool(l_cfg_foir, "InterlaceHack") != 0;
    c->threads = ConfigGetParamInt(l_cfg_foir, "Threads");
    c->mt_helper = ConfigGetParamBool(l_cfg_foir, "SyncHelper") != 0;
    c->specialize = ConfigGetParamBool(l_cfg_foir, "Specialize") != 0;
    c->neon = ConfigGetParamBool(l_cfg_foir, "NEON") != 0;
    c->vsync = ConfigGetParamBool(l_cfg_foir, "VSync") != 0;
    c->filter = ConfigGetParamBool(l_cfg_foir, "Filter") != 0;
    c->widescreen = ConfigGetParamBool(l_cfg_foir, "Widescreen") != 0;
    c->vi_aa = ConfigGetParamBool(l_cfg_foir, "VIAntiAliasing") != 0;
    c->vi_divot = ConfigGetParamBool(l_cfg_foir, "VIDivot") != 0;
    c->vi_dedither = ConfigGetParamBool(l_cfg_foir, "VIDedither") != 0;
    c->vi_gamma = ConfigGetParamBool(l_cfg_foir, "VIGamma") != 0;
    c->crop_h = ConfigGetParamBool(l_cfg_foir, "CropOverscanH") != 0;
    c->crop_v = ConfigGetParamBool(l_cfg_foir, "CropOverscanV") != 0;
    const char *fs = ConfigGetParamString(l_cfg_foir, "FrameSkip");
    if (fs && !strcmp(fs, "auto")) c->frame_skip = FOIR_FRAMESKIP_AUTO;
    else c->frame_skip = fs ? atoi(fs) : 0;
}

static void suxty_apply_staged(struct foir_config *c);

static void load_config(void)
{
    l_fullscreen = ConfigGetParamBool(l_cfg_general, "Fullscreen") != 0;
    l_win_w = ConfigGetParamInt(l_cfg_general, "ScreenWidth");
    l_win_h = ConfigGetParamInt(l_cfg_general, "ScreenHeight");
    read_foir_section(&l_cfg);
    suxty_apply_staged(&l_cfg);
    const char *ne = getenv("FOIR_NEON");
    if (ne && *ne) l_cfg.neon = atoi(ne) != 0;
    const char *he = getenv("FOIR_HELPER");
    if (he && *he) l_cfg.mt_helper = atoi(he) != 0;
    const char *sl = getenv("FOIR_SKIPLOG");
    l_skiplog = sl && *sl && atoi(sl) != 0;
    l_cfg.skip_log = l_skiplog;
    const char *fe = getenv("FOIR_FRAMESKIP");   /* tests: overrides FrameSkip (N or auto) */
    if (fe && *fe) l_cfg.frame_skip = !strcmp(fe, "auto") ? FOIR_FRAMESKIP_AUTO : atoi(fe);
    const char *sm = getenv("FOIR_SKIP_MINPRIMS");
    if (sm && *sm) l_cfg.skip_min_prims = atoi(sm);
    const char *so = getenv("FOIR_SKIP_OPTS");
    if (so && *so) l_cfg.skip_opts = atoi(so);
    const char *fh = getenv("FOIR_FRAMEHASH");
    l_framehash = fh && *fh && atoi(fh) != 0;
    const char *sg = getenv("FOIR_SKIP_LEGACY");
    if (sg && *sg) l_cfg.skip_legacy = atoi(sg) != 0;
    const char *ke = getenv("FOIR_KNOBS");
    if (ke && !strcmp(ke, "exact")) {
        l_cfg.relaxed_precision = false; l_cfg.frame_skip = 0; l_cfg.interlace_hack = false;
    }
}

EXPORT int CALL RomOpen(void)
{
    if (!l_initialized) return 0;
    l_emu_thread = pthread_self();
    load_config();

    uint32_t rdram_size = 0x800000;
    int core_version = 0;
    if (CoreGetVersion) CoreGetVersion(NULL, &core_version, NULL, NULL, NULL);
    if (core_version >= 0x020501 && l_gfx.version >= 2 && l_gfx.RDRAM_SIZE && *l_gfx.RDRAM_SIZE)
        rdram_size = *l_gfx.RDRAM_SIZE;

    memset(l_cov_base, 0, sizeof(l_cov_base)); l_covtex_base = 0;
    l_cpu_all_base = cpu_ms(RUSAGE_SELF); l_cpu_emu_base = cpu_ms(RUSAGE_THREAD);
    rdp_init(&l_rdp, &l_cfg);
    rdp_attach_memory(&l_rdp, l_gfx.RDRAM, rdram_size);
    l_rdp.dp_start = l_gfx.DPC_START_REG;
    l_rdp.dp_end = l_gfx.DPC_END_REG;
    l_rdp.dp_current = l_gfx.DPC_CURRENT_REG;
    l_rdp.dp_status = l_gfx.DPC_STATUS_REG;
    l_rdp.dmem = l_gfx.DMEM;
    l_rdp.mi_intr = l_gfx.MI_INTR_REG;
    l_rdp.check_interrupts = l_gfx.CheckInterrupts;
    capture_init(&l_rdp);
#ifdef FOIR_FRAMELOG
    l_fl_on = framelog_open();
#endif
    l_rdp.frame_end_cb = on_rdp_frame_end;
    l_rdp.frame_start_cb = on_rdp_frame_start;
    l_rdp.skip_pixels = false;
    rdp_fb_history_reset(&l_rdp);
    l_stale_frames = 0;
    l_rdp.skip_frames = l_rdp.stat_skipped = 0;
    l_rdp.skip_level = 0;
    int nthreads = l_cfg.threads;
    const char *te = getenv("FOIR_THREADS");
    if (te && *te) nthreads = atoi(te);
    if (nthreads == 0) {
        long nc = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = nc > 2 ? (int)nc - 1 : (nc == 2 ? 1 : -1);
        if (nthreads > 7) nthreads = 7;
    }
    bool mt_on = nthreads > 0 && rdp_start_workers(&l_rdp, nthreads);
#ifdef FOIR_FRAMELOG
    if (l_fl_on) {
        mt_prof_enable(&l_rdp);
        getrusage(RUSAGE_THREAD, &l_fl_ru);
        { struct timespec tc; if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tc) == 0) l_fl_cpu = (uint64_t)tc.tv_sec * 1000000000ull + (uint64_t)tc.tv_nsec; }
        uint64_t jobs = 0, sy = 0, hp = 0;
        mt_prof_read(&l_rdp, l_fl_busy_base, &jobs, &sy, &hp);
        l_fl_jobs_base = jobs; l_fl_mtsync_base = sy; l_fl_help_base = hp;
        foir_log(FOIR_LOG_INFO, "FOIR_FRAMELOG: per-frame record to %s", getenv("FOIR_FRAMELOG"));
    }
#endif
    foir_log(FOIR_LOG_INFO, "foir M3: kernels=%s workers=%d rdram=%u KiB, specialised variants %s: texture %d, combine %d, zcmp %d, blend %d, store %d",
             l_rdp.k->name, mt_on ? nthreads : 0, rdram_size >> 10, l_rdp.spec_ok_mask && l_cfg.specialize ? "on" : "off",
             foir_spec_tabs[SPEC_TEX].n, foir_spec_tabs[SPEC_COMB].n, foir_spec_tabs[SPEC_ZCMP].n, foir_spec_tabs[SPEC_BLEND].n, foir_spec_tabs[SPEC_STORE].n);

    struct gl_out_params gp = { l_win_w, l_win_h, l_fullscreen, l_cfg.vsync, l_cfg.filter, l_cfg.widescreen };
    if (!gl_out_init(l_core, &gp)) return 0;
    l_rom_open = true;
    foir_log(FOIR_LOG_INFO, "video output ready (%dx%d, %s, vsync %d)", l_win_w, l_win_h, l_fullscreen ? "fullscreen" : "windowed", l_cfg.vsync);
    return 1;
}

EXPORT void CALL RomClosed(void)
{
    if (!l_rom_open) return;
    capture_shutdown(&l_rdp);
#ifdef FOIR_FRAMELOG
    framelog_close();
    l_fl_on = false;
#endif
    foir_stats_flush();
    gl_out_shutdown();
    rdp_destroy(&l_rdp);
    l_rom_open = false;
}

EXPORT void CALL ProcessDList(void)
{
    if (!l_warned_hle) {
        foir_log(FOIR_LOG_WARNING, "HLE display lists are not supported; use an LLE RSP (e.g. rsp-cxd4, or rsp-hle with DisplayListToGraphicsPlugin = False and a fallback)");
        l_warned_hle = true;
    }
}

EXPORT void CALL ProcessRDPList(void)
{
    if (!l_rom_open) return;
    double t0 = now_ms();
    rdp_process_list(&l_rdp);
    double dt = now_ms() - t0;
    l_rdp_ms += dt;
    l_lists++;
#ifdef FOIR_FRAMELOG
    if (l_fl_on) {
        l_fl_cb_ms += dt; l_fl_lists++;
        if (dt > l_fl_cbmax_ms) l_fl_cbmax_ms = dt;
    }
#endif
}

#ifdef FOIR_FRAMELOG
#define MS_TO_NS(x) ((uint64_t)((x) * 1e6 + 0.5))
static uint32_t sat32(uint64_t x) { return x > 0xffffffffull ? 0xffffffffu : (uint32_t)x; }

/* FOIR_FRAMELOG: one record per UpdateScreen; ts = entry, ts1 = after the drain, t0 / t1 / t2 = vi render start / end, present end */
static void fl_record(double ts, double ts1, double t0, double t1, double t2, const struct vi_regs *v)
{
    (void)v;
    struct fl_rec r;
    memset(&r, 0, sizeof r);
    uint64_t busy[8] = { 0 }, jobs = 0, sy = 0, hp = 0, bsum = 0, bmax = 0;
    int nw = mt_prof_read(&l_rdp, busy, &jobs, &sy, &hp);
    for (int i = 0; i < nw; i++) {
        uint64_t d = busy[i] - l_fl_busy_base[i];
        l_fl_busy_base[i] = busy[i];
        bsum += d;
        if (d > bmax) bmax = d;
    }
    uint32_t pix = rdp_pixels(&l_rdp);
    r.t_enter_ns = MS_TO_NS(ts); r.t_leave_ns = MS_TO_NS(t2);
    r.frame = l_frames + 1;
    r.lists = l_fl_lists;
    r.cb_ns = sat32(MS_TO_NS(l_fl_cb_ms)); r.cb_max_ns = sat32(MS_TO_NS(l_fl_cbmax_ms));
    r.syncfull_ns = sat32(MS_TO_NS(l_rdp.sync_wait_ms - l_fl_syncfull_base));
    r.mtsync_ns = sat32(sy - l_fl_mtsync_base);
    r.helper_ns = sat32(hp - l_fl_help_base);
    r.drain_ns = sat32(MS_TO_NS(ts1 - ts)); r.vi_ns = sat32(MS_TO_NS(t1 - t0)); r.present_ns = sat32(MS_TO_NS(t2 - t1));
    r.cmds = (uint32_t)(l_rdp.frame_cmds - l_fl_cmds_base);
    r.prims = l_rdp.stat_prims - l_fl_prims_base;
    r.pixels = pix - l_fl_pix_base;
    r.loads = l_rdp.stat_loads - l_fl_loads_base;
    r.wk_busy_sum_ns = sat32(bsum); r.wk_busy_max_ns = sat32(bmax);
    r.jobs = (uint32_t)(jobs - l_fl_jobs_base);
    r.vi_res = ((uint32_t)l_frame.width << 16) | ((uint32_t)l_frame.height & 0xffff);
    {
        struct rusage ru;
        struct timespec tc;
        if (getrusage(RUSAGE_THREAD, &ru) == 0 && clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tc) == 0) {
            /* getrusage's times are scheduler-tick granular (10 ms here); CLOCK_THREAD_CPUTIME_ID is exact */
            uint64_t cpu_now = (uint64_t)tc.tv_sec * 1000000000ull + (uint64_t)tc.tv_nsec;
            r.thr_cpu_ns = sat32(cpu_now - l_fl_cpu);
            l_fl_cpu = cpu_now;
            r.thr_faults = ((uint32_t)(ru.ru_minflt - l_fl_ru.ru_minflt) & 0xffff) | ((uint32_t)(ru.ru_majflt - l_fl_ru.ru_majflt) << 16);
            r.thr_ctxsw = ((uint32_t)(ru.ru_nvcsw - l_fl_ru.ru_nvcsw) & 0xffff) | ((uint32_t)(ru.ru_nivcsw - l_fl_ru.ru_nivcsw) << 16);
            l_fl_ru = ru;
        }
    }
    framelog_put(&r);
    l_fl_cb_ms = l_fl_cbmax_ms = 0; l_fl_lists = 0;
    l_fl_syncfull_base = l_rdp.sync_wait_ms; l_fl_mtsync_base = sy; l_fl_help_base = hp; l_fl_jobs_base = jobs;
    l_fl_cmds_base = l_rdp.frame_cmds; l_fl_prims_base = l_rdp.stat_prims; l_fl_pix_base = pix; l_fl_loads_base = l_rdp.stat_loads;
}
#endif

EXPORT void CALL UpdateScreen(void)
{
    if (!l_rom_open) return;
    struct vi_regs v;
    read_vi(&v);
    double ts = now_ms();
    rdp_sync(&l_rdp);
    double ts1 = now_ms();
    l_sync_ms += ts1 - ts;
    capture_frame(&l_rdp, &v);
    double t0 = now_ms();
    /* keep the last good frame on screen rather than a buffer FrameSkip left holding an older one */
    rdp_note_scanout(&l_rdp, v.origin);
    rdp_vi_scanned(&l_rdp);
    bool stale = rdp_fb_skipped(&l_rdp, v.origin);
    if (l_skiplog) rdp_fb_skip_log(&l_rdp, v.origin, stale);
    l_stale_frames += stale;
    if (!stale) vi_render_ex(&l_rdp, &v, &l_frame, true);
    double t1 = now_ms();
    if (!stale) gl_out_present(l_frame.valid ? l_frame.pixels : NULL, l_frame.raw16, l_frame.width, l_frame.height, l_frame.screen_w, l_frame.screen_h, l_render_cb);
    double t2 = now_ms();
    if (l_framehash) {
        uint32_t hsh = 2166136261u;
        for (int i = 0; l_frame.valid && i < l_frame.width * l_frame.height; i++) hsh = (hsh ^ l_frame.pixels[i]) * 16777619u;
        foir_log(FOIR_LOG_INFO, "framehash %u %08x %s status %05x scfield %d", l_frames + 1, hsh, stale ? "held" : "shown", v.status, l_rdp.scfield);
    }
    l_vi_ms += t1 - t0;
    l_present_ms += t2 - t1;
#ifdef FOIR_FRAMELOG
    if (l_fl_on) fl_record(ts, ts1, t0, t1, t2, &v);
#endif
    if (l_cfg.frame_skip == FOIR_FRAMESKIP_AUTO) {
        if (l_win_frames == 0) l_win_start_ms = t0;
        if (++l_win_frames >= 60) {
            double per = (t2 - l_win_start_ms) / l_win_frames;
            if (per > 16.67 * 1.12 && l_rdp.skip_level < 2) l_rdp.skip_level++;
            else if (per < 16.67 * 0.92 && l_rdp.skip_level > 0) l_rdp.skip_level--;
            l_win_frames = 0;
        }
    }
    (void)l_last_us_ms;
    capture_screenshot(&l_frame);
    ++l_frames;
    if (l_frames <= 3) foir_log(FOIR_LOG_INFO, "UpdateScreen #%u: vi %dx%d valid=%d type=%u origin=%08x", l_frames, l_frame.width, l_frame.height, l_frame.valid, v.status & 3, v.origin);
    if (l_frames % 120 == 0) {
        uint32_t pix = rdp_pixels(&l_rdp);
        double wall = now_ms();
        double per_frame = l_last_log_ms > 0 ? (wall - l_last_log_ms) / 120 : 0;
        l_last_log_ms = wall;
        double sfw = l_rdp.sync_wait_ms - l_syncfull_base;
        l_syncfull_base = l_rdp.sync_wait_ms;
        foir_log(FOIR_LOG_INFO, "frame %u: %.2f ms/frame wall, %.2f ms/frame RDP (%.2f parse+setup of which %.2f waiting at SYNC_FULL, %.2f drain; %u lists, %llu cmds, %u prims, %u px), %.2f ms/frame VI (%.2f convert + %.2f present), vi %dx%d type %u, skipped %u/%u level %d, held %u",
                 l_frames, per_frame, (l_rdp_ms + l_sync_ms) / 120, l_rdp_ms / 120, sfw / 120, l_sync_ms / 120, l_lists, (unsigned long long)(l_rdp.frame_cmds - l_cmds_base),
                 l_rdp.stat_prims - l_prims_base, pix - l_pix_base, (l_vi_ms + l_present_ms) / 120, l_vi_ms / 120, l_present_ms / 120,
                 l_frame.width, l_frame.height, v.status & 3, l_rdp.stat_skipped, l_rdp.skip_frames, l_rdp.skip_level, l_stale_frames);
        {
            double all = cpu_ms(RUSAGE_SELF), emu = cpu_ms(RUSAGE_THREAD);
            foir_log(FOIR_LOG_INFO, "cpu: %.2f ms/frame in the process (%.2f on the emulation thread, %.2f on the other threads)",
                     (all - l_cpu_all_base) / 120, (emu - l_cpu_emu_base) / 120, (all - l_cpu_all_base - emu + l_cpu_emu_base) / 120);
            l_cpu_all_base = all; l_cpu_emu_base = emu;
        }
        {
            uint64_t cov[COV_N], texall;
            rdp_coverage(&l_rdp, cov, &texall);
            double all = (double)(cov[5] - l_cov_base[5]), ta = (double)(texall - l_covtex_base);
            foir_log(FOIR_LOG_INFO, "specialised pixels: texture %.1f%% (of %.0f%% textured; any-format kernels %.1f%%), combine %.1f%% (shape kernels %.1f%%), zcmp %.1f%%, blend %.1f%%, store %.1f%%",
                     ta > 0 ? 100.0 * (double)(cov[0] - l_cov_base[0]) / ta : 0.0, all > 0 ? 100.0 * ta / all : 0.0,
                     ta > 0 ? 100.0 * (double)(cov[7] - l_cov_base[7]) / ta : 0.0,
                     all > 0 ? 100.0 * (double)(cov[1] - l_cov_base[1]) / all : 0.0, all > 0 ? 100.0 * (double)(cov[6] - l_cov_base[6]) / all : 0.0,
                     all > 0 ? 100.0 * (double)(cov[2] - l_cov_base[2]) / all : 0.0,
                     all > 0 ? 100.0 * (double)(cov[3] - l_cov_base[3]) / all : 0.0, all > 0 ? 100.0 * (double)(cov[4] - l_cov_base[4]) / all : 0.0);
            char vr[400];
            foir_neon_verify_report(vr, sizeof vr);
            if (vr[0]) foir_log(FOIR_LOG_INFO, "FOIR_NEON_VERIFY: %s", vr);
            for (int k = 0; k < COV_N; k++) l_cov_base[k] = cov[k];
            l_covtex_base = texall;
            foir_stats_flush();
        }
        l_rdp_ms = l_vi_ms = l_sync_ms = l_present_ms = 0; l_lists = 0;
        l_cmds_base = l_rdp.frame_cmds; l_prims_base = l_rdp.stat_prims; l_pix_base = pix;
    }
}

EXPORT void CALL ShowCFB(void) {}
EXPORT void CALL ViStatusChanged(void) {}
EXPORT void CALL ViWidthChanged(void) {}
EXPORT void CALL MoveScreen(int x, int y) { (void)x; (void)y; }
EXPORT void CALL ChangeWindow(void) { gl_out_toggle_fullscreen(); }
EXPORT void CALL SetRenderingCallback(void (*cb)(int)) { l_render_cb = cb; }
EXPORT void CALL ResizeVideoOutput(int w, int h) { l_win_w = w; l_win_h = h; gl_out_resize(w, h); }

/* The core expects a bottom-up RGB24 image (see main/screenshot.c) */
EXPORT void CALL ReadScreen2(void *dest, int *width, int *height, int front)
{
    (void)front;
    struct vi_regs v;
    static struct vi_frame shot;
    if (!l_rom_open) { *width = 640; *height = 480; return; }
    rdp_sync(&l_rdp);
    read_vi(&v);
    vi_render(&l_rdp, &v, &shot);
    int w = shot.valid ? shot.width : 640, h = shot.valid ? shot.height : 480;
    *width = w;
    *height = h;
    if (!dest) return;
    uint8_t *out = (uint8_t *)dest;
    for (int y = 0; y < h; y++) {
        uint8_t *row = out + (size_t)(h - 1 - y) * (size_t)w * 3;
        for (int x = 0; x < w; x++) {
            uint32_t p = shot.valid ? shot.pixels[y * w + x] : 0;
            row[x * 3 + 0] = (uint8_t)(p & 0xff);
            row[x * 3 + 1] = (uint8_t)((p >> 8) & 0xff);
            row[x * 3 + 2] = (uint8_t)((p >> 16) & 0xff);
        }
    }
}

EXPORT void CALL FBRead(unsigned int addr) { (void)addr; if (l_rom_open) rdp_sync(&l_rdp); }
EXPORT void CALL FBWrite(unsigned int addr, unsigned int size) { (void)addr; (void)size; if (l_rom_open) rdp_sync(&l_rdp); }
EXPORT void CALL FBGetFrameBufferInfo(void *p) { (void)p; }

static const char *const l_lbl_frameskip[] = { "Off", "Every 2nd", "2 of 3", "3 of 4", "Auto" };
static const char *const l_lbl_threads[] = { "None", "Auto", "1", "2", "3", "4", "5", "6", "7" };

/* Option ids are the ABI: append only.  enum values: FrameSkip index 0..3 = skip level 0..3, 4 = auto; Threads index =
 * Threads + 1 (so -1 "None" = render on the emulation thread). */
enum { OPT_WIDESCREEN, OPT_FILTER, OPT_INTERLACE, OPT_FRAMESKIP, OPT_VSYNC, OPT_THREADS, OPT_NEON, OPT_SPECIALIZE,
       OPT_FASTCOMB, OPT_RELAXED, OPT_CROP_H, OPT_CROP_V, OPT_VIAA, OPT_VIDIVOT, OPT_VIDEDITHER, OPT_VIGAMMA, OPT_COUNT };

static const struct foir_suxty_option l_opts[OPT_COUNT] = {
    [OPT_WIDESCREEN] = { "Widescreen", "Widescreen", "Stretch to the whole screen instead of 4:3", FOIR_OPT_BOOL, FOIR_OPTF_LIVE, 0, 1, 1, NULL, 0 },
    [OPT_FILTER]     = { "Filter", "Smoothing", "Bilinear filtering when scaling the image", FOIR_OPT_BOOL, FOIR_OPTF_LIVE, 0, 1, 1, NULL, 0 },
    [OPT_INTERLACE]  = { "InterlaceHack", "Interlace Hack", "Show one field line-doubled instead of weaving fields", FOIR_OPT_BOOL, FOIR_OPTF_LIVE, 0, 1, 1, NULL, 1 },
    [OPT_FRAMESKIP]  = { "FrameSkip", "Frame Skip", "Skip rendering to hold full speed (Auto adapts)", FOIR_OPT_ENUM, FOIR_OPTF_LIVE, 0, 4, 1, l_lbl_frameskip, 0 },
    [OPT_VSYNC]      = { "VSync", "VSync", "Wait for vertical sync when presenting", FOIR_OPT_BOOL, 0, 0, 1, 1, NULL, 1 },
    [OPT_THREADS]    = { "Threads", "Worker Threads", "RDP worker threads (None = render on the emulation thread)", FOIR_OPT_ENUM, 0, 0, 8, 1, l_lbl_threads, 1 },
    [OPT_NEON]       = { "NEON", "NEON Kernels", "Off = scalar reference kernels (slow)", FOIR_OPT_BOOL, 0, 0, 1, 1, NULL, 1 },
    [OPT_SPECIALIZE] = { "Specialize", "Specialised Kernels", "Per-state precompiled pixel kernels", FOIR_OPT_BOOL, 0, 0, 1, 1, NULL, 1 },
    [OPT_FASTCOMB]   = { "FastCombiner", "Fast Combiner", "Fast paths for common combiner/blender modes", FOIR_OPT_BOOL, 0, 0, 1, 1, NULL, 1 },
    [OPT_RELAXED]    = { "RelaxedPrecision", "Relaxed Precision", "Forces lower accuracy depth values and calculations for speed", FOIR_OPT_BOOL, 0, 0, 1, 1, NULL, 0 },
    [OPT_CROP_H]     = { "CropOverscanH", "Crop Overscan H", "Hide edge artefacts: crop 8 VI pixels off the left and right", FOIR_OPT_BOOL, FOIR_OPTF_LIVE, 0, 1, 1, NULL, 0 },
    [OPT_CROP_V]     = { "CropOverscanV", "Crop Overscan V", "Hide edge artefacts: crop 12 VI lines off the top and bottom", FOIR_OPT_BOOL, FOIR_OPTF_LIVE, 0, 1, 1, NULL, 0 },
    [OPT_VIAA]       = { "VIAntiAliasing", "VI Anti-Aliasing", "Edge anti-aliasing as the VI does, where the game uses it (costs time)", FOIR_OPT_BOOL, FOIR_OPTF_LIVE, 0, 1, 1, NULL, 0 },
    [OPT_VIDIVOT]    = { "VIDivot", "VI Divot", "Divot filter as the VI does, where the game uses it (costs time)", FOIR_OPT_BOOL, FOIR_OPTF_LIVE, 0, 1, 1, NULL, 0 },
    [OPT_VIDEDITHER] = { "VIDedither", "VI Dither Restore", "Dither restore as the VI does, where the game uses it (costs time)", FOIR_OPT_BOOL, FOIR_OPTF_LIVE, 0, 1, 1, NULL, 0 },
    [OPT_VIGAMMA]    = { "VIGamma", "VI Gamma", "Gamma correction as the VI does, where the game uses it (costs time)", FOIR_OPT_BOOL, FOIR_OPTF_LIVE, 0, 1, 1, NULL, 0 },
};

static bool l_staged_set[OPT_COUNT];
static int  l_staged[OPT_COUNT];

static int opt_load(const struct foir_config *c, unsigned i)
{
    switch (i) {
    case OPT_WIDESCREEN: return c->widescreen;
    case OPT_FILTER:     return c->filter;
    case OPT_INTERLACE:  return c->interlace_hack;
    case OPT_FRAMESKIP:  return c->frame_skip == FOIR_FRAMESKIP_AUTO ? 4 : (c->frame_skip < 0 ? 0 : c->frame_skip > 3 ? 3 : c->frame_skip);
    case OPT_VSYNC:      return c->vsync;
    case OPT_THREADS:    return c->threads < -1 ? 0 : c->threads > 7 ? 8 : c->threads + 1;
    case OPT_NEON:       return c->neon;
    case OPT_SPECIALIZE: return c->specialize;
    case OPT_FASTCOMB:   return c->fast_combiner;
    case OPT_RELAXED:    return c->relaxed_precision;
    case OPT_CROP_H:     return c->crop_h;
    case OPT_CROP_V:     return c->crop_v;
    case OPT_VIAA:       return c->vi_aa;
    case OPT_VIDIVOT:    return c->vi_divot;
    case OPT_VIDEDITHER: return c->vi_dedither;
    case OPT_VIGAMMA:    return c->vi_gamma;
    }
    return 0;
}

static void opt_store(struct foir_config *c, unsigned i, int v)
{
    switch (i) {
    case OPT_WIDESCREEN: c->widescreen = v != 0; break;
    case OPT_FILTER:     c->filter = v != 0; break;
    case OPT_INTERLACE:  c->interlace_hack = v != 0; break;
    case OPT_FRAMESKIP:  c->frame_skip = v == 4 ? FOIR_FRAMESKIP_AUTO : v; break;
    case OPT_VSYNC:      c->vsync = v != 0; break;
    case OPT_THREADS:    c->threads = v - 1; break;
    case OPT_NEON:       c->neon = v != 0; break;
    case OPT_SPECIALIZE: c->specialize = v != 0; break;
    case OPT_FASTCOMB:   c->fast_combiner = v != 0; break;
    case OPT_RELAXED:    c->relaxed_precision = v != 0; break;
    case OPT_CROP_H:     c->crop_h = v != 0; break;
    case OPT_CROP_V:     c->crop_v = v != 0; break;
    case OPT_VIAA:       c->vi_aa = v != 0; break;
    case OPT_VIDIVOT:    c->vi_divot = v != 0; break;
    case OPT_VIDEDITHER: c->vi_dedither = v != 0; break;
    case OPT_VIGAMMA:    c->vi_gamma = v != 0; break;
    }
}

static void suxty_apply_staged(struct foir_config *c)
{
    for (unsigned i = 0; i < OPT_COUNT; i++)
        if (l_staged_set[i]) opt_store(c, i, l_staged[i]);
}

static bool suxty_thread_ok(void)
{
    return !l_rom_open || pthread_equal(pthread_self(), l_emu_thread);
}

static int suxty_get(unsigned i, int *value)
{
    if (i >= OPT_COUNT || !value) return FOIR_SUXTY_E_INDEX;
    if (!l_initialized) return FOIR_SUXTY_E_STATE;
    if (!suxty_thread_ok()) return FOIR_SUXTY_E_THREAD;
    if (l_rom_open) { *value = opt_load(&l_cfg, i); return 0; }
    if (l_staged_set[i]) { *value = l_staged[i]; return 0; }
    struct foir_config c = l_cfg;
    read_foir_section(&c);
    *value = opt_load(&c, i);
    return 0;
}

static int suxty_set(unsigned i, int value)
{
    if (i >= OPT_COUNT) return FOIR_SUXTY_E_INDEX;
    if (!l_initialized) return FOIR_SUXTY_E_STATE;
    if (value < l_opts[i].min || value > l_opts[i].max) return FOIR_SUXTY_E_RANGE;
    if (!suxty_thread_ok()) return FOIR_SUXTY_E_THREAD;
    l_staged_set[i] = true;
    l_staged[i] = value;
    if (!l_rom_open) return FOIR_SUXTY_APPLIED;       /* RomOpen reads it */
    if (!(l_opts[i].flags & FOIR_OPTF_LIVE)) return FOIR_SUXTY_NEXT_LAUNCH;
    /* on the emulation thread between frames: rdp_sync() has drained the workers, so nothing runs concurrently */
    opt_store(&l_cfg, i, value);
    switch (i) {
    case OPT_WIDESCREEN: gl_out_set_widescreen(value != 0); break;
    case OPT_FILTER:     gl_out_set_filter(value != 0); break;
    case OPT_INTERLACE:  l_rdp.cfg.interlace_hack = value != 0; break;
    case OPT_CROP_H:     l_rdp.cfg.crop_h = value != 0; break;
    case OPT_CROP_V:     l_rdp.cfg.crop_v = value != 0; break;
    case OPT_VIAA:       l_rdp.cfg.vi_aa = value != 0; break;
    case OPT_VIDIVOT:    l_rdp.cfg.vi_divot = value != 0; break;
    case OPT_VIDEDITHER: l_rdp.cfg.vi_dedither = value != 0; break;
    case OPT_VIGAMMA:    l_rdp.cfg.vi_gamma = value != 0; break;
    case OPT_FRAMESKIP:
        l_rdp.skip_level = 0; l_win_frames = 0; l_rdp.skip_pixels = false; break;
    }
    return FOIR_SUXTY_APPLIED;
}

static int suxty_save_default(unsigned i, int value)
{
    if (i >= OPT_COUNT) return FOIR_SUXTY_E_INDEX;
    if (!l_initialized || !ConfigSetParameter || !ConfigSaveSection || !l_cfg_foir) return FOIR_SUXTY_E_STATE;
    if (value < l_opts[i].min || value > l_opts[i].max) return FOIR_SUXTY_E_RANGE;
    struct foir_config c;
    memset(&c, 0, sizeof c);
    opt_store(&c, i, value);
    const char *key = l_opts[i].key;
    int iv = 0;
    m64p_error e;
    switch (i) {
    case OPT_FRAMESKIP: {
        char buf[8];
        if (c.frame_skip == FOIR_FRAMESKIP_AUTO) snprintf(buf, sizeof buf, "auto"); else snprintf(buf, sizeof buf, "%d", c.frame_skip);
        e = ConfigSetParameter(l_cfg_foir, key, M64TYPE_STRING, buf);
        break; }
    case OPT_THREADS:
        iv = c.threads;
        e = ConfigSetParameter(l_cfg_foir, key, M64TYPE_INT, &iv);
        break;
    default:
        iv = value != 0;
        e = ConfigSetParameter(l_cfg_foir, key, M64TYPE_BOOL, &iv);
        break;
    }
    if (e != M64ERR_SUCCESS) return FOIR_SUXTY_E_CONFIG;
    return ConfigSaveSection("Video-Foir") == M64ERR_SUCCESS ? 0 : FOIR_SUXTY_E_CONFIG;
}

static const struct foir_suxty_interface l_suxty_if = {
    FOIR_SUXTY_ABI, sizeof(struct foir_suxty_interface), FOIR_VERSION, OPT_COUNT, l_opts,
    suxty_get, suxty_set, suxty_save_default
};

EXPORT const struct foir_suxty_interface *FoirSuxtyGetInterface(void)
{
    return &l_suxty_if;
}
