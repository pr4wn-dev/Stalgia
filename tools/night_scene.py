"""Fireflies and Snowfall, second pass.

Pixel sizes are defined at 1920x1080 and scaled by height/1080. Motion is
periodic over 3600s so a clock wrap does not pop. Snow is over-composited,
never added as glow.

The same functions feed the stills, render_sky.py --check, and the HLSL port.
"""

import numpy as np

from render_sky import frac, grade, hash12, hash22, hex_rgb, lerp, saturate, smoothstep, tonemap, vnoise

W0, H0 = 1920, 1080


def lin_hex(v):
    return np.power(np.clip(hex_rgb(v), 0.0, 1.0), 2.2).astype(np.float32)


def srgb_u8(col, uv, w, h, look):
    enc = grade(tonemap(col), uv, w, h, look)
    return np.clip(enc * 255.0 + 0.5, 0, 255).astype(np.uint8).transpose(1, 2, 0)


def scene_luma(col):
    lin = np.power(np.clip(tonemap(col), 0.0, 1.0), 2.2)
    return lin[0] * 0.2126 + lin[1] * 0.7152 + lin[2] * 0.0722


def px_grid(w, h):
    ys, xs = np.indices((h, w), dtype=np.float32)
    return xs + 0.5, ys + 0.5


def uv_of(x, y, w, h):
    return np.stack([x / w, y / h], axis=0)


# Slow rolling hills. A third, shorter octave keeps the soil off a single
# raster row without cutting the edge into flat steps. Grass or a soft snow
# drift supplies the fine silhouette.
_GROUND_AMP = {"far": 28.0, "mid": 36.0, "near": 22.0}


def soil_y(x, h, frac_y, amp, length, seed):
    """Rolling hill. Sines stay curved, so the edge never sits on a flat step."""
    s = max(h / float(H0), 1e-3)
    p = x / s
    length = max(length, 48.0)
    w = (
        np.sin(p * (6.2831853 / length) + seed) * 0.62
        + np.sin(p * (6.2831853 / (length * 0.37)) + seed * 2.1) * 0.28
        + np.sin(p * (6.2831853 / 52.0) + seed * 3.7) * 0.22
    )
    g = frac_y * h + amp * s * w
    return np.clip(g, 8.0, h - 6.0)


def ground_y(x, h, frac_y, amp, length, seed):
    return soil_y(x, h, frac_y, amp, length, seed)


def _tree_widths(t, hh, base, n_tiers, jitter, y, seed, octaves):
    tn = saturate(t)
    prof = base * np.power(np.maximum(1.0 - tn, 0.0), 0.85)
    s = frac(tn * n_tiers + jitter)
    tier = lerp(0.42, 1.0, np.power(np.maximum(1.0 - s, 0.0), 1.6))
    prof = prof * tier
    edge = vnoise(y / 6.0, np.full_like(y, seed)) - 0.5
    prof = prof + base * 0.10 * edge
    if octaves > 1:
        edge2 = vnoise(y / 2.5, np.full_like(y, seed + 4.0)) - 0.5
        prof = prof + base * 0.05 * edge2
    spike = tn > 0.94
    prof = np.where(spike, np.minimum(prof, 1.6), prof)
    return np.maximum(prof, 0.0)


def grass_fringe(x, y, h, frac_y, amp, length, seed, short, tall):
    """Tapered blades along the soil line. Tips come to a point, never a flat shelf."""
    s = h / float(H0)
    # Tufts, not a picket fence: a cell is mostly gap, with two or three blades.
    cell = max(13.0 * s, 5.0)
    cov = np.zeros_like(x, dtype=np.float32)
    col_i = np.floor(x / cell)
    for k in (-1.0, 0.0, 1.0):
        cid = col_i + k
        present = hash12(cid, np.full_like(cid, seed + 3.0)) > 0.28
        for b in (0.0, 1.0, 2.0):
            hs = hash12(cid, np.full_like(cid, seed + 40.0 + b * 2.3))
            keep = present & (hs > (0.08 if b < 2.0 else 0.55))
            u = lerp(0.28, 0.72, hash12(cid, np.full_like(cid, seed + 51.0 + b)))
            cx = (cid + u) * cell
            root = soil_y(cx, h, frac_y, amp, length, seed)
            bh = lerp(short, tall, np.power(np.clip(hs, 0.0, 1.0), 1.7)) * s
            bw = lerp(1.05, 2.05, hash12(cid, np.full_like(cid, seed + 60.0 + b))) * s
            lean = (hash12(cid, np.full_like(cid, seed + 70.0 + b)) - 0.5) * 0.85
            t = (root - y) / np.maximum(bh, 1.0)
            half = bw * np.power(np.maximum(1.0 - t, 0.0), 1.45)
            bow = np.sin(np.clip(t, 0.0, 1.0) * 3.1416) * lean * bh * 0.15
            dx = np.abs(x - cx - lean * (root - y) - bow)
            on = keep & (t > 0.0) & (t < 1.0)
            cov = np.maximum(cov, saturate(half - dx + 0.5) * on.astype(np.float32))
    return cov


