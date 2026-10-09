/*
 * mupen64plus-rdp-foir - foir.h
 * Common definitions shared by every foir module.
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FOIR_H
#define FOIR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define FOIR_VERSION        0x000100  /* 0.1.0 (M1) */
#define FOIR_NAME           "foir RDP"

#if defined(__GNUC__)
#define FOIR_INLINE   static inline __attribute__((always_inline))
#define FOIR_UNUSED   __attribute__((unused))
#define FOIR_LIKELY(x)   __builtin_expect(!!(x), 1)
#define FOIR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FOIR_INLINE   static inline
#define FOIR_UNUSED
#define FOIR_LIKELY(x)   (x)
#define FOIR_UNLIKELY(x) (x)
#endif

#define UNUSED(x) (void)(x)

/* RDRAM in Mupen64Plus is stored as host 32-bit words (word-swapped on LE hosts) */
#ifdef FOIR_BIG_ENDIAN
#define FOIR_BYTE_XOR   0
#define FOIR_HALF_XOR   0
#else
#define FOIR_BYTE_XOR   3
#define FOIR_HALF_XOR   1
#endif

/* log levels match M64MSG_* in m64p_types.h */
enum { FOIR_LOG_ERROR = 1, FOIR_LOG_WARNING = 2, FOIR_LOG_INFO = 3, FOIR_LOG_STATUS = 4, FOIR_LOG_VERBOSE = 5 };
void foir_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void foir_log_set_callback(void (*cb)(void *ctx, int level, const char *msg), void *ctx);

#endif
