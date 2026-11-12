#!/usr/bin/env python3
"""Offscreen lava-lamp preview matching the HLSL iso-surface and pinch-off sim.

Writes a bright still + 8-frame strip (0.5 s steps), and prints a 30 fps
consecutive-frame MAE check (must stay low / gooey, no boil).
"""
from __future__ import annotations

import math
import os
import struct
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
STILL = os.path.join(ROOT, "resources", "stalgia-lava.png")
STRIP = os.path.join(ROOT, "resources", "stalgia-lava-strip.png")

W, H = 960, 540
LAVA = (1.0, 0.416, 0.078)  # #FF6A14
LIQUID = (0.063, 0.031, 0.078)
GLOW = 1.4
BLOB_SIZE = 1.15
SPEED = 1.0
N_BLOBS = 11
ISO = 0.28

POOL, FORMING, FREE, QUEUED = 0, 1, 2, 3


def png_bytes(w: int, h: int, rgba: bytes) -> bytes:
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
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def clamp(v, lo=0.0, hi=1.0):
    return lo if v < lo else hi if v > hi else v


def lerp(a, b, t):
    return a + (b - a) * t


def hashu(x: int) -> int:
    x ^= x >> 16
    x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x


def rand01(s: list) -> float:
    s[0] = hashu(s[0])
    return (s[0] >> 8) / 16777215.0


class Blob:
    __slots__ = ("x", "y", "vx", "vy", "r", "temp", "phase", "timer", "state", "seed")

    def __init__(self):
        self.x = 0.5
        self.y = 0.08
        self.vx = 0.0
        self.vy = 0.0
        self.r = 0.06
        self.temp = 0.4
        self.phase = 0.0
        self.timer = 0.0
        self.state = QUEUED
        self.seed = 1


def spawn(b: Blob, index: int, size: float) -> None:
    s = [hashu((index + 1) * 0x9E3779B9 ^ 0xC2B2AE35)]
    b.seed = s[0]
    b.phase = rand01(s) * 6.2831853
    b.vx = 0.0
    b.vy = 0.0
    b.timer = 0.0
    if index == 0:
        b.state = POOL
        b.x = 0.50
        b.r = 0.165 * size
        b.y = b.r * 0.40
        b.temp = 0.95
        return
    if index == 1:
        b.state = FORMING
        b.x = 0.50 + 0.03 * math.sin(b.phase)
        b.r = 0.095 * size
        b.y = 0.14
        b.temp = 0.90
        b.vy = 0.010
        return
    u = rand01(s)
    if index >= 6 and u < 0.40:
        b.state = QUEUED
        b.x = 0.50
        b.y = 0.0
        b.r = 0.0
        b.temp = 0.70
        b.timer = 5.0 + rand01(s) * 14.0
        return
    b.state = FREE
    b.x = 0.18 + rand01(s) * 0.64
    b.y = 0.28 + rand01(s) * 0.58
    b.r = (0.058 + rand01(s) * 0.034) * size
    b.temp = 0.30 + rand01(s) * 0.42
    b.vy = (b.temp - 0.42) * 0.08


def gooey(speed: float) -> float:
    sp = clamp(speed, 0.1, 2.5)
    return 0.85 + 0.15 * sp