def conifer_card(x, y, h, frac_g, amp, length, cell, seed, h_lo, h_hi, tiers, octaves, trunk, cypress,
                 edge="grass", blade=(8.0, 28.0), soft=0.0, gates=(0.08, 0.55)):
    """Opaque from a rolling soil line down, with tiered conifers above it.

    edge='grass' adds a pointed blade fringe. edge='snow' feathers the soil
    into a drift. edge='none' is a hard fill with no fringe.
    """
    s = h / float(H0)
    g = soil_y(x, h, frac_g, amp, length, seed + 0.2)
    if edge == "snow":
        feather = max(8.0 * s, 2.0)
        card = smoothstep(g - feather, g + feather * 0.30, y)
    else:
        card = saturate(y - g + 0.5)
        if edge == "grass":
            card = np.maximum(
                card,
                grass_fringe(x, y, h, frac_g, amp, length, seed + 0.2, blade[0], blade[1]),
            )
    cell_px = max(cell * s, 4.0)
    col_i = np.floor(x / cell_px)
    for k in (-1.0, 0.0, 1.0):
        cell_id = col_i + k
        for ti in (0.0, 1.0):
            hs = hash12(cell_id, np.full_like(cell_id, seed + ti * 3.0))
            h2 = hash12(cell_id, np.full_like(cell_id, seed + 5.0 + ti))
            gate = gates[0] if ti == 0.0 else gates[1]
            present = hs > gate
            u = lerp(0.08, 0.92, hs)
            if ti == 1.0:
                u = frac(u + 0.37)
            center = (cell_id + u) * cell_px
            tall = hash12(cell_id, np.full_like(cell_id, seed + 19.0)) > 0.82
            hh = (h_lo + (h_hi - h_lo) * np.power(h2, 0.7)) * h
            hh = np.where(tall, hh * 1.28, hh)
            g_c = soil_y(center, h, frac_g, amp, length, seed + 0.2)
            root = g_c + 0.02 * hh
            t = (root - y) / np.maximum(hh, 1.0)
            base = hh * lerp(0.10 if cypress else 0.15, 0.13 if cypress else 0.22, hs)
            narrow = hash12(cell_id, np.full_like(cell_id, seed + 23.0)) > (0.72 if cypress else 1.1)
            base = np.where(narrow, hh * 0.09, base)
            n_tiers = lerp(tiers[0], tiers[1], h2)
            jitter = hash12(cell_id, np.full_like(cell_id, seed + 8.0 + ti))
            half = _tree_widths(t, hh, base, n_tiers, jitter, y, seed + 30.0 + ti * 9.0, octaves)
            lean = (hash12(cell_id, np.full_like(cell_id, seed + 11.0)) - 0.5) * 0.06 * hh * saturate(t)
            dx = np.abs(x - center - lean)
            on = present & (t > 0.0) & (t < 1.0)
            if soft > 0.0:
                cov = smoothstep(-soft, 1.25, half - dx) * on.astype(np.float32)
            else:
                cov = saturate(half - dx + 0.5) * on.astype(np.float32)
            if trunk and ti == 0.0:
                tw = lerp(3.0, 8.0, hs) * s
                trunk_on = present & (y < root) & (y > root - hh * 0.22) & (np.abs(x - center) < tw)
                cov = np.maximum(cov, trunk_on.astype(np.float32))
            card = np.maximum(card, cov)
    return card, g


def grass_tufts(x, y, h, g, seed):
    s = h / float(H0)
    cell = 14.0 * s
    cov = np.zeros_like(x)
    col_i = np.floor(x / cell)
    for k in (-1.0, 0.0, 1.0):
        cid = col_i + k
        for b in (0.0, 1.0, 2.0):
            hs = hash12(cid, np.full_like(cid, seed + b * 2.0))
            if b == 2.0:
                keep = hs > 0.35
            else:
                keep = hs > 0.05
            u = lerp(0.15, 0.85, hash12(cid, np.full_like(cid, seed + 4.0 + b)))
            cx = (cid + u) * cell
            bh = lerp(16.0, 62.0, np.power(np.clip(hs, 0.0, 1.0), 1.2)) * s
            bw = lerp(1.3, 2.6, hash12(cid, np.full_like(cid, seed + 6.0 + b))) * s
            lean = (hash12(cid, np.full_like(cid, seed + 7.0 + b)) - 0.5) * 0.42
            root = g
            t = (root - y) / np.maximum(bh, 1.0)
            half = bw * np.power(np.maximum(1.0 - t, 0.0), 1.2)
            dx = np.abs(x - cx - lean * (root - y))
            on = keep & (t > 0.0) & (t < 1.0)
            cov = np.maximum(cov, saturate(half - dx + 0.5) * on.astype(np.float32))
    return cov


def mist_band(x, y, h, g, time, alpha, color, speed_px):
    s = h / float(H0)
    half = lerp(0.06, 0.12, 0.5) * h
    dy = y - g
    vert = np.exp(-(dy * dy) / max(half * half, 1.0))
    scroll = (time / 3600.0) * speed_px * 3600.0 * s
    n = 0.55 * vnoise((x + scroll) / (380.0 * s), y / (140.0 * s))
    n = n + 0.45 * vnoise((x + scroll) / (190.0 * s) + 1.7, y / (70.0 * s) + 2.2)
    dens = smoothstep(0.35, 0.75, n)
    a = np.clip(alpha * vert * dens, 0.0, 1.0)
    return a, color


def blink_env(time, phase, pattern):
    """Three blinks. Periods divide 3600s. pattern is 0..1 per fly."""
    period = np.where(pattern < 0.55, 6.0, np.where(pattern < 0.80, 5.0, 8.0))
    u = frac(time / 3600.0 * (3600.0 / period) + phase)
    slow = smoothstep(0.0, 0.03, u) * (1.0 - smoothstep(0.42, 0.55, u))
    a = smoothstep(0.04, 0.07, u) * (1.0 - smoothstep(0.14, 0.18, u))
    b = smoothstep(0.24, 0.27, u) * (1.0 - smoothstep(0.34, 0.40, u))
    double = np.clip(a + b, 0.0, 1.0)
    stroke = smoothstep(0.0, 0.025, u) * (1.0 - smoothstep(0.28, 0.40, u))
    return np.where(pattern < 0.55, slow, np.where(pattern < 0.80, double, stroke))


def fly_grid(w, h, y0, y1, count, kind, speed, density):
    """Cell layout shared by the renderer and the C19 reach check. kind is 0/1/2."""
    s = h / float(H0)
    sp_k = speed / 0.32
    amp_k = float(np.clip(sp_k, 0.25, 1.5))
    k_mul = max(1.0, round(sp_k / 1.5))
    n = max(count * max(density, 0.05) * s, 1.0)
    base_ax = 10.0 if kind == 0 else 20.0 if kind == 1 else 36.0
    base_ay = 7.0 if kind == 0 else 14.0 if kind == 1 else 16.0
    ax = base_ax * s * amp_k
    ay = base_ay * s * amp_k
    b_max = (6.0 if kind == 0 else 10.0 if kind == 1 else 22.0) * s
    reach_y = 3.0 * b_max + ay + 6.0 * s + 1.0
    reach_x = 3.0 * b_max + ax + 1.0
    band_h = max(y1 - y0, 8.0)
    cols = n if kind == 2 else max(1.0, (n * w / max(h, 1.0)) ** 0.5)
    rows = 1.0 if kind == 2 else max(1.0, float(np.ceil(n / max(cols, 1.0))))
    rows = min(rows, max(1.0, float(np.floor(band_h * 1.15 / max(reach_y, 1.0)))))
    cols = max(cols, float(np.ceil(n / max(rows * 0.85, 1.0))))
    cols = min(cols, max(1.0, float(np.floor(w * 1.15 / max(reach_x, 1.0)))))
    return dict(n=n, cols=cols, rows=rows, cw=w / max(cols, 1.0), ch=band_h / max(rows, 1.0),
                ax=ax, ay=ay, reach_x=reach_x, reach_y=reach_y, k_mul=k_mul, b_max=b_max, s=s)


