#!/usr/bin/env python3
"""1920x1200 stills at 1 s and 10 s plus a 4-frame strip of live Code Ape rain.

Simulates src/effects/CodeApe.cpp: dense wrapping rain, luma-modulated trails,
no lock-in / draw cycle.

    python3 tools/render_ape_preview.py
"""
from __future__ import annotations

import math
import os
import struct
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MASK_PNG = os.path.join(ROOT, "resources", "ape-mask.png")
STILL_1S = os.path.join(ROOT, "resources", "stalgia-ape-1s.png")
STILL = os.path.join(ROOT, "resources", "stalgia-ape.png")
STRIP = os.path.join(ROOT, "resources", "stalgia-ape-strip.png")

W, H = 1920, 1200
CELL = 9
COLOR = (0.949, 0.965, 1.0)  # #F2F6FF
HEAD = (0.987, 0.991, 1.0)
BG_DIM = 1.00
APE_BRI = 1.70
SIZE = 0.85
POS_X = 0.50
SPEED = 1.0
BREATHE_ON = True
OUTSIDE_DIM = 0.70
STRIP_TIMES = (1.0, 1.5, 2.0, 2.5)
STILL_TIMES = (1.0, 10.0)


def png_rgba(w, h, rgba: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    raw = b"".join(b"\x00" + rgba[y * w * 4 : (y + 1) * w * 4] for y in range(h))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )


def read_png_gray(path: str):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    w = h = 0
    col = 0
    idat = b""
    while pos < len(data):
        ln = struct.unpack(">I", data[pos : pos + 4])[0]
        tag = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + ln]
        pos += 12 + ln
        if tag == b"IHDR":
            w, h, _bitd, col, *_ = struct.unpack(">IIBBBBB", chunk)
        elif tag == b"IDAT":
            idat += chunk
        elif tag == b"IEND":
            break
    raw = zlib.decompress(idat)
    bpp = 1 if col == 0 else (3 if col == 2 else 4)
    stride = 1 + w * bpp
    out = bytearray(w * h)
    for y in range(h):
        row = raw[y * stride + 1 : (y + 1) * stride]
        if col == 0:
            out[y * w : (y + 1) * w] = row
        else:
            for x in range(w):
                r, g, b = row[x * bpp], row[x * bpp + 1], row[x * bpp + 2]
                out[y * w + x] = int(0.2126 * r + 0.7152 * g + 0.0722 * b + 0.5)
    return w, h, bytes(out)


def clamp(v, lo=0.0, hi=1.0):
    return lo if v < lo else hi if v > hi else v


def lerp(a, b, t):
    return a + (b - a) * t


def hashu(x: int) -> int:
    x &= 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x


def rand01(s: list) -> float:
    s[0] = hashu(s[0])
    return (s[0] >> 8) / 16777215.0


def _mirror5(rows):
    out = []
    for b in rows:
        m = 0
        for bit in range(5):
            if b & (1 << bit):
                m |= 1 << (4 - bit)
        out.append(m)
    return tuple(out)


