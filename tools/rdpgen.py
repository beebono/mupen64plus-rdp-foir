#!/usr/bin/env python3
"""rdpgen: tiny RDP command assembler used to build synthetic foir dumps.

Commands are lists of 32-bit words (big-endian values, as the RDP sees them).
A Scene collects commands plus an RDRAM image and writes an .fdump file in the
format described in fdump.h.
"""
import math
import struct

RDRAM_SIZE = 0x800000


def w(*words):
    return [x & 0xFFFFFFFF for x in words]


# ---- state commands ---------------------------------------------------------

def set_color_image(fmt, size, width, addr):
    return w((0x3F << 24) | (fmt << 21) | (size << 19) | (width - 1), addr)


def set_mask_image(addr):
    return w(0x3E << 24, addr)


def set_texture_image(fmt, size, width, addr):
    return w((0x3D << 24) | (fmt << 21) | (size << 19) | (width - 1), addr)


def set_scissor(xh, yh, xl, yl, field=0, odd=0):
    return w((0x2D << 24) | (int(xh * 4) << 12) | int(yh * 4),
             (field << 25) | (odd << 24) | (int(xl * 4) << 12) | int(yl * 4))


def set_fill_color(c):
    return w(0x37 << 24, c)


def set_prim_color(c, minlevel=0, lodfrac=0):
    return w((0x3A << 24) | (minlevel << 8) | lodfrac, c)


def set_env_color(c):
    return w(0x3B << 24, c)


def set_blend_color(c):
    return w(0x39 << 24, c)


def set_fog_color(c):
    return w(0x38 << 24, c)


def set_prim_depth(z, dz):
    return w(0x2E << 24, ((z & 0x7FFF) << 16) | (dz & 0xFFFF))


def set_convert(k0, k1, k2, k3, k4, k5):
    return w((0x2C << 24) | ((k0 & 0x1FF) << 13) | ((k1 & 0x1FF) << 4) | ((k2 >> 5) & 0xF),
             ((k2 & 0x1F) << 27) | ((k3 & 0x1FF) << 18) | ((k4 & 0x1FF) << 9) | (k5 & 0x1FF))


def set_combine(a0, b0, c0, d0, A0, B0, C0, D0, a1, b1, c1, d1, A1, B1, C1, D1):
    """rgb (a-b)*c+d, alpha (A-B)*C+D for cycle 0 and cycle 1 (raw field values)."""
    w0 = (a0 << 20) | (c0 << 15) | (A0 << 12) | (C0 << 9) | (a1 << 5) | c1
    w1 = (b0 << 28) | (b1 << 24) | (A1 << 21) | (C1 << 18) | (d0 << 15) | (B0 << 12) | \
         (D0 << 9) | (d1 << 6) | (B1 << 3) | D1
    return w((0x3C << 24) | w0, w1)


def set_other_modes(**k):
    g = lambda n, d=0: k.get(n, d)
    hi = (g('cycle', 0) << 20) | (g('persp') << 19) | (g('detail') << 18) | (g('sharpen') << 17) | \
         (g('lod') << 16) | (g('tlut') << 15) | (g('tlut_ia') << 14) | (g('bilerp_sample') << 13) | \
         (g('mid_texel') << 12) | (g('bi0') << 11) | (g('bi1') << 10) | (g('conv1') << 9) | \
         (g('key') << 8) | (g('rgb_dither', 3) << 6) | (g('alpha_dither', 3) << 4)
    lo = (g('m1a0') << 30) | (g('m1a1') << 28) | (g('m1b0') << 26) | (g('m1b1') << 24) | \
         (g('m2a0') << 22) | (g('m2a1') << 20) | (g('m2b0') << 18) | (g('m2b1') << 16) | \
         (g('force_blend') << 14) | (g('alpha_cvg') << 13) | (g('cvg_times_alpha') << 12) | \
         (g('zmode') << 10) | (g('cvg_dest') << 8) | (g('color_on_cvg') << 7) | \
         (g('image_read') << 6) | (g('z_update') << 5) | (g('z_compare') << 4) | \
         (g('aa') << 3) | (g('z_source') << 2) | (g('dither_alpha') << 1) | g('alpha_compare')
    return w((0x2F << 24) | hi, lo)


