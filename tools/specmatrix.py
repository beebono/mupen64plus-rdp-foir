#!/usr/bin/env python3
"""specmatrix.py tex|texl|store|zcmp: prints the cross-product lines of src/spec_{tex,store,zcmp}.def (the rarer variants come from specstats.py)"""
import sys

# (format, size, tlut): RGBA16, RGBA32, IA4, IA8, IA16, I4, I8, CI4 + TLUT, CI8 + TLUT
FORMATS = [(0, 2, 0), (0, 3, 0), (3, 0, 0), (3, 1, 0), (3, 2, 0), (4, 0, 0), (4, 1, 0), (2, 0, 1), (2, 1, 1)]


def tex():
    for fmt, siz, tlut in FORMATS:
        for two in (0, 1):
            for smp in (0, 1):
                for persp in (0, 1):
                    print('TEX(%d, 1,0,0, %d,%d, 0,0, %d,0, %d,0, %d, 1,%d)' % (two, fmt, siz, smp, tlut, persp, two))


def texl():
    """one cycle, bilinear, perspective, the LOD selecting the tile"""
    for fmt, siz, tlut in FORMATS:
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