# 5×7 katakana-ish + digits (not Latin A–Z). Extra entries are mirrored kana.
_KANA = [
    (0x1F, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x04),
    (0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A, 0x11),
    (0x1C, 0x04, 0x08, 0x10, 0x10, 0x11, 0x0E),
    (0x1F, 0x04, 0x08, 0x14, 0x04, 0x04, 0x04),
    (0x1F, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1F),
    (0x15, 0x15, 0x1F, 0x04, 0x04, 0x04, 0x04),
    (0x01, 0x02, 0x04, 0x00, 0x10, 0x08, 0x07),
    (0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F),
    (0x1F, 0x04, 0x1F, 0x04, 0x04, 0x04, 0x18),
    (0x10, 0x08, 0x04, 0x00, 0x02, 0x01, 0x01),
    (0x1F, 0x04, 0x1F, 0x15, 0x04, 0x04, 0x04),
    (0x04, 0x1F, 0x04, 0x0E, 0x00, 0x02, 0x01),
    (0x11, 0x0A, 0x00, 0x00, 0x10, 0x08, 0x07),
    (0x1F, 0x04, 0x04, 0x0E, 0x04, 0x04, 0x04),
    (0x04, 0x04, 0x04, 0x04, 0x14, 0x0C, 0x04),
    (0x04, 0x1F, 0x04, 0x04, 0x0A, 0x11, 0x00),
    (0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F),
    (0x0E, 0x11, 0x0E, 0x04, 0x0A, 0x11, 0x00),
    (0x04, 0x1F, 0x04, 0x0E, 0x15, 0x04, 0x08),
    (0x01, 0x02, 0x04, 0x04, 0x08, 0x10, 0x00),
    (0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04, 0x04),
    (0x10, 0x10, 0x1E, 0x10, 0x10, 0x11, 0x0E),
    (0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08),
    (0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00),
    (0x04, 0x1F, 0x04, 0x0E, 0x15, 0x04, 0x04),
    (0x1F, 0x04, 0x1F, 0x04, 0x04, 0x04, 0x03),
    (0x1C, 0x00, 0x0E, 0x00, 0x07, 0x00, 0x00),
    (0x0E, 0x10, 0x10, 0x10, 0x11, 0x0A, 0x04),
    (0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00, 0x00),
    (0x1F, 0x04, 0x1F, 0x04, 0x04, 0x04, 0x1F),
    (0x0E, 0x11, 0x1F, 0x04, 0x04, 0x04, 0x04),
    (0x1E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x1F),
    (0x1F, 0x01, 0x1F, 0x01, 0x01, 0x01, 0x1F),
    (0x1F, 0x04, 0x08, 0x10, 0x0E, 0x01, 0x06),
    (0x12, 0x12, 0x12, 0x12, 0x12, 0x11, 0x0E),
    (0x12, 0x12, 0x12, 0x1A, 0x16, 0x12, 0x11),
    (0x10, 0x10, 0x10, 0x10, 0x10, 0x11, 0x0E),
    (0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F),
    (0x1F, 0x10, 0x10, 0x10, 0x11, 0x0A, 0x04),
    (0x10, 0x08, 0x00, 0x00, 0x02, 0x01, 0x00),
]
_DIGITS = [
    (0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E),
    (0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E),
    (0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F),
    (0x1F, 0x02, 0x04, 0x0E, 0x01, 0x11, 0x0E),
    (0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02),
    (0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E),
    (0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E),
    (0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08),
    (0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E),
    (0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C),
]
GLYPHS = list(_KANA) + list(_DIGITS) + [_mirror5(g) for g in _KANA[:20]]


def trail_bri(dist, trail, inside=False):
    if inside:
        if dist < 0.35:
            return 1.0
        if dist < 8.0:
            return lerp(0.82, 0.68, clamp((dist - 0.35) / 7.65))
        tail = clamp((dist - 8.0) / max(1.0, trail - 8.0))
        return 0.68 * ((1.0 - tail) ** 0.72)
    if dist < 0.35:
        return 1.0
    if dist < 5.5:
        return lerp(0.70, 0.60, clamp((dist - 0.35) / 5.15))
    tail = clamp((dist - 5.5) / max(1.0, trail - 5.5))
    return 0.60 * ((1.0 - tail) ** 1.05)


def shade_luma(luma):
    x = clamp(luma)
    x = x ** 0.62
    x = clamp((x - 0.06) / 0.86)
    x = clamp((x - 0.38) * 1.50 + 0.38)
    return x


MASK_W = MASK_H = 0
MASK = b""


def load_mask():
    global MASK_W, MASK_H, MASK
    MASK_W, MASK_H, MASK = read_png_gray(MASK_PNG)


def sample_luma(x, y):
    ape_h = SIZE * H
    ape_w = ape_h * (MASK_W / float(MASK_H))
    cx = POS_X * W
    cy = H * 0.50
    u = (x - (cx - ape_w * 0.5)) / ape_w
    v = (y - (cy - ape_h * 0.5)) / ape_h
    if u <= 0 or v <= 0 or u >= 1 or v >= 1:
        return 0.0
    fx = u * (MASK_W - 1)
    fy = v * (MASK_H - 1)
    x0 = int(fx)
    y0 = int(fy)
    x1 = min(x0 + 1, MASK_W - 1)
    y1 = min(y0 + 1, MASK_H - 1)
    tx = fx - x0
    ty = fy - y0

    def pix(ix, iy):
        return MASK[iy * MASK_W + ix] / 255.0

    return lerp(lerp(pix(x0, y0), pix(x1, y0), tx), lerp(pix(x0, y1), pix(x1, y1), tx), ty)