def set_tile(fmt, size, line, tmem, tile, pal=0, ct=0, mt=0, maskt=0, shiftt=0, cs=0, ms=0, masks=0, shifts=0):
    return w((0x35 << 24) | (fmt << 21) | (size << 19) | (line << 9) | tmem,
             (tile << 24) | (pal << 20) | (ct << 19) | (mt << 18) | (maskt << 14) | (shiftt << 10) |
             (cs << 9) | (ms << 8) | (masks << 4) | shifts)


def set_tile_size(tile, sl, tl, sh, th):
    """coordinates in texels (converted to 10.2)"""
    return w((0x32 << 24) | (int(sl * 4) << 12) | int(tl * 4), (tile << 24) | (int(sh * 4) << 12) | int(th * 4))


def load_block(tile, sl, tl, sh, dxt):
    return w((0x33 << 24) | (sl << 12) | tl, (tile << 24) | (sh << 12) | dxt)


def load_tile(tile, sl, tl, sh, th):
    return w((0x34 << 24) | (int(sl * 4) << 12) | int(tl * 4), (tile << 24) | (int(sh * 4) << 12) | int(th * 4))


def load_tlut(tile, sl, tl, sh, th):
    return w((0x30 << 24) | (int(sl * 4) << 12) | int(tl * 4), (tile << 24) | (int(sh * 4) << 12) | int(th * 4))


def sync_full():
    return w(0x29 << 24, 0)


def sync_pipe():
    return w(0x27 << 24, 0)


# ---- primitives ---------------------------------------------------------------

def fill_rect(x0, y0, x1, y1):
    xl, yl, xh, yh = int(x1 * 4), int(y1 * 4), int(x0 * 4), int(y0 * 4)
    return w((0x36 << 24) | (xl << 12) | yl, (xh << 12) | yh)


def tex_rect(tile, x0, y0, x1, y1, s, t, dsdx, dtdy, flip=False):
    """s,t in texels; dsdx, dtdy in texels per pixel"""
    xl, yl, xh, yh = int(x1 * 4), int(y1 * 4), int(x0 * 4), int(y0 * 4)
    s5 = int(round(s * 32)) & 0xFFFF
    t5 = int(round(t * 32)) & 0xFFFF
    dsdx10 = int(round(dsdx * 1024)) & 0xFFFF
    dtdy10 = int(round(dtdy * 1024)) & 0xFFFF
    return w(((0x25 if flip else 0x24) << 24) | (xl << 12) | yl,
             (tile << 24) | (xh << 12) | yh, (s5 << 16) | t5, (dsdx10 << 16) | dtdy10)


def _split(v):
    v = int(round(v * 65536)) & 0xFFFFFFFF
    return (v >> 16) & 0xFFFF, v & 0xFFFF


