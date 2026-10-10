/*
 * mupen64plus-rdp-foir - framelog.h
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

/* FOIR_FRAMELOG=<path>: one fixed-size binary record per UpdateScreen written into a shared mmap (no syscall per frame, survives SIGKILL); tools/framelog.py decodes it */

/* *_ns cover the interval ending at this UpdateScreen, except drain / vi / present (this UpdateScreen's own durations) */
struct fl_rec {
    uint64_t t_enter_ns;
    uint64_t t_leave_ns;
    uint32_t frame;
    uint32_t lists;
    uint32_t cb_ns;
    uint32_t cb_max_ns;
    uint32_t syncfull_ns;
    uint32_t mtsync_ns;
    uint32_t drain_ns;
    uint32_t vi_ns;
    uint32_t present_ns;
    uint32_t cmds;
    uint32_t prims;
    uint32_t pixels;
    uint32_t loads;
    uint32_t wk_busy_sum_ns;
    uint32_t wk_busy_max_ns;
    uint32_t jobs;
    uint32_t vi_res;          /* width << 16 | height */
    uint32_t thr_cpu_ns;
    uint32_t thr_faults;      /* minor | major << 16 */
    uint32_t thr_ctxsw;       /* voluntary | involuntary << 16 */
    uint32_t helper_ns;
    uint32_t reserved;
};

bool framelog_open(void);
void framelog_put(const struct fl_rec *r);
void framelog_close(void);

#endif
