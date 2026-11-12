#!/usr/bin/env python3
"""CPU reference for the Aurora, Stars, Fireflies, and Snow wallpapers.

Matches PSAurora / PSStarfield. Noise is the same 256^2 R8 lattice the GPU
samples with one Hermite-adjusted bilinear fetch. The grade pass saturates
before the 2.2 encode, and a night preset uses 0.85 (the most nightAmount
ever applies). Effects tonemap with 1 - exp(-1.1 c) before the grade.
"""

import argparse
import os

import numpy as np
from PIL import Image

W0, H0 = 1920, 1080


def frac(x):
    return x - np.floor(x)


def lerp(a, b, t):
    return a + (b - a) * t


def smoothstep(e0, e1, x):
    denom = e1 - e0
    denom = np.where(np.abs(denom) < 1e-6, np.sign(denom + 1e-8) * 1e-6, denom)
    t = np.clip((x - e0) / denom, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def saturate(x):
    return np.clip(x, 0.0, 1.0)


def hash12(x, y):
    p3x = frac(x * 0.1031)
    p3y = frac(y * 0.1031)
    p3z = frac(x * 0.1031)
    d = p3x * (p3y + 33.33) + p3y * (p3z + 33.33) + p3z * (p3x + 33.33)
    p3x = p3x + d
    p3y = p3y + d
    p3z = p3z + d
    return frac((p3x + p3y) * p3z)


def hash22(x, y):
    return hash12(x, y), hash12(x + 19.1, y + 7.7)


_ix = np.arange(256, dtype=np.float32)
_gx, _gy = np.meshgrid(_ix, _ix, indexing="xy")
_NOISE = np.round(hash12(_gx, _gy) * 255.0).astype(np.float32) / 255.0


def vnoise(x, y):
    """One bilinear fetch of the Hermite-adjusted lattice. Matches the GPU."""
    ix = np.floor(x)
    iy = np.floor(y)
    fx = x - ix
    fy = y - iy
    fx = fx * fx * (3.0 - 2.0 * fx)
    fy = fy * fy * (3.0 - 2.0 * fy)
    ixm = np.mod(ix, 256).astype(np.int32)
    iym = np.mod(iy, 256).astype(np.int32)
    ix1 = (ixm + 1) & 255
    iy1 = (iym + 1) & 255
    a = _NOISE[iym, ixm]
    b = _NOISE[iym, ix1]
    c = _NOISE[iy1, ixm]
    d = _NOISE[iy1, ix1]
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy)


def fbm(x, y, octaves):
    s = np.zeros_like(np.asarray(x, dtype=np.float32), dtype=np.float32)
    a = 0.5
    px = np.asarray(x, dtype=np.float32)
    py = np.asarray(y, dtype=np.float32)
    for _ in range(octaves):
        s = s + a * vnoise(px, py)
        px = px * 2.03 + 1.7
        py = py * 2.03 + 9.2
        a *= 0.5
    return s


def fbm2(x, y):
    return fbm(x, y, 2)


def fbm3(x, y):
    return fbm(x, y, 3)


def vnoise1(x):
    return vnoise(x, np.full_like(np.asarray(x, dtype=np.float32), 0.37))


def ridged(x, freq, seed):
    n = vnoise(x * freq, np.full_like(np.asarray(x, dtype=np.float32), seed))
    r = 1.0 - np.abs(n * 2.0 - 1.0)
    return r * r


def star_color(u):
    warm = np.array([1.0, 0.58, 0.32], dtype=np.float32)
    white = np.array([0.94, 0.96, 1.0], dtype=np.float32)
    blue = np.array([0.62, 0.76, 1.0], dtype=np.float32)
    c = lerp(warm[:, None, None], white[:, None, None], smoothstep(0.0, 0.42, u))
    return lerp(c, blue[:, None, None], smoothstep(0.42, 1.0, u))


def hex_rgb(v):
    return np.array([(v >> 16) & 255, (v >> 8) & 255, v & 255], dtype=np.float32) / 255.0


def tonemap(col):
    return 1.0 - np.exp(-1.1 * np.maximum(col, 0.0))


def grade(col, uv, width, height, look, quantize=False):
    bright, sat, vig, night = look
    # toLin saturates first. Night presets use 0.85, the cap of nightAmount().
    # quantize matches the R16G16B16A16 scene target, which drops sub-normal starlight.
    lin = np.power(np.clip(col, 0.0, 1.0), 2.2)
    if quantize:
        lin = lin.astype(np.float16).astype(np.float32)
    l = lin[0] * 0.2126 + lin[1] * 0.7152 + lin[2] * 0.0722
    lin = lerp(l, lin, sat)
    lin = lin * bright
    night_amt = 0.85 if night else 0.0
    night_tint = lin * np.array([0.62, 0.72, 0.95], dtype=np.float32)[:, None, None] * 0.78
    lin = lerp(lin, night_tint, night_amt)
    aspect = width / max(height, 1.0)
    px = (uv[0] - 0.5) * aspect
    py = uv[1] - 0.5
    v = smoothstep(1.05, 0.22, np.sqrt(px * px + py * py))
    lin = lin * lerp(1.0, v, vig)
    enc = np.power(np.maximum(lin, 0.0), 1.0 / 2.2)
    ys, xs = np.indices((enc.shape[1], enc.shape[2]), dtype=np.float32)
    n = frac(52.9829189 * frac(xs * 0.06711056 + ys * 0.00583715))
    enc = saturate(enc + (n - 0.5) * (1.0 / 255.0))
    return enc


