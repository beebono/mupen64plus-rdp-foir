/*
 * mupen64plus-rdp-foir - combreg.h
 *
 * Copyright (C) 2026 foir contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FOIR_COMBREG_H
#define FOIR_COMBREG_H

#include "rdp.h"

/* the functions fold to constants for literal arguments (specialised combiner kernels) */
FOIR_INLINE int comb_reg_rgb(int pos, int sel, int ch)
{
    switch (pos) {
    case 0:
        switch (sel & 0xf) {
        case 0: return REG_COMB + ch;  case 1: return REG_TEX0 + ch; case 2: return REG_TEX1 + ch;
        case 3: return REG_PRIM + ch;  case 4: return REG_SHADE + ch; case 5: return REG_ENV + ch;
        case 6: return REG_ONE;        case 7: return REG_NOISE;     default: return REG_ZERO;
        }
    case 1:
        switch (sel & 0xf) {
        case 0: return REG_COMB + ch;  case 1: return REG_TEX0 + ch; case 2: return REG_TEX1 + ch;
        case 3: return REG_PRIM + ch;  case 4: return REG_SHADE + ch; case 5: return REG_ENV + ch;
        case 6: return REG_KEYC + ch;  case 7: return REG_K4;        default: return REG_ZERO;
        }
    case 2:
        switch (sel & 0x1f) {
        case 0: return REG_COMB + ch;  case 1: return REG_TEX0 + ch; case 2: return REG_TEX1 + ch;
        case 3: return REG_PRIM + ch;  case 4: return REG_SHADE + ch; case 5: return REG_ENV + ch;
        case 6: return REG_KEYS + ch;  case 7: return REG_COMB + 3;  case 8: return REG_TEX0 + 3;
        case 9: return REG_TEX1 + 3;   case 10: return REG_PRIM + 3; case 11: return REG_SHADE + 3;
        case 12: return REG_ENV + 3;   case 13: return REG_LODF;     case 14: return REG_PLODF;
        case 15: return REG_K5;        default: return REG_ZERO;
        }
    default:
        switch (sel & 7) {
        case 0: return REG_COMB + ch;  case 1: return REG_TEX0 + ch; case 2: return REG_TEX1 + ch;
        case 3: return REG_PRIM + ch;  case 4: return REG_SHADE + ch; case 5: return REG_ENV + ch;
        case 6: return REG_ONE;        default: return REG_ZERO;
        }
    }
}

FOIR_INLINE int comb_reg_alpha(int pos, int sel)
{
    if (pos == 2) {
        switch (sel & 7) {
        case 0: return REG_LODF;       case 1: return REG_TEX0 + 3;  case 2: return REG_TEX1 + 3;
        case 3: return REG_PRIM + 3;   case 4: return REG_SHADE + 3; case 5: return REG_ENV + 3;
        case 6: return REG_PLODF;      default: return REG_ZERO;
        }
    }
    switch (sel & 7) {
    case 0: return REG_COMB + 3;  case 1: return REG_TEX0 + 3;  case 2: return REG_TEX1 + 3;
    case 3: return REG_PRIM + 3;  case 4: return REG_SHADE + 3; case 5: return REG_ENV + 3;
    case 6: return REG_ONE;       default: return REG_ZERO;
    }
}

#endif