def step(blobs, dt: float, speed: float, size: float, sim_time: float) -> None:
    g = gooey(speed)
    n = len(blobs)
    if n <= 0:
        return
    two_pi = 6.2831853
    min_r = 0.050 * size
    max_r = 0.135 * size
    pool_target = 0.168 * size
    pool = blobs[0]
    pool.state = POOL
    pool.temp += (0.96 - pool.temp) * 0.07 * g * dt
    pool.temp = clamp(pool.temp, 0.78, 1.0)
    if pool.r < pool_target:
        pool.r += (pool_target - pool.r) * 0.035 * g * dt
    pool.r = clamp(pool.r, 0.110 * size, 0.210 * size)
    pool.y = pool.r * 0.40
    pool.vy = 0.0
    pool.vx = math.sin(sim_time * two_pi * 0.06 + pool.phase) * 0.006 * g
    pool.x += pool.vx * dt
    pool.x = clamp(pool.x, 0.28, 0.72)

    forming = sum(1 for b in blobs[1:] if b.state == FORMING)
    merge_left = 1 if any(
        b.state == FREE and b.y < pool.y + pool.r * 0.85 and b.vy <= 0.004 for b in blobs[1:]
    ) else 0

    for b in blobs[1:]:
        if b.state == QUEUED:
            b.r = 0.0
            b.vx = 0.0
            b.vy = 0.0
            b.x = pool.x
            b.y = pool.y
            b.timer -= dt
            if forming == 0 and b.timer <= 0.0 and pool.r > 0.125 * size:
                b.state = FORMING
                b.x = pool.x + 0.035 * math.sin(b.phase)
                b.r = pool.r * 0.52
                b.y = pool.y + pool.r * 0.22
                b.temp = 0.88
                b.vx = 0.0
                b.vy = 0.006 * g
                pool.r = max(0.115 * size, pool.r - b.r * 0.38)
                forming += 1
            continue

        if b.state == FORMING:
            target_r = (0.072 + 0.028 * b.temp) * size
            b.r += (target_r - b.r) * 0.055 * g * dt
            b.r = clamp(b.r, min_r, max_r)
            b.temp += (1.0 - b.temp) * 0.08 * g * dt
            b.vy += (0.016 * g - b.vy) * 0.9 * dt
            b.y += b.vy * dt
            lean = pool.x + 0.045 * math.sin(b.phase)
            b.x += (lean - b.x) * 0.35 * dt
            b.vx *= 0.90
            sep = b.y - pool.y
            pinch_at = (pool.r + b.r) * 0.90
            if sep > pinch_at and b.r > min_r:
                b.state = FREE
                b.vy = 0.014 * g
            continue

        b.temp += (0.26 - b.temp) * (0.045 + 0.070 * b.y) * g * dt
        b.temp = clamp(b.temp, 0.22, 1.0)
        joining = b.y < pool.y + pool.r * 0.70 and b.vy <= 0.004
        if not joining:
            b.r = clamp(b.r, min_r, max_r)
        else:
            b.r = min(b.r, max_r)
        target_vy = (b.temp - 0.43) * 0.095 * g
        if b.y > 0.80:
            b.temp += (0.22 - b.temp) * 0.09 * g * dt
            if b.temp < 0.38:
                target_vy = min(target_vy, -0.025 * g)
            if b.y > 0.93:
                b.y = 0.93
                if target_vy > 0:
                    target_vy = 0.0
        b.vy += (target_vy - b.vy) * 1.05 * dt
        hz = 0.07 + 0.03 * (b.phase * 0.159 - math.floor(b.phase * 0.159))
        wob = math.sin(sim_time * two_pi * hz + b.phase) * 0.0030 * g
        b.vx += (wob - b.vx) * 0.65 * dt
        b.x += b.vx * dt
        b.y += b.vy * dt
        if b.x < 0.10:
            b.x = 0.10
            b.vx = abs(b.vx) * 0.2
        if b.x > 0.90:
            b.x = 0.90
            b.vx = -abs(b.vx) * 0.2
        if merge_left > 0 and joining:
            merge_left -= 1
            xfer = min(b.r, 0.045 * size * dt)
            b.r -= xfer
            pool.r = min(0.210 * size, pool.r + xfer * 0.88)
            b.x += (pool.x - b.x) * 1.4 * dt
            b.y += (pool.y - b.y) * 1.2 * dt
            if b.r < min_r * 0.55:
                b.state = QUEUED
                b.r = 0.0
                b.timer = 6.0 + 10.0 * (b.phase * 0.37 - math.floor(b.phase * 0.37))
                b.x = pool.x
                b.y = pool.y
                b.vx = 0.0
                b.vy = 0.0


def simulate(seconds: float, speed=SPEED, size=BLOB_SIZE, count=N_BLOBS):
    blobs = [Blob() for _ in range(count)]
    for i, b in enumerate(blobs):
        spawn(b, i, size)
    t = 0.0
    ht = 1.0 / 60.0
    steps = int(seconds / ht + 0.5)
    for _ in range(steps):
        step(blobs, ht, speed, size, t)
        t += ht
    return blobs, t