def add_stars(col, p, height, time, scroll, travel, fwd, twinkle, density, tint, layers, look, field=False):
    """Single cell per pixel. Jitter stays inside the cell so the halo is not clipped."""
    lx, ly = look
    px_per_base = height
    curve = 1.7 if field else 6.0
    r0, r1 = (1.05, 1.75) if field else (0.45, 1.1)
    sharp = 0.62 if field else 0.42
    jit = 0.28 if field else 0.36
    halo_cut = 0.82 if field else 0.98
    halo_sig = 10.0 if field else 12.0
    halo_amp = 0.55 if field else 0.80
    gain0 = 0.34 if field else 0.35
    gain1 = 2.15 if field else 1.15
    for scale, depth, keep in layers:
        depth_mix = float(lerp(0.35, 1.0, depth))
        off = (fwd * (scroll * depth_mix * travel))[:, None, None]
        look_off = np.array([lx, -ly], dtype=np.float32)[:, None, None] * (0.42 * depth)
        q = (p + off + look_off) * scale
        cx = np.floor(q[0])
        cy = np.floor(q[1])
        fx = frac(q[0]) - 0.5
        fy = frac(q[1]) - 0.5
        n = hash12(cx + 3.1, cy + depth * 9.0)
        gate = 1.0 - keep
        alive = n > gate
        t = saturate((n - gate) / max(keep, 1e-3))
        bright = np.power(t, curve)
        jx, jy = hash22(cx + depth * 17.0, cy + 4.0)
        dx = fx - (jx - 0.5) * jit
        dy = fy - (jy - 0.5) * jit
        dist = np.sqrt(dx * dx + dy * dy) * (px_per_base / scale)
        core_r = np.maximum(lerp(r0, r1, bright), 0.2)
        core = np.exp(-(dist * dist) / (core_r * core_r * sharp))
        halo = np.where(t > halo_cut, np.exp(-(dist * dist) / halo_sig) * halo_amp, 0.0)
        u = np.power(hash12(cx + 9.0, cy + 2.2), 0.55)
        bb = star_color(u)
        tw = 0.90 + 0.10 * np.sin(time * (0.45 + n * 1.4) + n * 42.0)
        amp = lerp(1.0, tw, twinkle)
        inten = (core * (gain0 + gain1 * bright) + halo) * amp * density
        inten = np.where(alive, inten, 0.0)
        shade = (0.55 + 0.45 * depth) * inten
        for c in range(3):
            col[c] += bb[c] * shade * float(tint[c])


def add_radial(col, p, height, time, speed, travel, density, tint):
    """Stars laid out in polar cells so the streak is one cell axis and grows with r."""
    r = np.maximum(np.hypot(p[0], p[1]), 1e-3)
    ang = np.arctan2(p[1], p[0])
    n_ang = 90.0
    n_rad = 18.0
    u = ang / (2.0 * np.pi) * n_ang
    v = np.log(r) * n_rad - time * speed * 1.4
    cx = np.floor(u)
    cy = np.floor(v)
    fx = frac(u)
    fy = frac(v)
    n = hash12(cx + 1.7, cy + 4.0)
    keep = np.clip(0.45 * density, 0.05, 0.9)
    alive = n > (1.0 - keep)
    t = saturate((n - (1.0 - keep)) / keep)
    bright = np.power(t, 4.0)
    side = fx - (0.5 + (hash12(cx, cy + 8.0) - 0.5) * 0.25)
    along = fy - 0.72
    dtheta = side * (2.0 * np.pi / n_ang)
    side_px = np.abs(dtheta) * r * height
    length = lerp(0.15, 0.85, travel)
    streak = np.exp(-(side_px * side_px) / (1.15 * 1.15))
    streak = streak * np.exp(-np.maximum(-along, 0.0) / length)
    streak = streak * smoothstep(0.14, 0.0, np.maximum(along, 0.0))
    u_col = np.power(hash12(cx + 3.0, cy), 0.55)
    bb = star_color(u_col)
    # Slight blue shift on the head.
    head = smoothstep(0.14, 0.0, np.maximum(along, 0.0))
    bb = lerp(bb, bb * np.array([0.75, 0.88, 1.15], dtype=np.float32)[:, None, None], head * 0.45)
    inten = np.where(alive, streak * (0.55 + 2.4 * bright) * density, 0.0)
    for c in range(3):
        col[c] += bb[c] * inten * float(tint[c])


def add_haze(col, pixel_x, pixel_y, band, dust, band_col, gain=1.0):
    """Unresolved 1px stars. One hash, no neighbour search."""
    gx = np.floor(pixel_x * 0.75)
    gy = np.floor(pixel_y * 0.75)
    h1 = hash12(gx, gy)
    h2 = hash12(gx + 19.0, gy + 3.0)
    on = h1 > (1.0 - 0.38 * band)
    mag = np.power(h2, 2.4) * 0.28
    mag = np.where(on, mag, 0.0) * (1.0 - dust) * gain
    for c in range(3):
        col[c] += mag * band_col[c]


def milky(p, uv):
    """Diagonal band. The silhouette is a wide gaussian with no distance cutoff."""
    diag = p[1] * 0.95 + p[0] * 0.78
    along = p[0] * 0.95 - p[1] * 0.78
    sky_keep = 0.12 + 0.32 * np.exp(-(diag * diag) * 3.5)
    env = np.exp(-(diag * diag) / (2.0 * 0.30 * 0.30))
    core = np.exp(-((along - 0.15) ** 2) / 0.28)
    return diag, along, core, env, sky_keep


def paint_band(col, p, uv, pixel_x, pixel_y, band_scale):
    diag, along, core, env, sky_keep = milky(p, uv)
    arm_col = np.array([0.55, 0.66, 0.98], dtype=np.float32)
    core_col = np.array([0.92, 0.68, 0.40], dtype=np.float32)
    glow_col = lerp(arm_col[:, None, None], core_col[:, None, None], core)
    glow = np.zeros_like(col)
    base = (0.18 + 0.42 * core) * env * band_scale
    for c in range(3):
        glow[c] = glow_col[c] * base
    fade = smoothstep(0.35, 0.62, env)
    detail_on = env > 0.35
    wobble = (fbm2(along * 0.9, np.full_like(along, 2.2)) - 0.5) * 0.05
    diag_w = diag - wobble
    width_b = 0.10 + 0.05 * fbm2(along * 0.8, np.full_like(along, 4.0)) + 0.06 * core
    band = np.exp(-(diag_w * diag_w) / (2.0 * width_b * width_b))
    dom_x = along * 4.0 + fbm2(along * 2.0, diag_w * 3.0)
    dom_y = diag_w * 9.0
    clouds = np.power(np.clip(fbm3(dom_x, dom_y), 0.0, 1.0), 1.6)
    rift_off = 0.012 + (fbm2(along * 1.2, np.full_like(along, 1.0)) - 0.5) * 0.03
    rift = np.exp(-((diag_w - rift_off) ** 2) / (2.0 * 0.018 * 0.018))
    rift_span = smoothstep(-0.6, -0.35, along) * (1.0 - smoothstep(0.15, 0.40, along))
    filaments = 1.0 - np.abs(vnoise(along * 2.5, diag_w * 24.0) * 2.0 - 1.0)
    filaments = filaments * filaments
    dust = saturate(rift * rift_span * 0.85 + filaments * 0.40 * band)
    detail_amp = (0.22 + 0.48 * core) * (0.40 + 0.60 * clouds) * band * (1.0 - 0.70 * dust) * band_scale
    detail_amp = np.where(detail_on, detail_amp, 0.0) * fade
    knots = smoothstep(0.78, 0.90, fbm2(along * 3.0, diag_w * 8.0))
    knot_amp = np.where(detail_on, knots * 0.10 * band * band_scale * fade, 0.0)
    gx = np.floor(pixel_x * 0.75)
    gy = np.floor(pixel_y * 0.75)
    h1 = hash12(gx, gy)
    h2 = hash12(gx + 19.0, gy + 3.0)
    haze = np.where(h1 > (1.0 - 0.38 * band), np.power(h2, 2.4) * 0.28 * (1.0 - dust), 0.0)
    haze = np.where(detail_on, haze * band_scale * fade, 0.0)
    ext = lerp(1.0, 1.0 - 0.48 * dust * band, np.where(detail_on, fade, 0.0))
    pink = np.array([1.0, 0.40, 0.48], dtype=np.float32)
    for c in range(3):
        glow[c] *= ext
        glow[c] += glow_col[c] * detail_amp + pink[c] * knot_amp + glow_col[c] * haze
    glow = glow / (1.0 + glow * 0.90)
    col += glow
    return env, sky_keep


