/*
 * mupen64plus-rdp-foir - framelog.h
 * FOIR_FRAMELOG=<path>: one fixed-size binary record per UpdateScreen (= per presented VI frame), written straight into a
 * shared mmap of the file (no syscall per frame, survives SIGKILL).  Compiled in only with -DFOIR_FRAMELOG (CFLAGS env var); then off unless the variable is set.  Measurement aid only;
 * decode with tools/framelog.py.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FOIR_FRAMELOG_H
#define FOIR_FRAMELOG_H

#include <stdbool.h>
#include <stdint.h>

#define FL_MAGIC "FOIRFL01"
#define FL_HDR_BYTES 64

/* time stamps are CLOCK_MONOTONIC nanoseconds; the *_ns fields are the time of the interval that ENDS at this UpdateScreen
 * (ProcessRDPList calls and worker activity since the previous UpdateScreen finished) except drain / vi / present, which
 * are the durations of this UpdateScreen itself. */
struct fl_rec {
    uint64_t t_enter_ns;      /* UpdateScreen entry */
    uint64_t t_leave_ns;      /* UpdateScreen exit (after the buffer swap) */
    uint32_t frame;           /* UpdateScreen count, 1-based */
    uint32_t lists;           /* ProcessRDPList calls in the interval */
    uint32_t cb_ns;           /* time inside those calls (parse + setup + all waits for the workers) */
    uint32_t cb_max_ns;       /* longest single call */
    uint32_t syncfull_ns;     /* of cb_ns: waiting for the workers at SYNC_FULL */
    uint32_t mtsync_ns;       /* all worker-pool barrier waits since the previous record (SYNC_FULL + hazards + drain) */
    uint32_t drain_ns;        /* rdp_sync at the start of UpdateScreen */
    uint32_t vi_ns;           /* vi_render_ex */
    uint32_t present_ns;      /* gl_out_present (incl. the swap) */
    uint32_t cmds;            /* RDP commands parsed in the interval */
    uint32_t prims;           /* primitives set up */
    uint32_t pixels;          /* pixels rendered (workers + parser) */
    uint32_t loads;           /* LOAD_BLOCK / LOAD_TILE / LOAD_TLUT */
    uint32_t wk_busy_sum_ns;  /* sum over the workers of the time spent rendering jobs */
    uint32_t wk_busy_max_ns;  /* busiest worker */
    uint32_t jobs;            /* worker jobs published */
    uint32_t vi_res;          /* vi width << 16 | height */
    uint32_t thr_cpu_ns;      /* emulation thread CPU time (user + system) since the previous record */
    uint32_t thr_faults;      /* minor faults | major faults << 16 (deltas) */
    uint32_t thr_ctxsw;       /* voluntary | involuntary context switches << 16 (deltas) */
};

bool framelog_open(void);                     /* FOIR_FRAMELOG set and the file mapped */
void framelog_put(const struct fl_rec *r);
void framelog_close(void);

#endif
