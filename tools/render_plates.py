"""Stills of the shipped photoreal plates with the effect drawn in the right depth.

Not used by --check. The procedural scenes stay the regression fixtures.
"""

import math
import os

import numpy as np
from PIL import Image

from night_scene import _snow_one, add_fireflies, px_grid
from render_sky import (
    add_stars,
    aurora_ribbons,
    grade,
    hex_rgb,
    lerp,
    meteor,
    paint_band,
    saturate,
    smoothstep,
    tonemap,
    vnoise,
)

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "resources", "plates")
LAYERS = ("layer-a-sky", "layer-b-mountains", "layer-c-mid", "layer-d-foreground")
DEPTH = (0.22, 0.55, 1.05, 1.75)


def snap_and_desat(arr, aurora):
    rgb = arr[:, :, :3].astype(np.float32)
    a = arr[:, :, 3].astype(np.float32)
    a = np.where(a >= 250.0, 255.0, a)
    if aurora:
        r, g, b = rgb[:, :, 0], rgb[:, :, 1], rgb[:, :, 2]
        mx = np.maximum(np.maximum(r, g), b)
        mn = np.minimum(np.minimum(r, g), b)
        sat = (mx - mn) / np.maximum(mx, 1.0)
        luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
        neutral = (r + g + b) / 3.0
        teal = (g > r + 6.0) & (b > r + 6.0) & (sat > 0.08) & (a > 0) & (mx >= 8.0)
        t = np.clip((luma - 40.0) / 140.0, 0.0, 1.0)
        t = t * t * (3.0 - 2.0 * t)
        k = np.where(teal, 0.38 + 0.45 * t, 0.0)
        rgb[:, :, 0] = r + (neutral - r) * k
        rgb[:, :, 1] = g + (neutral - g) * k
        rgb[:, :, 2] = b + (neutral - b) * k
    return np.clip(rgb, 0.0, 255.0), a


def load_layers(effect, w, h, aurora):
    out = []
    for name in LAYERS:
        im = Image.open(os.path.join(ROOT, effect, name + ".webp")).convert("RGBA")
        im = im.resize((w, h), Image.Resampling.LANCZOS)
        rgb, alpha = snap_and_desat(np.asarray(im), aurora)
        lin = np.power(rgb / 255.0, 2.2).astype(np.float32)
        out.append((np.transpose(lin, (2, 0, 1)), (alpha / 255.0).astype(np.float32)))
    return out


def shift_amount(which, time, w, h):
    depth = DEPTH[which]
    ang = (time % 3600.0) / 3600.0 * math.tau
    drift = math.sin(ang * 2.0 + which * 0.7)
    drift_y = math.sin(ang + 1.4) * 0.35
    amp = 0.0012 + 0.0055 * depth
    return int(round(drift * amp * w)), int(round(drift_y * amp * h))


def shift_clamp(arr, dy, dx):
    # Matches saturate(uv + shift): the photo clamps at the edge instead of wrapping.
    h, w = arr.shape[-2:]
    src_y = np.clip(np.arange(h) - dy, 0, h - 1)
    src_x = np.clip(np.arange(w) - dx, 0, w - 1)
    return arr[..., src_y[:, None], src_x[None, :]]


def sample_layer(layer, which, time, w, h):
    rgb, alpha = layer
    dx, dy = shift_amount(which, time, w, h)
    if dx == 0 and dy == 0:
        return rgb, alpha
    return shift_clamp(rgb, dy, dx), shift_clamp(alpha, dy, dx)


def over(col, rgb, alpha):
    for c in range(3):
        col[c] = col[c] * (1.0 - alpha) + rgb[c] * alpha


def cover(sky, layers, time, w, h, spill):
    col = sky.copy()
    for i in range(1, 4):
        rgb, alpha = sample_layer(layers[i], i, time, w, h)
        if spill:
            luma = rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722
            ice = smoothstep(0.16, 0.55, luma)
            g = np.maximum(np.maximum(sky[0], sky[1]), sky[2])
            catch = ice * saturate((g - 0.18) * 1.1) * 0.28
            tint = sky / np.maximum(g, 1e-3)
            for c in range(3):
                lit = rgb[c] * lerp(1.0, tint[c], 0.40) + tint[c] * 0.012
                rgb[c] = lerp(rgb[c], lit, catch)
        over(col, rgb, alpha)
    return col