def meteor(col, p, height, time):
    period = 9.0
    cycle = np.floor(time / period)
    u = (time / period) - cycle
    if u >= 0.16:
        return
    start_x = hash12(np.array(cycle), np.array(1.7)) * 1.3 - 0.75
    start_y = hash12(np.array(cycle), np.array(4.2)) * 0.50 - 0.38
    dir_y = 0.18 + 0.22 * float(hash12(np.array(cycle), np.array(8.0)))
    direc = np.array([0.86, dir_y], dtype=np.float32)
    direc = direc / np.linalg.norm(direc)
    head = np.array([start_x, start_y], dtype=np.float32) + direc * (u / 0.16) * 0.72
    rel_x = p[0] - head[0]
    rel_y = p[1] - head[1]
    along = rel_x * direc[0] + rel_y * direc[1]
    side_x = rel_x - direc[0] * along
    side_y = rel_y - direc[1] * along
    side_px = np.sqrt(side_x * side_x + side_y * side_y) * height
    along_px = along * height
    behind = np.maximum(-along_px, 0.0)
    head_g = np.exp(-(side_px * side_px + along_px * along_px) / (0.8 * 0.8))
    wide = lerp(2.2, 0.55, saturate(behind / (height * 0.22)))
    trail = np.exp(-(side_px * side_px) / (wide * wide))
    trail = trail * np.exp(-behind / (height * 0.22))
    trail = trail * smoothstep(height * 0.22, height * 0.01, behind)
    trail = trail * np.where(along_px < 0.8, 1.0, 0.0)
    life = u / 0.16
    fade = np.sin(np.power(life, 0.55) * np.pi)
    col[0] += (head_g * 1.15 * fade) + trail * fade * 1.05
    col[1] += (head_g * 1.05 * fade) + trail * fade * 0.72
    col[2] += (head_g * 0.90 * fade) + trail * fade * 0.42


def render_stars(w, h, preset, time):
    speed, density, twinkle, nebula, warp, direction, color, look = preset
    ys, xs = np.indices((h, w), dtype=np.float32)
    uv = np.stack([(xs + 0.5) / w, (ys + 0.5) / h], axis=0)
    aspect = w / float(h)
    p = np.stack([(uv[0] - 0.5) * aspect, uv[1] - 0.5], axis=0)
    travel = float(np.clip(warp, 0.0, 1.0))
    rot = time * speed * 0.012 * (1.0 - travel)
    cs, sn = np.cos(rot), np.sin(rot)
    pr = np.stack([cs * p[0] - sn * p[1], sn * p[0] + cs * p[1]], axis=0)
    ang = direction * 2.0 * np.pi
    fwd = np.array([np.cos(ang), np.sin(ang)], dtype=np.float32)
    scroll = (time * speed * 0.085) % 64.0
    sky = smoothstep(0.0, 0.9, uv[1])
    col = np.zeros((3, h, w), dtype=np.float32)
    col[0] = lerp(0.004, 0.009, sky)
    col[1] = lerp(0.006, 0.013, sky)
    col[2] = lerp(0.018, 0.030, sky)
    col[0] += 0.005 * smoothstep(0.55, 1.0, uv[1])
    col[1] += 0.009 * smoothstep(0.55, 1.0, uv[1])
    col[2] += 0.016 * smoothstep(0.55, 1.0, uv[1])

    band_scale = max(1.0 - travel * 0.55, 0.42)
    band, sky_keep = paint_band(col, pr, uv, xs, ys, band_scale)
    rgb = hex_rgb(color)
    peak = max(float(rgb.max()), 0.001)
    tint = rgb / peak
    if travel > 0.25:
        add_radial(col, pr, h, time, speed, travel, density, tint)
    else:
        # A fixed sparse field, plus a softer layer whose keep follows the band.
        add_stars(col, pr, h, time, scroll, 0.0, fwd, twinkle, density, tint,
                  ((34.0, 1.0, 0.22),), (0.0, 0.0), field=True)
        add_stars_mask(col, pr, h, time, scroll, twinkle, density, tint, 88.0, 0.62, sky_keep)
    if nebula > 0.08:
        blob = np.exp(-((pr[0] + 0.05) ** 2) / 0.70 - ((pr[1] + 0.02) ** 2) / 0.28)
        blob2 = np.exp(-((pr[0] - 0.45) ** 2) / 0.35 - ((pr[1] - 0.12) ** 2) / 0.16)
        n1 = fbm2(pr[0] * 1.15 + time * 0.008, pr[1] * 1.15 - 0.4)
        n2 = fbm2(pr[0] * 2.4 + 1.7, pr[1] * 1.8 + 0.6)
        veil = smoothstep(0.36, 0.78, n1) * (0.40 + 0.60 * n2) * (blob + 0.65 * blob2) * nebula
        neb_col = lerp(np.array([0.16, 0.07, 0.24], dtype=np.float32), tint, 0.62)
        for c in range(3):
            col[c] += neb_col[c] * veil * 1.85
    if travel < 0.45:
        meteor(col, pr, h, time)
    col = tonemap(col)
    enc = grade(col, uv, w, h, look, quantize=True)
    return np.clip(enc * 255.0 + 0.5, 0, 255).astype(np.uint8).transpose(1, 2, 0)