def shader_pixel(ux, uy, blobs, glow):
    aspect = W / float(H)
    k = 0.0
    tsum = 0.0
    wsum = 0.0
    under = 0.0
    for b in blobs:
        if b.state == QUEUED or b.r < 0.008:
            continue
        px, py = b.x, 1.0 - b.y
        rad = max(b.r, 0.008)
        dx = (ux - px) * aspect
        dy = uy - py
        q = (dx * dx + dy * dy) / (rad * rad)
        s = clamp(1.0 - q)
        f = s * s * s
        k += f
        tsum += b.temp * f
        wsum += f
        under += clamp(dy / rad) * f
    temp = (tsum / wsum) if wsum > 1e-5 else 0.0
    u = (under / wsum) if wsum > 1e-5 else 0.0
    fw = 2.8 / H
    body = clamp((k - (ISO - fw)) / (2 * fw))
    body = body * body * (3 - 2 * body)
    bloom = clamp((k - ISO * 0.20) / max(ISO * 0.95, 1e-3))
    bloom = bloom * bloom * (1.0 - body)
    hot = (
        clamp(LAVA[0] * 1.18 + 0.20),
        clamp(LAVA[1] * 1.10 + 0.42),
        clamp(LAVA[2] * 0.28 + 0.05),
    )
    cool = (
        clamp(LAVA[0] * 0.82 + 0.30),
        clamp(LAVA[1] * 0.18 + 0.08),
        clamp(LAVA[2] * 0.70 + 0.36),
    )
    lum_w = (0.30, 0.59, 0.11)
    hot_lum = max(hot[0] * lum_w[0] + hot[1] * lum_w[1] + hot[2] * lum_w[2], 1e-3)
    cool_lum = max(cool[0] * lum_w[0] + cool[1] * lum_w[1] + cool[2] * lum_w[2], 1e-3)
    scale = (hot_lum * 0.60) / cool_lum
    cool = (clamp(cool[0] * scale), clamp(cool[1] * scale), clamp(cool[2] * scale))
    tt = clamp((temp - 0.16) / 0.69)
    tt = tt * tt * (3 - 2 * tt)
    wax = (lerp(cool[0], hot[0], tt), lerp(cool[1], hot[1], tt), lerp(cool[2], hot[2], tt))
    core = clamp((k - ISO) * 2.0)
    lift = lerp(0.92, 1.20, clamp(u))
    mul = lerp(0.96, 1.12, core) * lift
    lit = [max(wax[i] * mul, wax[i] * 0.90) for i in range(3)]
    rim = (1.0 - core) * body * 0.22
    lit[0] += 1.00 * rim
    lit[1] += 0.88 * rim
    lit[2] += 0.48 * rim
    bottom = uy
    band = bottom ** 11.0
    fill = (bottom ** 4.2) * 0.30
    lp_x = (ux - 0.5) * aspect * 0.58
    lp_y = bottom - 1.03
    radial = math.exp(-(lp_x * lp_x + lp_y * lp_y) * 13.5)
    heat = (radial * 1.40 + band * 1.20 + fill) * glow
    heater = [
        (LAVA[0] * 1.32 + 0.42) * heat,
        (LAVA[1] * 0.82 + 0.16) * heat,
        (LAVA[2] * 0.10) * heat,
    ]
    heater = [heater[i] / (1.0 + heater[i] * 0.28) for i in range(3)]
    liquid = [
        clamp(LIQUID[0] * 0.22 + heater[0] * 0.92),
        clamp(LIQUID[1] * 0.22 + heater[1] * 0.92),
        clamp(LIQUID[2] * 0.22 + heater[2] * 0.92),
    ]
    col = [
        lerp(liquid[0], lit[0], body) + wax[0] * bloom * (0.18 + 0.42 * glow) + heater[0] * 0.10 * (1 - body),
        lerp(liquid[1], lit[1], body) + wax[1] * bloom * (0.18 + 0.42 * glow) + heater[1] * 0.10 * (1 - body),
        lerp(liquid[2], lit[2], body) + wax[2] * bloom * (0.18 + 0.42 * glow) + heater[2] * 0.10 * (1 - body),
    ]
    return (
        int(clamp(col[0]) * 255),
        int(clamp(col[1]) * 255),
        int(clamp(col[2]) * 255),
        255,
    )


def render(blobs, glow=GLOW) -> bytes:
    pix = bytearray(W * H * 4)
    for y in range(H):
        uy = (y + 0.5) / H
        for x in range(W):
            ux = (x + 0.5) / W
            r, g, b, a = shader_pixel(ux, uy, blobs, glow)
            o = (y * W + x) * 4
            pix[o] = r
            pix[o + 1] = g
            pix[o + 2] = b
            pix[o + 3] = a
    return bytes(pix)