def encode_linear(lin, w, h, look):
    ys, xs = np.indices((h, w), dtype=np.float32)
    uv = np.stack([(xs + 0.5) / w, (ys + 0.5) / h], axis=0)
    # grade() expects a display-referred color and applies toLin itself.
    # This buffer is already linear, so invert that step with a 1/2.2 first.
    shown = np.power(np.clip(lin, 0.0, None), 1.0 / 2.2)
    enc = grade(shown, uv, w, h, look)
    return np.clip(enc * 255.0 + 0.5, 0, 255).astype(np.uint8).transpose(1, 2, 0)


def sky_of(layers, time, w, h):
    rgb, _alpha = sample_layer(layers[0], 0, time, w, h)
    return rgb


def render_fireflies(w, h, time):
    layers = load_layers("fireflies", w, h, False)
    x, y = px_grid(w, h)
    tint = lerp(hex_rgb(0xD2F26A), np.array([1.0, 0.58, 0.20], np.float32), 0.62)
    fx = np.zeros((3, h, w), np.float32)
    mask = np.zeros((h, w), np.float32)
    add_fireflies(fx, x, y, w, h, time, 0.32, 1.0, tint, 0.50, 0.64, 18.0, "far", mask, halo_scale=1.5)
    add_fireflies(fx, x, y, w, h, time, 0.32, 1.0, tint, 0.54, 0.72, 20.0, "mid", mask, halo_scale=1.5)
    add_fireflies(fx, x, y, w, h, time, 0.32, 1.0, tint, 0.60, 0.78, 3.0, "near", mask, halo_scale=1.5)
    # add_fireflies mixes toward a linear white. Treat the sum as the pre-grade glow.
    sky = sky_of(layers, time, w, h)
    col = sky.copy()
    rgb, alpha = sample_layer(layers[1], 1, time, w, h)
    over(col, rgb, alpha)
    rgb, alpha = sample_layer(layers[2], 2, time, w, h)
    over(col, rgb, alpha)
    glow = np.power(np.clip(tonemap(fx), 0.0, 1.0), 2.2)
    col += glow
    rgb, alpha = sample_layer(layers[3], 3, time, w, h)
    over(col, rgb, alpha)
    return encode_linear(col, w, h, (1.0, 1.05, 0.28, False))


def render_snow(w, h, time):
    layers = load_layers("snow", w, h, False)
    x, y = px_grid(w, h)
    flake = hex_rgb(0xF4F7FF)
    near = lerp(flake, np.array([0.93, 0.95, 1.0], np.float32), 0.72)
    far = near * np.array([0.70, 0.78, 0.92], np.float32)
    col = sky_of(layers, time, w, h).copy()
    rgb, alpha = sample_layer(layers[1], 1, time, w, h)
    over(col, rgb, alpha)
    _snow_one(col, x, y, w, h, time, 0.48, 0.38, 1.15, far, 0, None)
    rgb, alpha = sample_layer(layers[2], 2, time, w, h)
    over(col, rgb, alpha)
    rgb, alpha = sample_layer(layers[3], 3, time, w, h)
    over(col, rgb, alpha)
    for index in (1, 2, 3):
        _snow_one(col, x, y, w, h, time, 0.48, 0.38, 1.15, near, index, None)
    return encode_linear(col, w, h, (1.0, 0.9, 0.30, False))