def add_stars_mask(col, p, height, time, scroll, twinkle, density, tint, scale, depth, keep):
    """Like add_stars, but `keep` is an image (the soft band density)."""
    depth_mix = float(lerp(0.35, 1.0, depth))
    q = p * scale
    cx = np.floor(q[0])
    cy = np.floor(q[1])
    fx = frac(q[0]) - 0.5
    fy = frac(q[1]) - 0.5
    n = hash12(cx + 3.1, cy + depth * 9.0)
    gate = 1.0 - keep
    alive = n > gate
    t = saturate((n - gate) / np.maximum(keep, 1e-3))
    bright = np.power(t, 1.7)
    jx, jy = hash22(cx + depth * 17.0, cy + 4.0)
    dx = fx - (jx - 0.5) * 0.28
    dy = fy - (jy - 0.5) * 0.28
    dist = np.sqrt(dx * dx + dy * dy) * (height / scale)
    core_r = np.maximum(lerp(1.05, 1.75, bright), 0.2)
    core = np.exp(-(dist * dist) / (core_r * core_r * 0.62))
    halo = np.where(t > 0.82, np.exp(-(dist * dist) / 10.0) * 0.55, 0.0)
    u = np.power(hash12(cx + 9.0, cy + 2.2), 0.55)
    bb = star_color(u)
    tw = 0.90 + 0.10 * np.sin(time * (0.45 + n * 1.4) + n * 42.0)
    amp = lerp(1.0, tw, twinkle)
    inten = np.where(alive, (core * (0.34 + 2.15 * bright) + halo) * amp * density, 0.0)
    shade = (0.55 + 0.45 * depth) * inten
    for c in range(3):
        col[c] += bb[c] * shade * float(tint[c])


def aurora_ribbons(px, py, t, height_k, rays_k, ca, cb, surge, aspect, lift, edge):
    curtain = np.zeros((3,) + px.shape, dtype=np.float32)
    ca = np.asarray(ca, dtype=np.float32)[:, None, None]
    cb = np.asarray(cb, dtype=np.float32)[:, None, None]
    rays_k = float(np.clip(rays_k, 0.0, 1.0))
    for layer, fade, y_lift, x_shift, h_scale, ray_contrast, edge_boost, tint in (
        (0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0, np.array([1.0, 1.0, 1.0], dtype=np.float32)),
        (1, 0.30, 0.0, 3.1, 1.4, 0.5, 0.0, np.array([0.8, 0.9, 1.1], dtype=np.float32)),
    ):
        if edge_boost > 0.5 and not edge:
            edge_boost = 0.0
        w1 = fbm2(px * 0.38 + t * 0.02 + x_shift, np.full_like(px, 0.4 + layer * 4.0))
        w2 = fbm2(px * 1.05 + 2.0 + layer * 3.0, np.full_like(px, 1.6 + layer))
        qx = px + (w1 - 0.5) * 1.6 + (w2 - 0.5) * 0.40
        w1b = fbm2((px + 0.02) * 0.38 + t * 0.02 + x_shift, np.full_like(px, 0.4 + layer * 4.0))
        w2b = fbm2((px + 0.02) * 1.05 + 2.0 + layer * 3.0, np.full_like(px, 1.6 + layer))
        qxb = (px + 0.02) + (w1b - 0.5) * 1.6 + (w2b - 0.5) * 0.40
        dqx = (qxb - qx) / 0.02
        gain = np.clip(0.35 / np.maximum(np.abs(dqx), 0.05), 0.6, 2.5)
        gain = 0.55 + 0.55 * gain
        fold = fbm2(qx * 0.7 + layer * 2.2, np.full_like(px, 0.3 + t * 0.01))
        base = lerp(0.92, 0.64, height_k) - y_lift - lift
        base = base + (fold - 0.48) * 0.42
        above = base - py
        h_len = lerp(0.06, 0.24, height_k) * h_scale
        rx = qx + (px - 0.5 * aspect) * np.maximum(above, 0.0) * 0.35
        ray_n = vnoise1(rx * 38.0)
        ray_f = vnoise1(rx * 110.0)
        ray = 0.35 + 0.65 * np.power(saturate(ray_n), 2.5) + 0.25 * ray_f
        ray = lerp(1.0, ray, rays_k * ray_contrast)
        hs = h_len * (0.45 + 0.9 * ray)
        body = smoothstep(-0.0025, 0.0015, above) * np.exp(-np.maximum(above, 0.0) / np.maximum(hs, 1e-3))
        hem = np.exp(-np.maximum(above, 0.0) / 0.0045) * edge_boost
        knots = 0.6 + 0.8 * vnoise1(qx * 6.0)
        intensity = body * (1.0 + 2.4 * hem) * knots * gain
        sheet = smoothstep(0.25, 0.65, fbm2(qx * 0.32 + x_shift, np.full_like(px, 0.7 + layer)))
        tcol = above / np.maximum(hs, 1e-3)
        rgb = lerp(ca, cb, smoothstep(0.60, 1.20, tcol))
        rgb = np.minimum(rgb, 1.4)
        inten = lerp(1.0, 0.25, saturate(smoothstep(1.0, 1.8, tcol)))
        glow = np.exp(-np.abs(above) / 0.12) * 0.05
        pink = np.exp(-np.maximum(-above, 0.0) / 0.002) * (1.0 - smoothstep(-0.0025, 0.0015, above))
        pink = pink * smoothstep(0.8, 0.95, surge) * edge_boost
        mask = sheet * fade
        for c in range(3):
            curtain[c] += rgb[c] * tint[c] * intensity * inten * mask
            curtain[c] += ca[c] * tint[c] * glow * mask
            curtain[c] += np.array([1.0, 0.35, 0.55], dtype=np.float32)[c] * pink * 0.45 * mask
    # Faint third arc, no rays, above the main curtains.
    w = fbm2(px * 0.30 + 5.0, np.full_like(px, 1.2))
    base3 = lerp(0.92, 0.64, height_k) - lift - 0.16 + (w - 0.5) * 0.20
    a3 = base3 - py
    i3 = smoothstep(-0.01, 0.004, a3) * np.exp(-np.maximum(a3, 0.0) / 0.16) * 0.06
    for c in range(3):
        curtain[c] += ca[c] * i3
    return curtain