def add_fireflies(col, x, y, w, h, time, speed, density, tint, band_lo, band_hi, count, kind, mask_a):
    """kind: 'far', 'mid', 'near'. A 3x3 search so the glow is not cut on the cell edge."""
    kind_i = 0 if kind == "far" else 1 if kind == "mid" else 2
    y0 = band_lo * h
    y1 = band_hi * h
    g = fly_grid(w, h, y0, y1, count, kind_i, speed, density)
    s, cw, ch = g["s"], g["cw"], g["ch"]
    y_lo = max(0, int(np.floor(y0 - g["reach_y"])) - 1)
    y_hi = min(h, int(np.ceil(y1 + g["reach_y"])) + 1)
    if y_hi <= y_lo:
        return mask_a
    sl = (slice(y_lo, y_hi), slice(None))
    xx, yy = x[sl], y[sl]
    cell_x = np.floor(xx / cw)
    cell_y = np.floor((yy - y0) / ch)
    white = lin_hex(0xFFF6C8)
    occ = min(0.92, g["n"] / max(g["cols"] * g["rows"], 1.0))
    for jy in (-1.0, 0.0, 1.0):
        for jx in (-1.0, 0.0, 1.0):
            ix = cell_x + jx
            iy = cell_y + jy
            hs = hash12(ix, iy + 3.1)
            alive = hs > (1.0 - occ)
            jx2, jy2 = hash22(ix + 2.0, iy + 1.0)
            cx = (ix + 0.15 + 0.7 * jx2) * cw
            cy = y0 + (iy + 0.15 + 0.7 * jy2) * ch
            k1 = (180.0 + np.floor(hs * 200.0)) * g["k_mul"]
            k2 = (260.0 + np.floor(hash12(ix, iy + 9.0) * 180.0)) * g["k_mul"]
            ang = 6.2831853 * time / 3600.0
            pattern = hash12(ix, iy + 4.0)
            env = blink_env(time, hs, pattern)
            cx = cx + g["ax"] * np.sin(ang * k1 + hs * 6.0)
            cy = cy + g["ay"] * np.sin(ang * k2 + hs * 4.0)
            cy = cy - np.where(pattern > 0.80, 14.0, 6.0) * s * env
            dist = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
            if kind_i == 2:
                rad = lerp(11.0, 20.0, hs) * s
                edge = 3.5 * s
                disc = smoothstep(rad + edge, np.maximum(rad - edge, 0.0), dist)
                rim = smoothstep(rad, rad - 2.2 * s, dist) * smoothstep(rad - 5.5 * s, rad - 2.2 * s, dist)
                shade = (disc * 0.20 + rim * 0.07) * env
                level = 1.0
                glow = tint * 0.8
            else:
                core_r = (lerp(0.7, 1.05, hs) if kind_i == 0 else lerp(1.2, 1.9, hs)) * s
                bloom = (lerp(4.0, 6.0, hs) if kind_i == 0 else lerp(7.0, 10.0, hs)) * s
                r0 = (2.6 if kind_i == 0 else 3.6) * s
                core = np.exp(-(dist * dist) / np.maximum(core_r * core_r, 0.2))
                halo = np.power(1.0 / (1.0 + (dist / r0) ** 2), 1.5)
                window = np.exp(-(dist * dist) / np.maximum(bloom * bloom, 0.4))
                peak, gain = (1.05, 0.28) if kind_i == 0 else (1.7, 0.42)
                shade = (core * peak + halo * window * gain) * env
                level = 0.45
                glow = tint
            pad = max(8.0 * s, 2.0)
            edge_fade = smoothstep(y0 - pad - 12.0 * s, y0 - pad, cy) * (
                1.0 - smoothstep(y1 + pad, y1 + pad + 12.0 * s, cy)
            )
            shade = np.where(alive, shade * edge_fade, 0.0)
            env_s = np.where(alive, env, 0.0)
            for c in range(3):
                base = glow[c] + (white[c] - glow[c]) * 0.62 * env_s
                col[c][sl] += base * shade
            mark = (shade > 0.05).astype(np.float32)
            mask_a[sl] = np.maximum(mask_a[sl], mark * level)
    return mask_a


def composite_disc(col, dist, rad, blur, alpha, color):
    a = smoothstep(rad + blur, max(rad - blur, 0.0), dist) * alpha
    a = np.clip(a, 0.0, 1.0)
    for c in range(3):
        col[c] = col[c] * (1.0 - a) + color[c] * a
    return a