def render_stars(w, h, time, preset):
    speed, density, twinkle, _nebula, warp, direction, color, look = preset
    layers = load_layers("starfield", w, h, False)
    ys, xs = np.indices((h, w), dtype=np.float32)
    uv = np.stack([(xs + 0.5) / w, (ys + 0.5) / h], axis=0)
    aspect = w / float(h)
    p = np.stack([(uv[0] - 0.5) * aspect, uv[1] - 0.5], axis=0)
    travel = float(np.clip(warp, 0.0, 1.0))
    rot = time * speed * 0.012 * (1.0 - travel)
    cs, sn = math.cos(rot), math.sin(rot)
    pr = np.stack([cs * p[0] - sn * p[1], sn * p[0] + cs * p[1]], axis=0)
    fx = np.zeros((3, h, w), np.float32)
    galaxy = 0.70 if density > 1.35 else 0.36
    detail = 0.18 if density > 1.35 else 0.0
    dust = 0.22 if density > 1.35 else 0.0
    _band, _keep = paint_band(
        fx, pr, uv, xs, ys, max(1.0 - travel * 0.55, 0.42) * galaxy,
        detail_gain=detail, cool=0.62, dust_gain=dust,
    )
    rgb = hex_rgb(color)
    tint = rgb / max(float(rgb.max()), 0.001)
    scroll = (time * speed * 0.085) % 64.0
    add_stars(fx, pr, h, time, scroll, 0.0, np.array([1.0, 0.0], np.float32), twinkle, density, tint,
              ((34.0, 1.0, 0.22),), (0.0, 0.0), field=True)
    if travel < 0.45:
        meteor(fx, pr, h, time)
    sky = sky_of(layers, time, w, h) + np.power(np.clip(tonemap(fx), 0.0, 1.0), 2.2)
    # Daytime grade. The Milky Way preset's night slider only comes on after dusk.
    day = (look[0], look[1], look[2], False)
    return encode_linear(cover(sky, layers, time, w, h, False), w, h, day)


def render_aurora(w, h, time):
    layers = load_layers("aurora", w, h, True)
    ys, xs = np.indices((h, w), dtype=np.float32)
    uv = np.stack([(xs + 0.5) / w, (ys + 0.5) / h], axis=0)
    aspect = w / float(h)
    look = 0.0
    shifted = uv + np.array([look, -look], np.float32)[:, None, None] * 0.12
    px = shifted[0] * aspect
    py = shifted[1]
    t = time * 0.32
    fx = np.zeros((3, h, w), np.float32)
    p = np.stack([(uv[0] - 0.5) * aspect, uv[1] - 0.5], axis=0)
    add_stars(fx, p, h, time, 0.0, 0.0, np.array([1.0, 0.0], np.float32), 0.45, 0.65,
              np.array([0.85, 0.9, 1.0], np.float32), ((70.0, 0.7, 0.045),), (0.0, 0.0), field=True)
    ca = hex_rgb(0x3CFF78)
    cb = hex_rgb(0xFF4B7A)
    surge = 0.72 + 0.28 * float(vnoise(np.array(t * 0.045), np.array(2.2)))
    sky_room = 1.0 - smoothstep(0.16, 0.44, uv[1])
    fx += aurora_ribbons(px, py, t, 0.72, 0.82, ca, cb, surge, aspect, 0.0, False) * (0.40 * sky_room)
    sky = sky_of(layers, time, w, h) + np.power(np.clip(tonemap(fx), 0.0, 1.0), 2.2)
    return encode_linear(cover(sky, layers, time, w, h, True), w, h, (1.0, 1.0, 0.18, False))


def main():
    import argparse

    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/plates-stills")
    ap.add_argument("--scale", type=float, default=1.0)
    args = ap.parse_args()
    w = max(16, int(1920 * args.scale))
    h = max(16, int(1080 * args.scale))
    os.makedirs(args.out, exist_ok=True)
    from render_sky import presets

    stars, _aurora, _flies, _snow = presets()
    jobs = [
        ("fireflies-summer-dusk", lambda: render_fireflies(w, h, 4.2)),
        ("snow-pine-forest", lambda: render_snow(w, h, 3.1)),
        ("stars-still-night", lambda: render_stars(w, h, 0.48, stars["still-night"])),
        ("stars-milky-way", lambda: render_stars(w, h, 0.48, stars["milky-way"])),
        ("aurora-classic", lambda: render_aurora(w, h, 6.5)),
    ]
    for name, fn in jobs:
        print(f"rendering {name} {w}x{h}", flush=True)
        img = fn()
        path = os.path.join(args.out, name + ".png")
        Image.fromarray(img, "RGB").save(path, optimize=True)
        print(f"  wrote {path}", flush=True)


if __name__ == "__main__":
    main()
