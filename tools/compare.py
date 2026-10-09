#!/usr/bin/env python3
"""compare.py: decode the framebuffer out of two RDRAM dumps (foir vs angrylion)
and report / visualise the differences.

usage: compare.py dump.fdump foir.rdram angry.rdram out_prefix
Writes out_prefix_foir.png, _angry.png, _diff.png and prints statistics.
"""
import struct
import sys

import numpy as np
from PIL import Image


def read_hdr(path):
    with open(path, 'rb') as f:
        h = f.read(8 + 4 + 28 + 8)
    magic, size = h[:8], struct.unpack('<I', h[8:12])[0]
    vi = struct.unpack('<7I', h[12:40])
    return size, vi


def fb_region(vi):
    status, origin, width, hs, vs, xs, ys = vi
    typ = status & 3
    hstart, hend = (hs >> 16) & 0x3FF, hs & 0x3FF
    vstart, vend = (vs >> 16) & 0x3FF, vs & 0x3FF
    w = ((hend - hstart) * (xs & 0xFFF)) >> 10
    h = (((vend - vstart) >> 1) * (ys & 0xFFF)) >> 10
    return typ, origin & 0xFFFFFF, width & 0xFFF, w, h


def decode(rdram, typ, origin, stride, w, h):
    """rdram is host-word order: byte address a lives at index a^3"""
    words = np.frombuffer(rdram, dtype='<u4')
    img = np.zeros((h, w, 3), np.uint8)
    cvg = np.zeros((h, w), np.uint8)
    if typ == 2:
        h16 = np.frombuffer(rdram, dtype='<u2')
        idx = ((origin >> 1) + np.arange(h)[:, None] * stride + np.arange(w)[None, :]) ^ 1
        p = h16[idx % len(h16)].astype(np.uint32)
        r, g, b = (p >> 11) & 31, (p >> 6) & 31, (p >> 1) & 31
        img[..., 0] = (r << 3) | (r >> 2)
        img[..., 1] = (g << 3) | (g >> 2)
        img[..., 2] = (b << 3) | (b >> 2)
    elif typ == 3:
        idx = (origin >> 2) + np.arange(h)[:, None] * stride + np.arange(w)[None, :]
        p = words[idx % len(words)]
        img[..., 0] = (p >> 24) & 255
        img[..., 1] = (p >> 16) & 255
        img[..., 2] = (p >> 8) & 255
    return img


def main():
    dump, a_path, b_path, prefix = sys.argv[1:5]
    size, vi = read_hdr(dump)
    typ, origin, stride, w, h = fb_region(vi)
    if typ < 2 or w <= 0 or h <= 0:
        print("blank VI")
        return 0
    A = decode(open(a_path, 'rb').read(), typ, origin, stride, w, h)
    B = decode(open(b_path, 'rb').read(), typ, origin, stride, w, h)
    Image.fromarray(A).save(prefix + '_foir.png')
    Image.fromarray(B).save(prefix + '_angry.png')
    d = np.abs(A.astype(int) - B.astype(int))
    dm = d.max(axis=2)
    diff_img = np.clip(dm * 4, 0, 255).astype(np.uint8)
    Image.fromarray(diff_img).save(prefix + '_diff.png')
    n = dm.size
    bad = int((dm > 0).sum())
    bad8 = int((dm > 8).sum())
    bad32 = int((dm > 32).sum())
    print("%dx%d  differing pixels: %d (%.2f%%)  >8: %d (%.2f%%)  >32: %d (%.2f%%)  max diff %d  mean %.3f" % (
        w, h, bad, 100.0 * bad / n, bad8, 100.0 * bad8 / n, bad32, 100.0 * bad32 / n, int(dm.max()), d.mean()))
    return 0


if __name__ == '__main__':
    sys.exit(main())