def fractal_ridge(x, base, amp, seed):
    h = ridged(x, 1.7, seed) * 0.46
    h = h + ridged(x, 3.6, seed + 1.7) * 0.28
    h = h + ridged(x, 7.8, seed + 4.0) * 0.16
    h = h + ridged(x, 16.0, seed + 8.0) * 0.10
    return base - h * amp


def pine_cover(uv, ground, scale, seed, h0, h1, grove_y, present_at, shore):
    tx = uv[0] * scale
    grove = fbm2(uv[0] * 2.3, np.full_like(uv[0], grove_y))
    cov = np.zeros_like(uv[0])
    px = uv.shape[-1] / scale
    for k in (-1, 0, 1):
        cell = np.floor(tx) + k
        fr = tx - cell
        h_a = hash12(cell, np.full_like(cell, seed))
        h_b = hash12(cell, np.full_like(cell, seed + 17.0))
        h_c = hash12(cell, np.full_like(cell, seed + 4.0))
        h_d = hash12(cell, np.full_like(cell, seed + 29.0))
        present = np.where((h_c > present_at) & (grove > 0.30) & (ground > shore - 0.08), 1.0, 0.0)
        lean = (h_d - 0.5) * 0.05
        center = 0.50 + (h_b - 0.5) * 0.20
        tree_h = lerp(h0, h1, h_b)
        t = (ground - uv[1]) / np.maximum(tree_h, 1e-3)
        dx = np.abs(fr - (center + lean * saturate(t)))
        tiers = lerp(7.0, 10.0, h_a)
        half = lerp(0.10, 0.20, h_d)
        tier = frac(t * tiers)
        flare = lerp(1.25, 0.15, smoothstep(0.0, 0.78, tier))
        env = np.maximum(1.0 - t, 0.0) * flare
        edge = saturate((half * env - dx) * px / 1.15)
        on = np.where((t > 0.0) & (t < 1.0), present, 0.0)
        cov = np.maximum(cov, edge * on)
        trunk = present
        trunk_h = tree_h * 0.18
        trunk_on = np.where((uv[1] > ground) & (uv[1] < ground + trunk_h) & (np.abs(fr - center) < 0.035), 1.0, 0.0)
        cov = np.maximum(cov, trunk * trunk_on)
    return cov


def paint_horizon(col, uv, w, h, t, height_k, rays_k, ca, cb, surge, aspect):
    shore = 0.66 + (fbm2(uv[0] * 1.6, np.full_like(uv[0], 11.0)) - 0.5) * 0.012
    far = fractal_ridge(uv[0], shore + 0.02, 0.34, 1.0)
    near = fractal_ridge(uv[0], shore, 0.16, 6.5)
    aa = 1.35 / max(h, 1.0)
    sky_side = uv[1] < shore
    water_side = ~sky_side
    # Mountains and trees only on the sky side, and only near the ridge band.
    band = sky_side & (uv[1] > shore - 0.28)
    far_a = smoothstep(far - aa, far + aa, uv[1])
    near_a = smoothstep(near - aa, near + aa, uv[1])
    trees = pine_cover(uv, far, 40.0, 3.0, 0.016, 0.040, 1.2, 0.62, shore)
    trees = np.maximum(trees, pine_cover(uv, near, 22.0, 30.0, 0.035, 0.100, 6.5, 0.08, shore))
    trees = np.maximum(trees, pine_cover(uv, shore, 12.0, 80.0, 0.045, 0.120, 9.0, 0.05, shore))
    # Continuous treeline mass, 0.015 deep on the near ridge.
    treeline = smoothstep(0.0, 0.015, near - uv[1]) * smoothstep(0.04, 0.01, near - uv[1])
    treeline = treeline * np.where(fbm2(uv[0] * 8.0, np.full_like(uv[0], 2.0)) > 0.35, 1.0, 0.0)
    trees = np.maximum(trees, treeline)
    tex = 0.8 + 0.2 * fbm2(uv[0] * 40.0, uv[1] * 40.0)
    haze = saturate((uv[1] - (far - 0.02)) / 0.08)
    far_col = lerp(np.array([0.018, 0.030, 0.028], dtype=np.float32)[:, None, None],
                   np.array([0.008, 0.012, 0.014], dtype=np.float32)[:, None, None], haze) * tex
    near_col = np.array([0.003, 0.004, 0.005], dtype=np.float32)[:, None, None]
    behind = col.copy()
    d = (far - uv[1]) / max(aa, 1e-4)
    rim = np.exp(-((d - 1.2) ** 2) / 7.0) * np.where(d > -0.2, 1.0, 0.0)
    rim = rim * (1.0 - near_a) * np.where(sky_side, 1.0, 0.0) * (1.0 - trees)
    aur = behind[1]
    rim = rim * saturate(aur * 6.0)
    # The whole silhouette, including peaks that rise well above the treeline, covers the curtain.
    occlude = sky_side & (uv[1] >= far)
    for c in range(3):
        rock_col = lerp(far_col[c], near_col[c], near_a)
        col[c] = np.where(occlude, rock_col, col[c])
        col[c] = np.where(occlude, lerp(col[c], 0.0012, np.clip(trees, 0.0, 1.0)), col[c])
        col[c] = col[c] + (ca[c] * 0.55 + cb[c] * 0.25) * rim * 0.35
    # Water: occlude where the mirrored ray hits the mountain, otherwise a rough reflection.
    my = shore * 2.0 - uv[1]
    rock = my > far
    depth = np.maximum(uv[1] - shore, 0.0)
    xoff = (vnoise(uv[0] * 4.0, uv[1] * 60.0) - 0.5) * 0.015 * (depth / 0.2)
    refl = aurora_ribbons((uv[0] * aspect) + xoff, my, t, height_k, rays_k * 0.25, ca, cb, surge,
                           aspect, 0.10, False)
    streaks = 0.55 + 0.45 * vnoise(uv[0] * 1.5, uv[1] * 140.0)
    depth_fade = np.exp(-depth / 0.22)
    for c in range(3):
        water = refl[c] * streaks * depth_fade * 0.85
        water = np.where(rock, 0.004, water)
        col[c] = np.where(water_side, water, col[c])
    return col