def put_px(buf, x, y, r, g, b, a=1.0):
    if x < 0 or y < 0 or x >= W or y >= H:
        return
    o = (y * W + x) * 4
    src_a = a
    dr, dg, db = buf[o] / 255.0, buf[o + 1] / 255.0, buf[o + 2] / 255.0
    buf[o] = int(clamp(r * src_a + dr * (1 - src_a)) * 255)
    buf[o + 1] = int(clamp(g * src_a + dg * (1 - src_a)) * 255)
    buf[o + 2] = int(clamp(b * src_a + db * (1 - src_a)) * 255)
    buf[o + 3] = 255


def stamp(buf, x0, y0, gi, r, g, b, a, cell):
    bits = GLYPHS[gi % len(GLYPHS)]
    sx = cell / 7.0
    fill = 2 if cell >= 8 else 1
    for row, rowbits in enumerate(bits):
        for col in range(5):
            if rowbits & (1 << (4 - col)):
                px = int(round(x0 + (col + 0.7) * sx))
                py = int(round(y0 + row * sx))
                for oy in range(fill):
                    for ox in range(fill):
                        put_px(buf, px + ox, py + oy, r, g, b, a)


class Sim:
    def __init__(self):
        self.rows = H // CELL + 2
        self.ncols = W // CELL
        self.t = 0.0
        dual_prob = clamp(0.18 + 0.32 * BG_DIM, 0.20, 0.55)
        self.cols = []
        for i in range(self.ncols):
            seed = hashu((i + 1) * 0x9E3779B9 ^ 0xC0DEA9E0)
            ds = [seed ^ 0xA53C9E4B]
            n = 2 if rand01(ds) < dual_prob else 1
            col = {"seed": seed, "n": n, "s": []}
            for which in range(n):
                st = {"seed": seed, "gen": 0, "head": 0.0, "speed": 12.0, "trail": 16.0, "delay": 0.0}
                self.reset_stream(col, st, which, True)
                col["s"].append(st)
            self.cols.append(col)

    def wrap_dist(self, head, row):
        period = float(max(1, self.rows))
        dist = head - row
        dist -= period * math.floor(dist / period)
        return dist

    def reset_stream(self, col, st, which, spawn_on):
        s = [col["seed"] ^ (st["gen"] * 0x85EBCA6B) ^ (which * 0x27D4EB2D)]
        st["speed"] = (10.0 + rand01(s) * 16.0) * SPEED
        st["trail"] = self.rows * (0.84 + rand01(s) * 0.13)
        st["gen"] += 1
        st["seed"] = hashu(s[0] ^ 0xA5A5A5A5)
        if spawn_on:
            st["head"] = rand01(s) * self.rows
            if which == 1:
                st["head"] = (st["head"] + self.rows * 0.47) % self.rows
            st["delay"] = 0.0
        else:
            st["head"] = -0.5
            st["delay"] = 0.0

    def step(self, dt):
        self.t += dt
        period = float(max(1, self.rows))
        for c in self.cols:
            for st in c["s"]:
                if st["delay"] > 0:
                    st["delay"] -= dt
                    continue
                st["head"] += st["speed"] * dt
                if st["head"] >= period:
                    st["head"] -= period * math.floor(st["head"] / period)

    def run_to(self, time, ht=1.0 / 30.0):
        while self.t + 1e-6 < time:
            self.step(ht)
        return self.t

    def render(self, time):
        buf = bytearray(W * H * 4)
        for i in range(W * H):
            buf[i * 4 + 3] = 255
        body_salt = int(round(time * 0.22))
        head_salt = int(round(time * 0.70))
        breathe = (1.0 + 0.03 * math.sin(time * 6.2831853 / 5.0)) if BREATHE_ON else 1.0
        rain_scale = clamp(BG_DIM, 0.35, 1.20)
        ape_scale = 0.72 + 0.18 * APE_BRI
        n_cells = n_lit = n_inside = n_in_trail = 0
        bri_out = bri_in = 0.0
        n_out = n_in = 0
        for i, c in enumerate(self.cols):
            x = i * CELL
            cx = x + CELL * 0.5
            for row in range(-1, self.rows + 1):
                y = row * CELL
                if y + CELL < 0 or y > H:
                    continue
                n_cells += 1
                cy = y + CELL * 0.5
                luma = sample_luma(cx, cy)
                inside = luma > 0.02
                if inside:
                    n_inside += 1
                rain_fade = 0.0
                k_head = 999
                stream_seed = c["s"][0]["seed"]
                stream_gen = c["s"][0]["gen"]
                any_trail = False
                for st in c["s"]:
                    if st["delay"] > 0:
                        continue
                    dist = self.wrap_dist(st["head"], row)
                    trail_cells = max(8, int(math.ceil(st["trail"])))
                    if dist > trail_cells + 0.5:
                        continue
                    any_trail = True
                    bri = trail_bri(dist, st["trail"], inside)
                    if bri >= rain_fade:
                        rain_fade = bri
                        k_head = int(round(max(0.0, dist)))
                        stream_seed = st["seed"]
                        stream_gen = st["gen"]
                if not any_trail:
                    continue
                glowing = k_head == 0
                shade = shade_luma(luma) if inside else 0.0
                if inside:
                    n_in_trail += 1
                    mod = lerp(0.32, 1.28, shade) * ape_scale * breathe
                    if glowing:
                        bri = lerp(0.78, 1.0, shade)
                    else:
                        bri = rain_fade * mod
                    n_in += 1
                    bri_in += bri
                else:
                    bri = 1.0 if glowing else rain_fade * rain_scale * OUTSIDE_DIM
                    n_out += 1
                    bri_out += bri
                if bri >= 0.018:
                    n_lit += 1
                if bri < 0.018:
                    continue
                h = hashu(stream_seed ^ (row * 0x9E3779B9) ^ (stream_gen * 7))
                salt = head_salt if glowing else body_salt
                if (hashu(h ^ salt) & 0xFFFF) < 2200:
                    h ^= salt * 0x45D9F3B
                gi = h % len(GLYPHS)
                if glowing and ((not inside) or shade > 0.35):
                    r, g, b = HEAD
                    a = 1.0
                else:
                    r, g, b = COLOR[0] * clamp(bri), COLOR[1] * clamp(bri), COLOR[2] * clamp(bri)
                    a = clamp(bri * 1.08)
                stamp(buf, x, y, gi, r, g, b, a, CELL)
        print(
            f"t={time:.1f}s occupancy={n_lit / max(1, n_cells):.2%} "
            f"ape_rain={n_in_trail / max(1, n_inside):.2%} "
            f"mean_out={bri_out / max(1, n_out):.2f} mean_in={bri_in / max(1, n_in):.2f}"
        )
        return bytes(buf)