def snow_layer(col, x, y, w, h, time, speed, wind, density, spec, tint, mask=None):
    """spec: cellx, celly, diam_lo, diam_hi, blur, alpha_lo, alpha_hi, fall, sway, freq, stretch, count."""
    s = h / float(H0)
    cell_x = spec["cellx"] * s
    cell_y = spec["celly"] * s
    fall = spec["fall"] * (speed / 0.40) * s
    # Snap the drift so 3600s is an integer number of cells (no wrap pop).
    quantum_y = cell_y / 3600.0
    fall_q = np.round(fall / quantum_y) * quantum_y
    wind_rate = wind * fall_q * 0.7
    quantum_x = cell_x / 3600.0
    wind_q = np.round(wind_rate / max(quantum_x, 1e-8)) * quantum_x
    shift_y = fall_q * time
    shift_x = wind_q * time
    ix0 = np.floor((x - shift_x) / cell_x)
    covered = np.zeros(x.shape, dtype=bool)
    for jx in (-1.0, 0.0, 1.0):
        cid_x = ix0 + jx
        jit = 1.0 + 0.2 * (hash12(cid_x, np.full_like(cid_x, spec["seed"] + 3.0)) - 0.5)
        fall_c = np.round(fall_q * jit / max(quantum_y, 1e-8)) * quantum_y
        shift_c = fall_c * time
        cid_y = np.floor((y - shift_c) / cell_y)
        hs = hash12(cid_x + 2.0, cid_y + spec["seed"])
        # Occupancy is defined at 1080p. Scale it with height so a half-res
        # check still lands in the H^2 component window.
        keep = np.clip(
            spec["count"] * density * (h / float(H0))
            / max((W0 / spec["cellx"]) * (H0 / spec["celly"]), 1.0),
            0.02, 0.98,
        )
        alive = hs > (1.0 - keep)
        jx2, jy2 = hash22(cid_x + spec["seed"], cid_y + 0.4)
        cx = (cid_x + 0.5 + (jx2 - 0.5) * 0.3) * cell_x + shift_x
        # per-column fall jitter, quantized
        cy = (cid_y + 0.5 + (jy2 - 0.5) * 0.25) * cell_y + shift_c
        freq = spec["freq"]
        cycles = max(1.0, np.round(freq * 3600.0))
        sway = spec["sway"] * s * np.sin(6.2831853 * time / 3600.0 * cycles + hs * 6.2)
        cx = cx + sway
        diam = lerp(spec["d0"], spec["d1"], hs) * s
        rad = diam * 0.5
        blur = max(spec["blur"] * s, 0.45 * s)
        dx = x - cx
        dy = (y - cy) / spec["stretch"]
        dist = np.sqrt(dx * dx + dy * dy)
        alpha = lerp(spec["a0"], spec["a1"], hash12(cid_x, cid_y + 8.0))
        vary = lerp(0.92, 1.0, hs)
        shade_col = tint[:, None, None] * vary
        edge = smoothstep(rad + blur, np.maximum(rad - blur, 0.0), dist)
        core = np.exp(-(dist * dist) / np.maximum(rad * rad * 0.28, 0.2))
        a = edge * alpha * lerp(0.62, 1.0, core)
        a = np.where(alive, np.clip(a, 0.0, 1.0), 0.0)
        if mask is not None:
            a = np.where(mask, a, 0.0)
        for c in range(3):
            col[c] = col[c] * (1.0 - a) + shade_col[c] * a
        covered |= a > 0.15
    return covered


def meadow_soil(y, h):
    """Open field. Lighter at the horizon, darker toward the camera."""
    t = saturate((y / h - 0.56) / 0.44)
    hi = lin_hex(0x8A9474)
    mid = lin_hex(0x46543C)
    lo = lin_hex(0x161E14)
    u = saturate(t * 1.35)
    v = saturate((t - 0.32) / 0.68)
    c = lerp(hi[:, None, None], mid[:, None, None], u)
    return lerp(c, lo[:, None, None], v)


def sky_gradient(y, h, stops):
    """stops: list of (frac_y, hex) top to bottom."""
    t = y / h
    col = np.zeros((3,) + y.shape, np.float32)
    acc = np.zeros(y.shape, np.float32)
    for i in range(len(stops) - 1):
        y0, c0 = stops[i]
        y1, c1 = stops[i + 1]
        span = np.clip((t - y0) / max(y1 - y0, 1e-4), 0.0, 1.0)
        band = (t >= y0) & (t <= y1)
        rgb = lerp(lin_hex(c0)[:, None, None], lin_hex(c1)[:, None, None], span)
        for c in range(3):
            col[c] = np.where(band, rgb[c], col[c])
        acc = np.where(band, 1.0, acc)
    return col


LAYERS = {
    "summer": dict(far=(0.58, 0.12, 0.24, 86.0, (8, 12)), mid=(0.84, 0.22, 0.46, 150.0, (7, 11)),
                   near=(0.88, 0.16, 0.34, 280.0, (6, 10)),
                   far_c=0x2A2438, mid_c=0x141320, near_c=0x12141C,
                   sky=[(0.0, 0x1A2458), (0.30, 0x3A3568), (0.55, 0x6A4060), (0.74, 0xC46A48)],
                   horizon=0xE8935A, mist=0x7A7F96, mist_a=(0.18, 0.12),
                   flies=(18, 18, 2), stars=75, glow=0xD2F26A),
    "deep": dict(far=(0.52, 0.14, 0.28, 84.0, (9, 13)), mid=(0.74, 0.22, 0.42, 140.0, (8, 12)),
                 near=(0.84, 0.28, 0.48, 280.0, (8, 12)),
                 far_c=0x1C2A2E, mid_c=0x0F181B, near_c=0x10161A,
                 sky=[(0.0, 0x152838), (0.45, 0x243E48), (0.72, 0x2E4A52)],
                 horizon=0x1E3138, mist=0x4A6A6A, mist_a=(0.28, 0.18),
                 flies=(18, 20, 4), stars=0, glow=0xB6E86A),
    "meadow": dict(far=(0.68, 0.05, 0.12, 90.0, (7, 11)), mid=None,
                   near=(0.90, 0.06, 0.12, 260.0, (6, 9)),
                   far_c=0x5A546C, mid_c=0x243028, near_c=0x121614,
                   sky=[(0.0, 0x0A1236), (0.40, 0x2B2C5E), (0.62, 0x8A4030), (0.75, 0xC46A40)],
                   horizon=0xC48462, mist=0x8A8FA0, mist_a=(0.12, 0.0),
                   flies=(22, 22, 2), stars=170, glow=0xE8FF8A),
}