def render_aurora(w, h, preset, time):
    speed, height_k, rays_k, stars, horizon, color_a, color_b, look = preset
    ys, xs = np.indices((h, w), dtype=np.float32)
    uv = np.stack([(xs + 0.5) / w, (ys + 0.5) / h], axis=0)
    aspect = w / float(h)
    px = uv[0] * aspect
    py = uv[1]
    t = time * speed
    col = np.zeros((3, h, w), dtype=np.float32)
    sky_t = smoothstep(0.0, 0.88, uv[1])
    col[0] = lerp(0.002, 0.008, sky_t)
    col[1] = lerp(0.003, 0.010, sky_t)
    col[2] = lerp(0.008, 0.020, sky_t)
    if stars:
        p = np.stack([(uv[0] - 0.5) * aspect, uv[1] - 0.5], axis=0)
        tint = np.array([0.85, 0.9, 1.0], dtype=np.float32)
        add_stars(col, p, h, time, 0.0, 0.0, np.array([1.0, 0.0], dtype=np.float32), 0.35, 0.55, tint,
                  ((80.0, 0.7, 0.012),), (0.0, 0.0))
    ca = hex_rgb(color_a)
    cb = hex_rgb(color_b)
    surge = 0.72 + 0.28 * float(vnoise(np.array(t * 0.045), np.array(2.2)))
    lift = 0.10 if horizon else 0.0
    col += aurora_ribbons(px, py, t, height_k, rays_k, ca, cb, surge, aspect, lift, True)
    if horizon:
        col = paint_horizon(col, uv, w, h, t, height_k, rays_k, ca, cb, surge, aspect)
    col = tonemap(col)
    enc = grade(col, uv, w, h, look)
    return np.clip(enc * 255.0 + 0.5, 0, 255).astype(np.uint8).transpose(1, 2, 0)


def tree_line(x, y, ground, scale, seed, amp, height, fill):
    tx = x * scale
    if fill > 0.5:
        aa = 1.5 / max(height, 1.0)
        cov = smoothstep(ground - aa, ground + aa, y)
    else:
        cov = np.zeros_like(x)
    edge_k = (height / max(scale, 1.0)) * 0.55
    for k in (-1.0, 0.0, 1.0):
        cell = np.floor(tx) + k
        fr = tx - cell
        for ti in (0.0, 1.0):
            hsh = hash12(cell, np.full_like(cell, seed + ti * 3.0))
            h2 = hash12(cell, np.full_like(cell, seed + 5.0 + ti))
            present = (hsh > (0.05 if ti == 0.0 else 0.42)).astype(np.float32)
            center = lerp(0.12, 0.88, hsh)
            if ti == 1.0:
                center = frac(center + 0.41)
            tree_h = amp * lerp(0.22, 1.0, h2)
            tt = (ground - y) / np.maximum(tree_h, 0.001)
            dx = np.abs(fr - center)
            half_w = lerp(0.08, 0.34, hsh) * (1.0 if ti == 0.0 else 0.55)
            shape = np.power(saturate(1.0 - tt), 0.65)
            edge = saturate((half_w * shape - dx) * edge_k)
            on = ((tt > 0.0) & (tt < 1.0)).astype(np.float32) * present
            cov = np.maximum(cov, edge * on)
    return cov


def add_flies(col, uv, scale, depth, dens, tint, time, speed, parallax_x, height):
    px = uv[0] + parallax_x + np.sin(time * speed * 0.20 + uv[1] * 6.0) * 0.012 * depth
    py = uv[1] + time * speed * 0.015 * depth
    qx = px * scale
    qy = py * scale * 0.72
    cx = np.floor(qx)
    cy = np.floor(qy)
    fx = (qx - cx) - 0.5
    fy = (qy - cy) - 0.5
    n = hash12(cx + depth * 9.0, cy + 1.7)
    keep = float(np.clip(0.22 * dens, 0.0, 1.0))
    alive = n > (1.0 - keep)
    jx, jy = hash22(cx, cy)
    phase = n * 6.2831853
    wob = np.sin(time * (0.25 + n * 0.35) * speed + phase)
    dx = fx - ((jx - 0.5) * 0.55 + wob * 0.22)
    dy = fy - ((jy - 0.5) * 0.55 + np.cos(time * 0.18 * speed + phase) * 0.22)
    dist = np.sqrt(dx * dx + dy * dy) * (height / scale)
    pulse = np.power(saturate(np.sin(time * (0.45 + n * 0.25) + phase) * 0.5 + 0.5), 6.0)
    core = np.exp(-(dist * dist) / 1.6)
    bloom = np.exp(-(dist * dist) / (10.0 + 14.0 * depth))
    band = smoothstep(0.16, 0.40, uv[1])
    shade = np.where(alive, (core * 1.7 + bloom * 0.72) * pulse * lerp(0.45, 1.0, depth) * band, 0.0)
    tint = np.asarray(tint, dtype=np.float32)
    for c in range(3):
        col[c] += tint[c] * shade


def add_snow(col, uv, scale, depth, dens, wind, time, speed, tint, softness, height, look_x):
    sway = np.sin(time * 0.35 * speed + uv[1] * 3.0 + depth * 2.2) * 0.02 * (0.25 + depth)
    px = uv[0] + wind * time * speed * (0.02 + 0.06 * depth) + sway + look_x * depth * 0.22
    py = uv[1] + time * speed * (0.012 + 0.04 * depth)
    qx = px * scale + depth * 17.0
    qy = py * scale * 0.72 + depth * 9.0
    ox = np.floor(qx)
    oy = np.floor(qy)
    fx = qx - ox
    fy = qy - oy
    keep = float(np.clip(dens * lerp(0.09, 0.22, depth), 0.0, 1.0))
    rad = lerp(1.1, 18.0, depth) * softness
    shade = np.zeros_like(qx)
    for jy in (-1.0, 0.0, 1.0):
        for jx in (-1.0, 0.0, 1.0):
            cx = ox + jx
            cy = oy + jy
            n = hash12(cx + 2.0, cy + depth * 11.0)
            alive = n > (1.0 - keep)
            hx, hy = hash22(cx + depth, cy + 0.4)
            dx = (fx - jx) - 0.5 - (hx - 0.5) * 0.35
            dy = (fy - jy) - 0.5 - (hy - 0.5) * 0.35
            dist = np.sqrt(dx * dx + dy * dy) * (height / scale)
            core = np.exp(-(dist * dist) / np.maximum(rad * rad * 0.28, 0.2))
            halo = np.exp(-(dist * dist) / np.maximum(rad * rad * 1.8, 0.8)) * lerp(0.0, 0.55, depth)
            shade += np.where(alive, core * lerp(0.45, 1.15, depth) + halo, 0.0)
    tint = np.asarray(tint, dtype=np.float32)
    for c in range(3):
        col[c] += tint[c] * shade