def main():
    load_mask()
    sim = Sim()
    stills = {}
    frames = []
    checkpoints = sorted(set(list(STILL_TIMES) + list(STRIP_TIMES)))
    for target in checkpoints:
        sim.run_to(target)
        img = sim.render(target)
        if target in STILL_TIMES:
            stills[target] = img
        if target in STRIP_TIMES:
            frames.append(img)

    with open(STILL_1S, "wb") as f:
        f.write(png_rgba(W, H, stills[1.0]))
    print(f"wrote {STILL_1S} ({os.path.getsize(STILL_1S)} bytes)")
    with open(STILL, "wb") as f:
        f.write(png_rgba(W, H, stills[10.0]))
    print(f"wrote {STILL} ({os.path.getsize(STILL)} bytes)")

    fw, fh = W // 2, H // 2
    gap = 8
    n = len(frames)
    sw = fw * n + gap * (n - 1)
    sh = fh
    strip = bytearray(sw * sh * 4)
    for i in range(sw * sh):
        strip[i * 4 + 3] = 255

    def down(src, dw, dh):
        out = bytearray(dw * dh * 4)
        for y in range(dh):
            sy = min(H - 1, y * H // dh)
            for x in range(dw):
                sx = min(W - 1, x * W // dw)
                si = (sy * W + sx) * 4
                di = (y * dw + x) * 4
                out[di : di + 4] = src[si : si + 4]
        return out

    for fi, fr in enumerate(frames):
        small = down(fr, fw, fh)
        ox = fi * (fw + gap)
        for y in range(fh):
            src = y * fw * 4
            dst = (y * sw + ox) * 4
            strip[dst : dst + fw * 4] = small[src : src + fw * 4]
    with open(STRIP, "wb") as f:
        f.write(png_rgba(sw, sh, bytes(strip)))
    print(f"wrote {STRIP} ({os.path.getsize(STRIP)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