def triangle(verts, shade=None, tex=None, zs=None, tile=0, level=0):
    """verts: [(x,y)]*3 in pixels. shade: [(r,g,b,a)]*3 (0..255). tex: [(s,t,w)]*3 (texels, w=1/w scale 0..1).
    zs: [z]*3 in 0..65535."""
    order = sorted(range(3), key=lambda i: verts[i][1])
    P = [verts[i] for i in order]
    (x1, y1), (x2, y2), (x3, y3) = P
    sh = [shade[i] for i in order] if shade else None
    tx = [tex[i] for i in order] if tex else None
    zz = [zs[i] for i in order] if zs else None

    hx, hy = x3 - x1, y3 - y1
    mx, my = x2 - x1, y2 - y1
    lx, ly = x3 - x2, y3 - y2
    nz = hx * my - hy * mx
    lft = 1 if nz < 0 else 0
    ish = hx / hy if abs(hy) > 1e-9 else 0.0
    ism = mx / my if abs(my) > 1e-9 else 0.0
    isl = lx / ly if abs(ly) > 1e-9 else 0.0
    fy = math.floor(y1) - y1
    xh = x1 + fy * ish
    xm = x1 + fy * ism
    ym_snap = math.floor(y2 * 4) / 4
    xl = x2 + (ym_snap - y2) * isl

    YL, YM, YH = int(math.floor(y3 * 4)), int(math.floor(y2 * 4)), int(math.floor(y1 * 4))
    cmd = 0x08
    words = []

    def fx(v, bits_frac=16):
        return int(round(v * (1 << bits_frac))) & 0xFFFFFFFF

    words += [(cmd << 24) | (lft << 23) | (level << 19) | (tile << 16) | (YL & 0x3FFF),
              ((YM & 0x3FFF) << 16) | (YH & 0x3FFF),
              fx(xl) & 0x0FFFFFFF, fx(isl) & 0x3FFFFFFF,
              fx(xh) & 0x0FFFFFFF, fx(ish) & 0x3FFFFFFF,
              fx(xm) & 0x0FFFFFFF, fx(ism) & 0x3FFFFFFF]
    det = mx * hy - my * hx

    def grads(a):
        if abs(det) < 1e-12:
            return 0.0, 0.0
        dadx = ((a[1] - a[0]) * hy - (a[2] - a[0]) * my) / det
        dady = (mx * (a[2] - a[0]) - hx * (a[1] - a[0])) / det
        return dadx, dady

    def attr_block(vals):
        """vals: list of 3-vertex arrays -> 8 words (int hi/lo pairs) laid out as the RDP wants
        for pairs of attributes. Returns (base, dx, de, dy) lists"""
        base, dx, de, dy = [], [], [], []
        for a in vals:
            dadx, dady = grads(a)
            dade = dady + dadx * ish
            b = a[0] + dadx * (xh - x1) + dady * fy
            base.append(b); dx.append(dadx); de.append(dade); dy.append(dady)
        return base, dx, de, dy

    def pack4(base, dx, de, dy):
        """4 attributes -> 16 words: base hi(2) dx hi(2) base lo(2) dx lo(2) de hi(2) dy hi(2) de lo(2) dy lo(2)"""
        out = []
        sp = lambda arr: [_split(v) for v in arr]
        B, X, E, Y = sp(base), sp(dx), sp(de), sp(dy)
        def pair_hi(S, i, j): return (S[i][0] << 16) | S[j][0]
        def pair_lo(S, i, j): return (S[i][1] << 16) | S[j][1]
        out += [pair_hi(B, 0, 1), pair_hi(B, 2, 3), pair_hi(X, 0, 1), pair_hi(X, 2, 3)]
        out += [pair_lo(B, 0, 1), pair_lo(B, 2, 3), pair_lo(X, 0, 1), pair_lo(X, 2, 3)]
        out += [pair_hi(E, 0, 1), pair_hi(E, 2, 3), pair_hi(Y, 0, 1), pair_hi(Y, 2, 3)]
        out += [pair_lo(E, 0, 1), pair_lo(E, 2, 3), pair_lo(Y, 0, 1), pair_lo(Y, 2, 3)]
        return out

    if sh:
        cmd |= 4
        vals = [[sh[i][c] for i in range(3)] for c in range(4)]
        words += pack4(*attr_block(vals))
    if tx:
        cmd |= 2
        vals = [[tx[i][0] * 32 for i in range(3)], [tx[i][1] * 32 for i in range(3)],
                [tx[i][2] * 32767 for i in range(3)], [0, 0, 0]]
        words += pack4(*attr_block(vals))
    if zs:
        cmd |= 1
        z = [zz[i] for i in range(3)]
        dzdx, dzdy = grads(z)
        dze = dzdy + dzdx * ish
        zb = z[0] + dzdx * (xh - x1) + dzdy * fy
        words += [fx(zb), fx(dzdx), fx(dze), fx(dzdy)]
    words[0] = (words[0] & 0x00FFFFFF) | (cmd << 24)
    return [x & 0xFFFFFFFF for x in words]


# ---- scene container ------------------------------------------------------------

class Scene:
    def __init__(self):
        self.cmds = []
        self.mem = bytearray(RDRAM_SIZE)  # host word order (see wr8)

    def add(self, *cmds):
        for c in cmds:
            self.cmds.append(c)

    def wr8(self, addr, v):
        self.mem[(addr ^ 3)] = v & 0xFF

    def wr16(self, addr, v):
        self.wr8(addr, v >> 8)
        self.wr8(addr + 1, v)

    def wr32(self, addr, v):
        for i in range(4):
            self.wr8(addr + i, v >> (24 - 8 * i))

    def write(self, path, vi):
        """vi = (status, origin, width, h_start, v_start, x_scale, y_scale)"""
        with open(path, 'wb') as f:
            f.write(b'FOIRDMP1')
            f.write(struct.pack('<I', RDRAM_SIZE))
            f.write(struct.pack('<7I', *vi))
            f.write(struct.pack('<II', len(self.cmds), 0))
            f.write(self.mem)
            for c in self.cmds:
                f.write(struct.pack('<I', len(c)))
                f.write(struct.pack('<%dI' % len(c), *c))


def vi_320x240(origin, bpp32=False):
    return (3 if bpp32 else 2, origin, 320, 0x006C02EC, 0x002E020E, 0x200, 0x400)