def scene_stats(col):
    lin = np.power(np.clip(1.0 - np.exp(-1.1 * np.maximum(col, 0.0)), 0.0, 1.0), 2.2)
    luma = lin[0] * 0.2126 + lin[1] * 0.7152 + lin[2] * 0.0722
    lit = int(np.count_nonzero(luma >= 0.02))
    cover = lit / float(luma.size)
    return float(luma.max()), lit, cover


def render_fireflies(w, h, preset, time):
    speed, density, mist, stars, woods, color, look = preset
    ys, xs = np.indices((h, w), dtype=np.float32)
    uv = np.stack([(xs + 0.5) / w, (ys + 0.5) / h], axis=0)
    aspect = w / float(h)
    col = np.zeros((3, h, w), dtype=np.float32)
    zenith = lerp(np.array([0.015, 0.020, 0.055], np.float32), np.array([0.004, 0.006, 0.016], np.float32), woods)
    dusk = lerp(np.array([0.16, 0.055, 0.045], np.float32), np.array([0.035, 0.020, 0.028], np.float32), woods)
    horizon = lerp(np.array([0.42, 0.18, 0.07], np.float32), np.array([0.08, 0.045, 0.03], np.float32), woods)
    sky = lerp(zenith[:, None, None], dusk[:, None, None], smoothstep(0.05, 0.48, uv[1]))
    sky = lerp(sky, horizon[:, None, None], smoothstep(0.42, 0.74, uv[1]))
    sky = sky + horizon[:, None, None] * 0.15 * np.exp(-np.abs(uv[1] - 0.70) / 0.08)
    col[:] = sky
    if stars > 0.02:
        sp = np.stack([(uv[0] - 0.5) * aspect, uv[1] - 0.42], axis=0)
        mask = uv[1] < 0.64
        tint = np.array([0.86, 0.90, 1.0], np.float32)
        before = col.copy()
        add_stars(col, sp, h, time, 0.0, 0.0, np.zeros(2, np.float32), 0.35, 0.65, tint,
                  ((64.0, 0.85, 0.045 * stars),), (0.0, 0.0), True)
        for c in range(3):
            col[c] = np.where(mask, col[c], before[c])
    far = tree_line(uv[0], uv[1], 0.66, 4.6, 2.0, lerp(0.18, 0.36, woods), h, 0.0)
    mid = tree_line(uv[0], uv[1], 0.80, 7.5, 6.5, lerp(0.12, 0.22, woods), h, 1.0)
    near = tree_line(uv[0], uv[1], 0.92, 12.0, 11.0, lerp(0.08, 0.15, woods), h, 1.0)
    far_col = lerp(np.array([0.035, 0.028, 0.032], np.float32), np.array([0.010, 0.012, 0.014], np.float32), woods)
    for c in range(3):
        col[c] = lerp(col[c], far_col[c], far * 0.94)
        col[c] = lerp(col[c], 0.008 if c == 0 else 0.009 if c == 1 else 0.010, mid)
        col[c] = lerp(col[c], 0.0025 if c == 0 else 0.0030 if c == 1 else 0.0035, near)
    mist_n = fbm2(uv[0] * 2.2 + time * speed * 0.012, np.full_like(uv[0], 0.35))
    mist_a = smoothstep(0.64, 1.0, uv[1]) * (0.50 + 0.50 * mist_n) * mist * 0.50
    col[0] += 0.11 * mist_a
    col[1] += 0.12 * mist_a
    col[2] += 0.13 * mist_a
    tint = np.maximum(hex_rgb(color), 0.02)
    add_flies(col, uv, 22.0, 0.42, density, tint * 0.75, time, speed, 0.0, h)
    add_flies(col, uv, 10.0, 1.0, density, tint, time, speed, 0.0, h)
    print("  fireflies scene", scene_stats(col))
    enc = grade(tonemap(col), uv, w, h, look)
    return np.clip(enc * 255.0 + 0.5, 0, 255).astype(np.uint8).transpose(1, 2, 0)