def render_fireflies(w, h, style, time, density, speed, mist_k, stars_on, glow, look, mask=False, flies=True):
    x, y = px_grid(w, h)
    p = LAYERS[style]
    col = sky_gradient(y, h, p["sky"])
    # Warm horizon row just above the far ground.
    s = h / float(H0)
    g_far = p["far"][0] * h
    dy = (y - (g_far - 0.02 * h)) / h
    band = np.exp(-(dy * dy) / (0.03 * 0.03)) + 0.35 * np.exp(-(dy * dy) / (0.12 * 0.12))
    hz = lin_hex(p["horizon"])
    for c in range(3):
        col[c] += hz[c] * band * (0.55 if style != "deep" else 0.18)
    masks = [np.zeros((h, w), np.float32) for _ in range(3)]
    mask_a = np.zeros((h, w), np.float32)
    order = [("far", 0), ("mid", 1), ("near", 2)]
    colors = [p["far_c"], p["mid_c"], p["near_c"]]
    if style == "meadow":
        amps = [16.0, 18.0, 12.0]
        blades = [(8.0, 28.0), (10.0, 36.0), (14.0, 70.0)]
    elif style == "deep":
        amps = [30.0, 36.0, 20.0]
        blades = [(6.0, 22.0), (8.0, 30.0), (10.0, 40.0)]
    else:
        amps = [34.0, 40.0, 24.0]
        blades = [(7.0, 26.0), (10.0, 38.0), (12.0, 52.0)]
    lengths = [480.0, 340.0, 260.0]
    seeds = [2.0, 6.5, 11.0]
    tree_hex = {0: 0x4C445E, 1: 0x221E30, 2: 0x121018}
    if style == "meadow":
        tree_hex = {0: 0x5A546C, 1: 0x314033, 2: 0x121810}
    for name, idx in order:
        spec = p[name]
        if spec is None:
            # Meadow has no second treeline. The card is the field itself:
            # a few low shrubs, tall grass, and a soil gradient.
            frac_g = 0.76
            card, g = conifer_card(
                x, y, h, frac_g, amps[idx], 360.0, 280.0, 6.5,
                0.055, 0.14, (6, 10), 1, False, False,
                edge="grass", blade=blades[idx], gates=(0.28, 0.75),
            )
        else:
            frac_g, h_lo, h_hi, cell, tiers = spec
            card, g = conifer_card(
                x, y, h, frac_g, amps[idx], lengths[idx], cell, seeds[idx],
                h_lo, h_hi, tiers, 2 if idx else 1, idx == 2, style == "summer" and idx > 0,
                edge="grass", blade=blades[idx],
            )
            if style == "meadow" and idx == 2:
                card = np.maximum(card, grass_tufts(x, y, h, g, 12.0))
        masks[idx] = card
        above = y < g - 1.0
        tree = lin_hex(tree_hex[idx])
        field = meadow_soil(y, h) if style == "meadow" else None
        tint = lin_hex(colors[idx])
        for c in range(3):
            soil = field[c] if field is not None else tint[c]
            if style == "meadow" and idx == 1:
                # The middle of the field is the gradient itself. Blades here
                # would draw a second dark horizon across the grass.
                shade = soil
            else:
                shade = np.where(above, tree[c], soil)
            col[c] = lerp(col[c], shade, np.clip(card, 0.0, 1.0))
        mist_a = p["mist_a"][idx] if idx < 2 else 0.0
        if style == "meadow" and idx < 2:
            mist_a = 0.28 if idx == 0 else 0.34
        if mist_a > 0 and mist_k > 0:
            a, mc = mist_band(x, y, h, g, time, mist_a * (mist_k / 0.55), lin_hex(p["mist"]), 6.0)
            for c in range(3):
                col[c] = lerp(col[c], mc[c], a)
        if flies and p["flies"][idx] > 0 and spec is not None:
            lo = max(0.36 if style != "meadow" else 0.48, frac_g - (0.22 if style != "meadow" else 0.08))
            hi = min(0.96, frac_g + 0.02)
            kind = "far" if idx == 0 else ("near" if idx == 2 else "mid")
            mask_a = add_fireflies(
                col, x, y, w, h, time, speed, density, lin_hex(glow),
                lo, hi, p["flies"][idx], kind, mask_a,
            )
        elif flies and p["flies"][idx] > 0:
            mask_a = add_fireflies(
                col, x, y, w, h, time, speed, density, lin_hex(glow),
                0.58, 0.80, p["flies"][idx], "mid", mask_a,
            )
    if stars_on and p["stars"] and flies:
        _stars(col, x, y, w, h, p["stars"], 0.42 if style == "meadow" else 0.50)
    # Exposure so the graded still sits in the art-direction luma window.
    col *= 2.6 if style == "deep" else (2.20 if style == "summer" else 1.9)
    if mask:
        out = np.stack([masks[0], masks[1], masks[2], mask_a], axis=0)
        return out
    return col


def _stars(col, x, y, w, h, count, y_max_frac):
    s = h / float(H0)
    cell = 70.0 * s
    region = y < y_max_frac * h
    ix = np.floor(x / cell)
    iy = np.floor(y / cell)
    n = hash12(ix + 4.0, iy + 8.0)
    cells = (w / cell) * (y_max_frac * h / cell)
    keep = np.clip(count / max(cells, 1.0), 0.02, 0.85)
    on = (n > 1.0 - keep) & region
    jx, jy = hash22(ix, iy)
    cx = (ix + jx) * cell
    cy = (iy + jy) * cell
    dist = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    core = np.exp(-(dist * dist) / 1.2) * on
    for c, gain in enumerate((0.75, 0.82, 1.0)):
        col[c] += core * gain * lerp(0.4, 1.3, n)


SNOW_SPECS = [
    dict(cellx=52, celly=44, d0=1.4, d1=2.2, blur=0.55, a0=0.40, a1=0.55, fall=32.0,
         sway=4.5, freq=0.70, stretch=1.0, count=900, seed=1.0),
    dict(cellx=96, celly=84, d0=3.2, d1=5.2, blur=0.9, a0=0.62, a1=0.82, fall=64.0,
         sway=12.0, freq=0.50, stretch=1.0, count=340, seed=2.0),
    dict(cellx=180, celly=160, d0=6.0, d1=9.0, blur=1.3, a0=0.75, a1=0.90, fall=112.0,
         sway=22.0, freq=0.35, stretch=1.28, count=80, seed=3.0),
    dict(cellx=520, celly=480, d0=18.0, d1=30.0, blur=4.0, a0=0.10, a1=0.18, fall=200.0,
         sway=36.0, freq=0.25, stretch=1.25, count=5, seed=4.0),
]


