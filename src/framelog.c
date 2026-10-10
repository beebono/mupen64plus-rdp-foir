/*
 * mupen64plus-rdp-foir - framelog.c (see framelog.h)
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#ifdef FOIR_FRAMELOG
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "framelog.h"

/* header: magic[8], u32 rec_bytes, u32 capacity, u32 count (written last), u32 version, u64 t0_ns (CLOCK_MONOTONIC), u64 realtime ns */
struct fl_hdr { char magic[8]; uint32_t rec_bytes, capacity; uint32_t count, version; uint64_t t0_ns, realtime_ns; };

static unsigned char *l_map;
static size_t l_len;
static struct fl_hdr *l_hdr;
static uint32_t l_n;

bool framelog_open(void)
{
    const char *path = getenv("FOIR_FRAMELOG");
    if (!path || !*path || l_map) return l_map != NULL;
    const char *mx = getenv("FOIR_FRAMELOG_MAX");
    uint32_t cap = mx && atoi(mx) > 0 ? (uint32_t)atoi(mx) : 36000;      /* 10 minutes at 60 Hz */
    size_t len = FL_HDR_BYTES + (size_t)cap * sizeof(struct fl_rec);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)len) != 0) { close(fd); return false; }
    void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return false;
    memset(m, 0, len);                       /* fault every page in now, not during a frame */
    l_map = (unsigned char *)m; l_len = len; l_hdr = (struct fl_hdr *)m; l_n = 0;
    struct timespec t, rt;
    clock_gettime(CLOCK_MONOTONIC, &t); clock_gettime(CLOCK_REALTIME, &rt);
    memcpy(l_hdr->magic, FL_MAGIC, 8);
    l_hdr->rec_bytes = sizeof(struct fl_rec); l_hdr->capacity = cap; l_hdr->version = 1;
    l_hdr->t0_ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
    l_hdr->realtime_ns = (uint64_t)rt.tv_sec * 1000000000ull + (uint64_t)rt.tv_nsec;
    return true;
}

void framelog_put(const struct fl_rec *r)
{
    if (!l_map || l_n >= l_hdr->capacity) return;
    memcpy(l_map + FL_HDR_BYTES + (size_t)l_n * sizeof(*r), r, sizeof(*r));
    __atomic_store_n(&l_hdr->count, ++l_n, __ATOMIC_RELEASE);
}

void framelog_close(void)
{
    if (!l_map) return;
    msync(l_map, l_len, MS_ASYNC);
    munmap(l_map, l_len);
    l_map = NULL; l_hdr = NULL; l_n = 0;
}
#else
typedef int foir_framelog_disabled;   /* keep the translation unit non-empty */
#endif
