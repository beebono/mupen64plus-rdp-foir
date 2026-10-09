#!/usr/bin/env python3
"""specmatrix.py: the part of the variant lists that is a plain cross product instead of a statistic.

  specmatrix.py tex     the texture unit variants for every common texel format x point / bilinear x perspective x
                        one / two cycles (what any game can use; the rarer combinations come from specstats.py)
  specmatrix.py texl    the LOD (TEX_LOD_EN) variants of the same
  specmatrix.py store   the FB / Z store variants: 16-bit RGBA colour buffer, every coverage destination x Z update
  specmatrix.py zcmp    the Z compare variants: every combination of image read, Z mode, memory-alpha shifts and
                        RelaxedPrecision for a 16-bit colour buffer (ANTIALIAS_EN / FORCE_BLEND are read at run time)

Both print lines for src/spec_tex.def / src/spec_store.def (see kspec_tex.inc, kspec_mem.inc).
"""
import sys

# (format, size, tlut): RGBA16, RGBA32, IA4, IA8, IA16, I4, I8, CI4 + TLUT, CI8 + TLUT
FORMATS = [(0, 2, 0), (0, 3, 0), (3, 0, 0), (3, 1, 0), (3, 2, 0), (4, 0, 0), (4, 1, 0), (2, 0, 1), (2, 1, 1)]


def tex():
    for fmt, siz, tlut in FORMATS:
        for two in (0, 1):
            for smp in (0, 1):
                for persp in (0, 1):
                    # TEX(two, t0,t1,nx, fmt0,siz0, fmt1,siz1, smp,mid, tlut,tlt, persp, bl0,bl1)
                    print('TEX(%d, 1,0,0, %d,%d, 0,0, %d,0, %d,0, %d, 1,%d)' % (two, fmt, siz, smp, tlut, persp, two))


def texl():
    """LOD states: one cycle, bilinear, perspective (what mip-mapped 3D textures use), every common texel format, the LOD
    selecting the tile (two-cycle and other LOD states come from the statistics)"""
    for fmt, siz, tlut in FORMATS:
        # TEXL(two, t0,t1,nx, fmt0,siz0, fmt1,siz1, smp,mid, tlut,tlt, persp, bl0,bl1, lodt)
        print('TEXL(0, 1,0,0, %d,%d, 0,0, 1,0, %d,0, 1, 1,0, 1)' % (fmt, siz, tlut))


def store():
    for cvgdest in range(4):
        for zupd in range(2):
            print('STORE(2, 1, %d, %d)' % (cvgdest, zupd))


def zcmp():
    # ZCMP(fbsz, rd,fbfmt, zc,zmode, shifts, relaxed): RelaxedPrecision only exists for the opaque / transparent compare
    for rd, fbfmt in ((0, 0), (1, 0), (1, 1)):
        for shifts in (0, 1):
            for zc, zmode in ((0, 0), (1, 0), (1, 1), (1, 2), (1, 3)):
                for relaxed in ((0, 1) if zc and zmode in (0, 2) else (0,)):
                    print('ZCMP(2, %d,%d, %d,%d, %d, %d)' % (rd, fbfmt, zc, zmode, shifts, relaxed))


if __name__ == '__main__':
    {'tex': tex, 'texl': texl, 'store': store, 'zcmp': zcmp}[sys.argv[1]]()