def render_snow(w, h, backdrop, time, density, speed, wind, flake, look, mask=False):
    x, y = px_grid(w, h)
    s = h / float(H0)
    masks = [np.zeros((h, w), np.float32) for _ in range(4)]
    if backdrop == 0:
        col = sky_gradient(y, h, [(0.0, 0x1A2C4A), (0.55, 0x243850), (1.0, 0x3A4E68)])
        col *= 2.4
        n = 0.6 * vnoise(x / (600 * s) + time * 3.0 * s / 600.0, y / (400 * s))
        n += 0.4 * vnoise(x / (280 * s) + 2.0, y / (200 * s))
        cloud = lin_hex(0x142033)
        for c in range(3):
            col[c] = lerp(col[c], cloud[c], np.clip(n, 0, 1) * 0.35)
        mx, my = 0.72 * w, 0.22 * h
        moon = np.exp(-((x - mx) ** 2 + (y - my) ** 2) / (2 * (0.18 * h) ** 2))
        glow = lin_hex(0x9DB4D6)
        for c in range(3):
            col[c] = lerp(col[c], glow[c], moon * 0.22)
        moon_r = 17.0 * s
        disc = smoothstep(moon_r + 1.6 * s, moon_r - 1.2 * s, np.sqrt((x - mx) ** 2 + (y - my) ** 2))
        moon_c = lin_hex(0xD7DCEB)
        for c in range(3):
            col[c] = lerp(col[c], moon_c[c], disc * 0.72)
        # Soft hills and a few faint pines. The snow field covers the tree fill
        # so there is no dark band under the tips.
        g = soil_y(x, h, 0.80, 22.0, 480.0, 4.0)
        tree, _tg = conifer_card(
            x, y, h, 0.80, 22.0, 480.0, 150.0, 4.2,
            0.02, 0.045, (5, 8), 1, False, False,
            edge="none", soft=max(3.5 * s, 1.2), gates=(0.55, 0.9),
        )
        tips = np.clip(tree, 0.0, 1.0) * (y < g - 1.0).astype(np.float32)
        far_pine = lin_hex(0x1C2838) * 2.3
        for c in range(3):
            col[c] = lerp(col[c], far_pine[c], tips * 0.72)
        feather = max(12.0 * s, 2.0)
        hill = smoothstep(g - feather, g + feather * 0.2, y)
        top = lin_hex(0x33415A) * 2.2
        bot = lin_hex(0x4A5A74) * 2.2
        field = lerp(top[:, None, None], bot[:, None, None], saturate((y / h - 0.76) / 0.24))
        crest = np.exp(-((y - g) / max(8.0 * s, 1.0)) ** 2) * 0.10
        for c in range(3):
            col[c] = lerp(col[c], field[c] * (1.0 + crest), hill)
        ambient = lin_hex(0xB9C6DA)
        glass = np.ones((h, w), dtype=bool)
    elif backdrop == 1:
        col = sky_gradient(y, h, [(0.0, 0x0E1828), (1.0, 0x26364C)])
        col *= 3.8
        specs = [
            (0.60, 0.12, 0.26, 80.0, (8, 11), 0x2B3A4E, 2.0, 1),
            (0.74, 0.20, 0.40, 130.0, (8, 11), 0x18222F, 6.0, 2),
            (0.90, 0.28, 0.50, 240.0, (8, 11), 0x0B1018, 11.0, 2),
        ]
        amps = [24.0, 32.0, 26.0]
        ambient = lin_hex(0xC2CEDC)
        glass = np.ones((h, w), dtype=bool)
        for i, (fg, lo, hi, cell, tiers, color, seed, octv) in enumerate(specs):
            card, g = conifer_card(
                x, y, h, fg, amps[i], 420.0, cell, seed, lo, hi, tiers, octv, i == 2, False,
                edge="snow",
            )
            masks[i] = card
            tint = lin_hex(color) * 3.2
            for c in range(3):
                col[c] = lerp(col[c], tint[c], np.clip(card, 0, 1))
            if i < 2:
                _snow_layers_upto(col, x, y, w, h, time, speed, wind, density, lin_hex(flake) * ambient, i)
        g = soil_y(x, h, 0.90, amps[2], 280.0, 11.2)
        field = lerp(lin_hex(0x3A4A60)[:, None, None], lin_hex(0x6A7C96)[:, None, None], saturate((y / h - 0.86) / 0.14))
        field *= 3.0
        feather = max(11.0 * s, 2.0)
        floor = smoothstep(g - feather, g + feather * 0.2, y)
        crest = np.exp(-((y - g) / max(7.0 * s, 1.0)) ** 2)
        lit = field * (1.0 + 0.18 * crest)
        for c in range(3):
            col[c] = lerp(col[c], lit[c], floor)
        # redraw near snow in front of the far trees only; L2 after the near card
        _snow_one(col, x, y, w, h, time, speed, wind, density, lin_hex(flake) * ambient, 2, None)
        _snow_one(col, x, y, w, h, time, speed, wind, density, lin_hex(flake) * ambient, 3, None)
        if mask:
            return np.stack([masks[0], masks[1], masks[2], np.zeros_like(masks[0])], axis=0)
        return col
    else:
        return _window(x, y, w, h, time, density, speed, wind, flake, mask)
    tint = lin_hex(flake) * ambient
    for i in range(4):
        _snow_one(col, x, y, w, h, time, speed, wind, density, tint, i, None if backdrop == 0 else glass)
    if mask:
        return np.stack([masks[0], masks[1], masks[2], np.zeros_like(masks[0])], axis=0)
    return col


def _snow_one(col, x, y, w, h, time, speed, wind, density, tint, index, mask):
    snow_layer(col, x, y, w, h, time, speed, wind, density, SNOW_SPECS[index], tint, mask)


def _snow_layers_upto(col, x, y, w, h, time, speed, wind, density, tint, index):
    _snow_one(col, x, y, w, h, time, speed, wind, density, tint, index, None)