def render_snow(w, h, preset, time):
    speed, density, wind, backdrop, color, look = preset
    ys, xs = np.indices((h, w), dtype=np.float32)
    uv = np.stack([(xs + 0.5) / w, (ys + 0.5) / h], axis=0)
    aspect = w / float(h)
    col = np.zeros((3, h, w), dtype=np.float32)
    sky = lerp(np.array([0.012, 0.016, 0.040], np.float32)[:, None, None],
               np.array([0.028, 0.032, 0.055], np.float32)[:, None, None], smoothstep(0.0, 1.0, uv[1]))
    col[:] = sky
    if backdrop < 0.5:
        sp = np.stack([(uv[0] - 0.5) * aspect, uv[1] - 0.45], axis=0)
        before = col.copy()
        add_stars(col, sp, h, time, 0.0, 0.0, np.zeros(2, np.float32), 0.25, 0.55,
                  np.array([0.90, 0.93, 1.0], np.float32), ((58.0, 0.9, 0.06),), (0.0, 0.0), True)
        for c in range(3):
            col[c] = np.where(uv[1] < 0.72, col[c], before[c])
    elif backdrop < 1.5:
        sp = np.stack([(uv[0] - 0.5) * aspect, uv[1] - 0.40], axis=0)
        before = col.copy()
        add_stars(col, sp, h, time, 0.0, 0.0, np.zeros(2, np.float32), 0.2, 0.4,
                  np.array([0.85, 0.90, 1.0], np.float32), ((70.0, 0.7, 0.03),), (0.0, 0.0), True)
        for c in range(3):
            col[c] = np.where(uv[1] < 0.55, col[c], before[c])
        far = tree_line(uv[0], uv[1], 0.64, 5.2, 3.0, 0.28, h, 0.0)
        near = tree_line(uv[0], uv[1], 0.82, 9.0, 9.0, 0.18, h, 1.0)
        for c in range(3):
            col[c] = lerp(col[c], np.array([0.012, 0.016, 0.018], np.float32)[c], far)
            col[c] = lerp(col[c], np.array([0.003, 0.004, 0.005], np.float32)[c], near)
        ground = smoothstep(0.78, 0.95, uv[1]) * (1.0 - near)
        col[0] = lerp(col[0], 0.045, ground)
        col[1] = lerp(col[1], 0.050, ground)
        col[2] = lerp(col[2], 0.058, ground)
    else:
        col[:] = np.array([0.015, 0.008, 0.005], np.float32)[:, None, None]
        c1 = uv - np.array([0.36, 0.46], np.float32)[:, None, None]
        c2 = uv - np.array([0.66, 0.58], np.float32)[:, None, None]
        b1 = np.exp(-np.sum(c1 * c1, axis=0) / 0.010)
        b2 = np.exp(-np.sum(c2 * c2, axis=0) / 0.006)
        col[0] += 0.95 * b1 * 0.85 + 1.0 * b2 * 0.55
        col[1] += 0.48 * b1 * 0.85 + 0.72 * b2 * 0.55
        col[2] += 0.16 * b1 * 0.85 + 0.38 * b2 * 0.55
        frame = smoothstep(0.0, 0.07, uv[0]) * smoothstep(1.0, 0.93, uv[0])
        frame = frame * smoothstep(0.0, 0.08, uv[1]) * smoothstep(1.0, 0.90, uv[1])
        col *= lerp(0.05, 1.0, frame)
    tint = np.maximum(hex_rgb(color), 0.2)
    add_snow(col, uv, 36.0, 0.12, density, wind, time, speed, tint, 0.70, h, 0.0)
    add_snow(col, uv, 18.0, 0.48, density, wind, time, speed, tint, 1.0, h, 0.0)
    add_snow(col, uv, 7.0, 1.0, density, wind, time, speed, tint, 1.15, h, 0.0)
    print("  snow scene", scene_stats(col))
    enc = grade(tonemap(col), uv, w, h, look)
    return np.clip(enc * 255.0 + 0.5, 0, 255).astype(np.uint8).transpose(1, 2, 0)


def presets():
    night = (1.0, 1.0, 0.18, False)
    stars = {
        "still-night": (0.28, 1.15, 0.40, 0.16, 0.0, 0.0, 0xF3F6FF, (1.0, 1.0, 0.18, False)),
        "milky-way": (0.18, 1.55, 0.28, 0.05, 0.0, 0.0, 0xFFF6E8, (0.92, 0.9, 0.34, True)),
        "nebula": (0.30, 0.85, 0.32, 0.85, 0.0, 0.0, 0xD9C6FF, (1.0, 1.2, 0.28, False)),
        "warp": (1.7, 0.62, 0.12, 0.10, 0.92, 0.06, 0xE7F0FF, (1.05, 1.05, 0.20, False)),
    }
    aurora = {
        "classic": (0.32, 0.72, 0.82, True, False, 0x3CFF78, 0xFF4B7A, night),
        "magenta": (0.36, 0.66, 0.60, True, False, 0xFF5FA2, 0xB44CFF, (1.0, 1.25, 0.24, False)),
        "polar": (0.24, 0.78, 0.70, True, False, 0x86FFE0, 0x6A4BFF, (0.96, 1.1, 0.30, False)),
        "horizon": (0.26, 0.85, 0.48, True, True, 0x3CFF78, 0xFF4B7A, (0.92, 1.0, 0.36, True)),
    }
    fireflies = {
        "summer-dusk": (0.32, 1.0, 0.55, 0.35, 0.45, 0xD2F26A, (1.0, 1.05, 0.28, False)),
        "deep-woods": (0.22, 0.72, 0.82, 0.0, 0.95, 0xB6E86A, (0.82, 0.95, 0.42, True)),
        "meadow": (0.38, 1.45, 0.32, 0.9, 0.12, 0xE8FF8A, (1.05, 1.15, 0.20, False)),
    }
    snow = {
        "night-sky": (0.40, 1.0, 0.22, 0, 0xF7F8FF, (1.0, 1.0, 0.22, False)),
        "pine-forest": (0.48, 1.15, 0.38, 1, 0xF4F7FF, (0.96, 0.9, 0.30, True)),
        "window": (0.28, 0.75, 0.10, 2, 0xFFF4E4, (1.0, 1.05, 0.18, False)),
    }
    return stars, aurora, fireflies, snow


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="docs")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--only", default="")
    ap.add_argument("--check", action="store_true", help="run the art-direction objective checks")
    args = ap.parse_args()
    if args.check:
        from scene_check import run as run_checks
        code = run_checks(args.scale)
        if code:
            raise SystemExit(code)
    os.makedirs(args.out, exist_ok=True)
    w = max(16, int(W0 * args.scale))
    h = max(16, int(H0 * args.scale))
    stars, aurora, fireflies, snow = presets()
    jobs = [("stars", k, v) for k, v in stars.items()] + [("aurora", k, v) for k, v in aurora.items()]
    jobs += [("fireflies", k, v) for k, v in fireflies.items()] + [("snow", k, v) for k, v in snow.items()]
    if args.only:
        jobs = [j for j in jobs if args.only in f"{j[0]}-{j[1]}"]
    for kind, name, preset in jobs:
        time = 4.2 if kind == "fireflies" else 3.1 if kind == "snow" else 0.48 if kind == "stars" else 6.5
        print(f"rendering {kind} {name} {w}x{h}", flush=True)
        if kind == "stars":
            img = render_stars(w, h, preset, time)
        elif kind == "aurora":
            img = render_aurora(w, h, preset, time)
        elif kind in ("fireflies", "snow"):
            from night_scene import encode_named
            img, _ = encode_named(f"{kind}-{name}", w, h, time=time)
        else:
            img = render_snow(w, h, preset, time)
        filename = f"{kind}-{name}.png"
        if kind == "fireflies" and name == "summer-dusk":
            filename = "fireflies.png"
        elif kind == "snow" and name == "night-sky":
            filename = "snow.png"
        path = os.path.join(args.out, filename)
        Image.fromarray(img, "RGB").save(path, optimize=True)
        print(f"  wrote {path}", flush=True)


if __name__ == "__main__":
    main()