def mae(a: bytes, b: bytes) -> float:
    s = 0
    n = min(len(a), len(b))
    for i in range(0, n, 4):
        s += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])
    return s / (3.0 * (n // 4) * 255.0)


def hstack(frames):
    n = len(frames)
    sw, sh = W // 4, H // 4
    out_w = sw * n
    out = bytearray(out_w * sh * 4)
    for i, fr in enumerate(frames):
        for y in range(sh):
            src_y = min(H - 1, y * 4 + 2)
            for x in range(sw):
                src_x = min(W - 1, x * 4 + 2)
                s = (src_y * W + src_x) * 4
                d = (y * out_w + i * sw + x) * 4
                out[d : d + 4] = fr[s : s + 4]
    return bytes(out), out_w, sh


def vis_count(blobs):
    return sum(1 for b in blobs if b.state != QUEUED and b.r >= 0.008)


def tiny_count(blobs):
    return sum(1 for b in blobs if b.state != QUEUED and 0.008 <= b.r < 0.035)


def continuing_sim(count, speed, size, warmup_s):
    blobs = [Blob() for _ in range(count)]
    for i, b in enumerate(blobs):
        spawn(b, i, size)
    t = 0.0
    ht = 1.0 / 60.0
    for _ in range(int(warmup_s / ht + 0.5)):
        step(blobs, ht, speed, size, t)
        t += ht
    return blobs, t, ht


def main():
    os.makedirs(os.path.dirname(STILL), exist_ok=True)

    still_blobs, _ = simulate(7.0, speed=2.15, size=1.14, count=13)
    still = render(still_blobs, glow=0.55)
    with open(STILL, "wb") as f:
        f.write(png_bytes(W, H, still))

    frames = []
    blobs, t, ht = continuing_sim(N_BLOBS, SPEED, BLOB_SIZE, 4.0)
    for i in range(8):
        if i:
            for _ in range(int(0.5 / ht + 0.5)):
                step(blobs, ht, SPEED, BLOB_SIZE, t)
                t += ht
        frames.append(render(blobs, glow=GLOW))
    strip, sw, sh = hstack(frames)
    with open(STRIP, "wb") as f:
        f.write(png_bytes(sw, sh, strip))

    blobs, t, ht = continuing_sim(N_BLOBS, SPEED, BLOB_SIZE, 5.0)
    a = render(blobs, glow=GLOW)
    for _ in range(2):
        step(blobs, ht, SPEED, BLOB_SIZE, t)
        t += ht
    b = render(blobs, glow=GLOW)
    d = mae(a, b)

    blobs, t, ht = continuing_sim(13, 2.15, 1.14, 5.0)
    a2 = render(blobs, glow=0.55)
    for _ in range(2):
        step(blobs, ht, 2.15, 1.14, t)
        t += ht
    b2 = render(blobs, glow=0.55)
    d2 = mae(a2, b2)

    # Max consecutive MAE across 8 frames at 30 fps from a live sim (user settings).
    blobs, t, ht = continuing_sim(13, 2.15, 1.14, 6.0)
    prev = render(blobs, glow=0.55)
    max_d = 0.0
    tinies = 0
    for _ in range(8):
        for __ in range(2):
            step(blobs, ht, 2.15, 1.14, t)
            t += ht
        cur = render(blobs, glow=0.55)
        max_d = max(max_d, mae(prev, cur))
        tinies = max(tinies, tiny_count(blobs))
        prev = cur

    print(f"wrote {STILL} ({os.path.getsize(STILL)} bytes)")
    print(f"wrote {STRIP} ({os.path.getsize(STRIP)} bytes)")
    print(f"visible blobs in still: {vis_count(still_blobs)}  tinies: {tiny_count(still_blobs)}")
    print(f"30fps consecutive MAE speed=1.0 glow=1.4: {d:.5f}")
    print(f"30fps consecutive MAE speed=2.15 glow=0.55: {d2:.5f}")
    print(f"max 8-frame 30fps MAE (user cfg): {max_d:.5f}  max tinies: {tinies}")
    ok = d < 0.020 and d2 < 0.020 and max_d < 0.020 and tinies == 0
    print("diff check:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