def _window(x, y, w, h, time, density, speed, wind, flake, mask):
    s = h / float(H0)
    lamp = lin_hex(0xFF9E4F)
    d = np.sqrt((x + 0.10 * w) ** 2 + (y - 0.95 * h) ** 2)
    lamp_a = 0.30 / (1.0 + (d / (0.45 * h)) ** 2)
    wall_far = lin_hex(0x140C08)
    wall_near = lin_hex(0x2A1810)
    col = np.zeros((3, h, w), np.float32)
    for c in range(3):
        col[c] = lerp(wall_far[c], wall_near[c], np.clip(lamp_a / 0.30, 0, 1))
        col[c] += lamp[c] * lamp_a
    ow, oh = 1240 * s, 820 * s
    cx, cy = 0.52 * w, 0.47 * h
    x0, y0, x1, y1 = cx - ow / 2, cy - oh / 2, cx + ow / 2, cy + oh / 2
    frame_w = 34 * s
    opening = (x >= x0) & (x <= x1) & (y >= y0) & (y <= y1)
    outer = (x >= x0 - frame_w) & (x <= x1 + frame_w) & (y >= y0 - frame_w) & (y <= y1 + 60 * s)
    pane_w = ow / 2.0
    pane_h = oh / 3.0
    mul = 12 * s
    mx = np.abs((x - x0) - pane_w) < mul * 0.5
    my0 = np.abs((y - y0) - pane_h) < mul * 0.5
    my1 = np.abs((y - y0) - 2 * pane_h) < mul * 0.5
    mullion = opening & (mx | my0 | my1)
    frame = (outer & ~opening) | mullion
    glass = opening & ~mullion
    # Lamp falls off toward the right. Left stile and left mullion carry the warmth.
    left = saturate((cx - x) / (0.62 * ow) + 0.05)
    wood = lin_hex(0x2E1C12)
    wood_lit = lin_hex(0x6A4428)
    mix = saturate(0.22 + 0.70 * left + lamp_a * 1.6)
    grain = 1.0 + (vnoise(x / max(2.2 * s, 1.0), y / max(58.0 * s, 1.0)) - 0.5) * 0.12
    wood_col = lerp(wood[:, None, None], wood_lit[:, None, None], mix) * grain
    # Bevel is a shade of the wood itself: lamp-facing edges lighter, far edges darker.
    lip = 2.2 * s
    facing = (
        (np.abs(x - (x0 - frame_w)) < lip)
        | (np.abs(x - (x0 + pane_w - mul * 0.5)) < lip)
        | (np.abs(y - (y1 + 60 * s)) < lip)
    )
    away = (
        (np.abs(x - (x1 + frame_w)) < lip)
        | (np.abs(x - (x0 + pane_w + mul * 0.5)) < lip)
        | (np.abs(y - (y0 - frame_w)) < lip)
    )
    inner_lip = (
        (np.abs(x - x0) < lip) | (np.abs(x - x1) < lip) | (np.abs(y - y0) < lip) | (np.abs(y - y1) < lip)
    ) & frame
    shade = np.ones_like(x)
    shade = np.where(frame & facing, 1.28, shade)
    shade = np.where(frame & away, 0.62, shade)
    shade = np.where(inner_lip, shade * 0.78, shade)
    for c in range(3):
        col[c] = np.where(frame, wood_col[c] * shade, col[c])
        col[c] = np.where(frame, col[c] + lamp[c] * lamp_a * (0.55 + 0.9 * left), col[c])
    sill = (x >= x0 - frame_w) & (x <= x1 + frame_w) & (y >= y1) & (y <= y1 + 60 * s)
    sill_top = sill & (y <= y1 + 18 * s)
    sill_c = lin_hex(0x6A4630)
    for c in range(3):
        body = wood[c] * (0.85 + 0.35 * left) + lamp[c] * lamp_a * (0.8 + left)
        top = sill_c[c] * (0.9 + 0.4 * left) + lamp[c] * (0.06 + 0.12 * left)
        col[c] = np.where(sill, body, col[c])
        col[c] = np.where(sill_top, top, col[c])
    out = _window_outdoors(x, y, w, h, s, x0, y0, ow, oh, time, density, speed, wind, flake, glass, lamp)
    frost = _frost(x, y, x0, y0, pane_w, pane_h, mul, glass, s)
    frost_c = lin_hex(0xD7E6F3)
    warm_f = frost_c * 0.82 + lin_hex(0xFFC89A) * 0.18
    use_warm = (x < x0 + pane_w) & (y > y0 + 1.2 * pane_h)
    for c in range(3):
        fc = np.where(use_warm, warm_f[c], frost_c[c])
        out[c] = lerp(out[c], fc, np.clip(frost * 0.62, 0, 1))
    sill_snow = _sill_snow(x, y, x0, x1, y1, s)
    col *= 4.2
    for c in range(3):
        col[c] = np.where(glass, out[c], col[c])
    ss = lin_hex(0xE7F0F8) * 1.25
    for c in range(3):
        pile = ss[c] * (0.92 + 0.10 * left) + lamp[c] * 0.05 * left
        col[c] = np.where(sill_snow > 0.02, lerp(col[c], pile, sill_snow), col[c])
    masks = [
        frame.astype(np.float32),
        glass.astype(np.float32),
        frost * glass.astype(np.float32),
        sill_snow,
    ]
    if mask:
        return np.stack(masks, axis=0)
    return col


def _window_outdoors(x, y, w, h, s, x0, y0, ow, oh, time, density, speed, wind, flake, glass, lamp):
    """Snowy night outside the glass: sky, soft pines, lit snow, neighbor windows."""
    rel = saturate((y - y0) / max(oh, 1.0))
    sky_top = lin_hex(0x1A2740) * 2.8
    sky_hor = lin_hex(0x3C5678) * 3.8
    out = np.zeros((3,) + x.shape, np.float32)
    sky = lerp(sky_top[:, None, None], sky_hor[:, None, None], np.power(rel, 0.75))
    for c in range(3):
        out[c] = sky[c]
    # Horizon sits inside the bottom row of panes, so those panes are glass
    # looking out at snow and trees, not a flat blue field.
    frac = (y0 + 0.76 * oh) / h
    card, _g = conifer_card(
        x, y, h, frac, 14.0, 300.0, 78.0, 21.0,
        0.035, 0.08, (6, 9), 1, False, False,
        edge="none", soft=max(4.5 * s, 1.4), gates=(0.15, 0.62),
    )
    tree = lin_hex(0x152030) * 2.4
    for c in range(3):
        out[c] = lerp(out[c], tree[c], np.clip(card, 0, 1) * 0.82)
    g = soil_y(x, h, frac, 12.0, 340.0, 21.2)
    feather = max(36.0 * s, 4.0)
    drift = smoothstep(g - feather, g + feather * 0.35, y)
    snow_a = lin_hex(0x3E5674) * 3.2
    snow_b = lin_hex(0x5A7494) * 3.5
    field = lerp(snow_a[:, None, None], snow_b[:, None, None], saturate((y - g) / max(0.26 * oh, 1.0)))
    for c in range(3):
        out[c] = lerp(out[c], field[c], drift)
    warm = lin_hex(0xFFB46A)
    core_c = lin_hex(0xFFD8B0)
    for i, (fx, fy, rad) in enumerate(((0.30, 0.70, 10.0), (0.58, 0.66, 8.0), (0.80, 0.72, 12.0))):
        px, py = x0 + fx * ow, y0 + fy * oh
        dist = np.sqrt((x - px) ** 2 + (y - py) ** 2)
        disc = smoothstep((rad + 5.0) * s, max((rad - 1.0) * s, 0.5), dist)
        core = np.exp(-(dist * dist) / max((2.4 * s) ** 2, 0.8))
        for c in range(3):
            out[c] = lerp(out[c], warm[c], disc * 0.42)
            out[c] = out[c] + core_c[c] * core * 0.28
    tint = lin_hex(flake) * lin_hex(0xAEBBD0) * 1.05
    for i in range(3):
        _snow_one(out, x, y, w, h, time, speed * 1.0, wind, density, tint, i, glass)
    glass_tint = lin_hex(0xD8E4EE)
    pane_w = ow / 2.0
    for c in range(3):
        out[c] = out[c] * glass_tint[c]
        out[c] = out[c] + lamp[c] * 0.10 * np.exp(
            -((x - (x0 + 0.28 * pane_w)) ** 2 + (y - (y0 + 0.78 * oh)) ** 2) / (2 * (0.28 * pane_w) ** 2)
        )
    return out


def _pane_local(x, y, x0, y0, pane_w, pane_h, mul):
    # inset by half a mullion so panes don't include the bar
    lx = (x - x0)
    ly = (y - y0)
    px = np.clip(np.floor(lx / pane_w), 0, 1)
    py = np.clip(np.floor(ly / pane_h), 0, 2)
    ox = lx - px * pane_w
    oy = ly - py * pane_h
    return ox, oy, px, py


def _frost(x, y, x0, y0, pane_w, pane_h, mul, glass, s):
    """Heavier in the lower corners, feathered, and uneven from pane to pane."""
    ox, oy, px, py = _pane_local(x, y, x0, y0, pane_w, pane_h, mul)
    pane = px + py * 2.0 + 1.0
    amt = hash12(pane, np.full_like(pane, 8.0))
    amt = smoothstep(0.12, 0.88, amt)
    pane_size = np.maximum(np.minimum(pane_w, pane_h), 1.0)
    lower = np.minimum(
        np.sqrt(ox ** 2 + (pane_h - oy) ** 2),
        np.sqrt((pane_w - ox) ** 2 + (pane_h - oy) ** 2),
    ) / pane_size
    upper = np.minimum(
        np.sqrt(ox ** 2 + oy ** 2),
        np.sqrt((pane_w - ox) ** 2 + oy ** 2),
    ) / pane_size
    corner_lo = smoothstep(0.62, 0.02, lower)
    corner_hi = smoothstep(0.26, 0.02, upper) * 0.30
    bottom = smoothstep(0.30, 0.0, (pane_h - oy) / np.maximum(pane_h, 1.0))
    frost = corner_lo * lerp(0.25, 1.0, amt) + corner_hi * amt + bottom * lerp(0.04, 0.42, amt)
    fern_u = (ox * 0.20 + oy) / max(18.0 * s, 1.0)
    fern_v = (np.minimum(ox, pane_w - ox) - np.minimum(oy, pane_h - oy)) / max(7.0 * s, 1.0)
    fern = vnoise(fern_u, fern_v + px)
    ridge = 1.0 - np.abs(2.0 * fern - 1.0)
    frost = frost * np.power(np.clip(ridge, 0.0, 1.0), 1.15) * lerp(0.20, 1.0, amt)
    return np.where(glass, np.clip(frost, 0.0, 1.0), 0.0)


def _sill_snow(x, y, x0, x1, y1, s):
    """Rounded pile on the sill, under the glass, with a soft uneven crest."""
    n = vnoise(x / max(80.0 * s, 1.0), np.full_like(x, 2.2))
    n2 = vnoise(x / max(34.0 * s, 1.0), np.full_like(x, 6.5))
    height = (20.0 + 12.0 * n + 5.0 * (n2 - 0.5)) * s
    t = (y - (y1 + 1.0 * s)) / np.maximum(height, 1.0)
    # Semicircular section: full at the sill, round at the top.
    prof = np.sqrt(np.clip(1.0 - np.clip(t, 0.0, 1.2) ** 2, 0.0, 1.0))
    prof = prof * smoothstep(1.05, 0.55, t)
    on = (x >= x0 + 8 * s) & (x <= x1 - 8 * s) & (y > y1) & (t >= 0.0) & (t <= 1.05)
    return np.where(on, prof.astype(np.float32), 0.0)


PRESETS = {
    "fireflies-summer-dusk": dict(kind="fireflies", style="summer", speed=0.32, density=1.0, mist=0.55,
                                  stars=True, glow=0xD2F26A, look=(1.0, 1.05, 0.28, False), time=4.2),
    "fireflies-deep-woods": dict(kind="fireflies", style="deep", speed=0.22, density=0.72, mist=0.82,
                                 stars=False, glow=0xB6E86A, look=(0.95, 0.95, 0.38, False), time=4.2),
    "fireflies-meadow": dict(kind="fireflies", style="meadow", speed=0.38, density=1.45, mist=0.32,
                             stars=True, glow=0xE8FF8A, look=(1.05, 1.15, 0.20, False), time=4.2),
    "snow-night-sky": dict(kind="snow", backdrop=0, speed=0.40, density=1.0, wind=0.22,
                           flake=0xF7F8FF, look=(1.0, 0.9, 0.22, False), time=3.1),
    "snow-pine-forest": dict(kind="snow", backdrop=1, speed=0.48, density=1.15, wind=0.38,
                             flake=0xF4F7FF, look=(1.0, 0.9, 0.30, False), time=3.1),
    "snow-window": dict(kind="snow", backdrop=2, speed=0.28, density=0.75, wind=0.10,
                        flake=0xFFF4E4, look=(1.0, 1.05, 0.30, False), time=3.1),
}


def render_named(name, w, h, time=None, mask=False, flies=True):
    p = PRESETS[name]
    t = p["time"] if time is None else time
    if p["kind"] == "fireflies":
        return render_fireflies(w, h, p["style"], t, p["density"], p["speed"], p["mist"], p["stars"] and flies,
                                p["glow"], p["look"], mask=mask, flies=flies)
    return render_snow(w, h, p["backdrop"], t, p["density"], p["speed"], p["wind"], p["flake"], p["look"], mask=mask)


def encode_named(name, w, h, time=None):
    p = PRESETS[name]
    col = render_named(name, w, h, time=time, mask=False)
    x, y = px_grid(w, h)
    return srgb_u8(col, uv_of(x, y, w, h), w, h, p["look"]), col
