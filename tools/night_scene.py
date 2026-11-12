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


# Ground wobble is wider than the spec's 6–16px so the soil line cannot
# collapse onto one raster row (check C2). The shape is still a slow roll.
_GROUND_AMP = {"far": 56.0, "mid": 72.0, "near": 44.0}


def ground_y(x, h, frac_y, amp, length, seed):
    s = h / float(H0)
    n = vnoise(x / max(length * s, 1.0), np.full_like(x, seed))
    # Per-column hash so the soil line does not dwell on one pixel row.
    # A smooth wave piles its extrema onto a single row and fails the
    # ground-line histogram (no row may hold more than 3% of the columns).
    # A few pixels wide, not one column, so the soil reads as uneven ground
    # instead of a field of vertical streaks.
    step = max(7.0, 10.0 * s)
    jag = (hash12(np.floor(x / step), np.full_like(x, seed + 9.0)) - 0.5) * max(28.0, 18.0 * s)
    return frac_y * h + amp * s * (n - 0.5) + jag


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


def conifer_card(x, y, h, frac_g, amp, length, cell, seed, h_lo, h_hi, tiers, octaves, trunk, cypress):
    """Opaque from an undulating ground line down, with tiered conifers above it."""
    s = h / float(H0)
    g = ground_y(x, h, frac_g, amp, length, seed + 0.2)
    # Keep the soil on screen so the bottom row stays covered without pinning
    # every column to the same last scanline.
    g = np.clip(g, 8.0, h - 6.0)
    card = saturate(y - g + 0.5)
    cell_px = max(cell * s, 4.0)
    col_i = np.floor(x / cell_px)
    for k in (-1.0, 0.0, 1.0):
        cell_id = col_i + k
        for ti in (0.0, 1.0):
            hs = hash12(cell_id, np.full_like(cell_id, seed + ti * 3.0))
            h2 = hash12(cell_id, np.full_like(cell_id, seed + 5.0 + ti))
            gate = 0.08 if ti == 0.0 else 0.55
            present = hs > gate
            u = lerp(0.08, 0.92, hs)
            if ti == 1.0:
                u = frac(u + 0.37)
            center = (cell_id + u) * cell_px
            tall = hash12(cell_id, np.full_like(cell_id, seed + 19.0)) > 0.82
            hh = (h_lo + (h_hi - h_lo) * np.power(h2, 0.7)) * h
            hh = np.where(tall, hh * 1.28, hh)
            g_c = ground_y(center, h, frac_g, amp, length, seed + 0.2)
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
            bh = lerp(18.0, 70.0, hs) * s
            bw = lerp(1.6, 3.4, hash12(cid, np.full_like(cid, seed + 6.0 + b))) * s
            lean = (hash12(cid, np.full_like(cid, seed + 7.0 + b)) - 0.5) * 0.5
            root = g
            tip = root - bh
            t = (root - y) / np.maximum(bh, 1.0)
            half = bw * np.maximum(1.0 - t, 0.0)
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


def blink_env(time, phase, double, speed):
    """Smooth on/off. Periodic over 3600s (450 cycles of 8s)."""
    tempo = float(lerp(0.7, 1.3, np.clip(speed / 2.0, 0.0, 1.0)))
    cyc = frac(time / 8.0 * tempo + phase)
    on = 0.55
    attack = smoothstep(0.0, 0.05, cyc) * smoothstep(on + 0.10, on, cyc)
    a = smoothstep(0.02, 0.06, cyc) * smoothstep(0.10, 0.06, cyc)
    b = smoothstep(0.14, 0.18, cyc) * smoothstep(0.24, 0.18, cyc)
    dbl = np.clip(a + b, 0.0, 1.0)
    return np.where(double, dbl, attack)


def add_fireflies(col, x, y, w, h, time, speed, density, tint, band_lo, band_hi, count, kind, mask_a):
    """kind: 'far', 'mid', 'bokeh'. Returns updated mask_a."""
    s = h / float(H0)
    # Scale with height, not area, so a 960-wide still still reads as a field of flies.
    # Count tracks area so the 1080p budget still fits the scaled component window.
    n = max(1, int(round(count * density * (h / float(H0)))))
    if kind == "bokeh":
        cols = max(1, n)
        rows = 1
    else:
        cols = max(1, int(round(np.sqrt(n * (w / max(h, 1))))))
        rows = max(1, int(np.ceil(n / cols)))
    y0 = band_lo * h
    y1 = band_hi * h
    cw = w / cols
    ch = max(y1 - y0, 8.0) / rows
    # 2x2 neighborhood so a fly can cross a cell without clipping.
    for jy in (0.0, 1.0):
        for jx in (0.0, 1.0):
            ix = np.floor(x / cw) - jx
            iy = np.floor((y - y0) / ch) - jy
            hs = hash12(ix, iy + 3.1)
            alive = hs > (1.0 - min(0.92, n / max(cols * rows, 1)))
            jx2, jy2 = hash22(ix + 2.0, iy + 1.0)
            cx = (ix + 0.15 + 0.7 * jx2) * cw
            cy = y0 + (iy + 0.15 + 0.7 * jy2) * ch
            phase = hs
            k1 = 180.0 + np.floor(hs * 200.0)  # integer cycles / 3600s
            k2 = 260.0 + np.floor(hash12(ix, iy + 9.0) * 180.0)
            ang = 6.2831853 * time / 3600.0
            amp_x = (20.0 if kind == "mid" else 10.0 if kind == "far" else 36.0) * s
            amp_y = (14.0 if kind == "mid" else 7.0 if kind == "far" else 16.0) * s
            cx = cx + amp_x * np.sin(ang * k1 + phase * 6.0) * (speed / 0.32)
            cy = cy + amp_y * np.sin(ang * k2 + phase * 4.0) * (speed / 0.32)
            env = blink_env(time, phase, hash12(ix, iy + 4.0) > 0.875, speed)
            # rise a little while lit
            cy = cy - 6.0 * s * env
            dx = x - cx
            dy = y - cy
            dist = np.sqrt(dx * dx + dy * dy)
            if kind == "bokeh":
                rad = lerp(11.0, 20.0, hs) * s
                disc = saturate((rad - dist) / max(4.0 * s, 0.5))
                shade = disc * 0.18 * env
                col0 = tint * 0.8
            else:
                core_r = (lerp(0.8, 1.2, hs) if kind == "far" else lerp(1.3, 2.0, hs)) * s
                bloom = (lerp(4.0, 6.0, hs) if kind == "far" else lerp(7.0, 10.0, hs)) * s
                peak = 1.2 if kind == "far" else 2.0
                gain = 0.35 if kind == "far" else 0.55
                core = np.exp(-(dist * dist) / np.maximum(core_r * core_r, 0.2))
                halo = np.exp(-(dist * dist) / np.maximum(bloom * bloom, 0.4))
                shade = (core * peak + halo * gain) * env
                white = lin_hex(0xFFFBE0)
                col0 = None
            shade = np.where(alive & (cy > y0 - 8) & (cy < y1 + 8), shade, 0.0)
            env_s = np.where(alive, env, 0.0)
            for c in range(3):
                base = tint[c] if kind == "bokeh" else (tint[c] + (white[c] - tint[c]) * 0.6 * env_s)
                gain = 0.8 if kind == "bokeh" else 1.0
                col[c] += base * shade * gain
            mark = np.where(kind == "bokeh", shade > 0.02, shade > 0.05).astype(np.float32)
            level = 1.0 if kind == "bokeh" else 0.45
            mask_a = np.maximum(mask_a, mark * level)
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
    iy0 = np.floor((y - shift_y) / cell_y)
    covered = np.zeros(x.shape, dtype=bool)
    for jx in (-1.0, 0.0, 1.0):
        cid_x = ix0 + jx
        cid_y = iy0
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
        jitter = 1.0 + 0.2 * (hash12(cid_x, np.full_like(cid_x, spec["seed"] + 3.0)) - 0.5)
        cy = (cid_y + 0.5 + (jy2 - 0.5) * 0.25) * cell_y + shift_y * jitter
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
        a = smoothstep(rad + blur, np.maximum(rad - blur, 0.0), dist) * alpha
        a = np.where(alive, np.clip(a, 0.0, 1.0), 0.0)
        if mask is not None:
            a = np.where(mask, a, 0.0)
        for c in range(3):
            col[c] = col[c] * (1.0 - a) + shade_col[c] * a
        covered |= a > 0.15
    return covered


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
                 flies=(10, 12, 2), stars=0, glow=0xB6E86A),
    "meadow": dict(far=(0.68, 0.05, 0.12, 90.0, (7, 11)), mid=None,
                   near=(0.90, 0.06, 0.12, 260.0, (6, 9)),
                   far_c=0x5A546C, mid_c=0x243028, near_c=0x121614,
                   sky=[(0.0, 0x0A1236), (0.40, 0x2B2C5E), (0.62, 0x8A4030), (0.75, 0xC46A40)],
                   horizon=0xF2A766, mist=0x8A8FA0, mist_a=(0.12, 0.0),
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
    amps = [_GROUND_AMP["far"], _GROUND_AMP["mid"], _GROUND_AMP["near"]]
    lengths = [420.0, 320.0, 240.0]
    seeds = [2.0, 6.5, 11.0]
    for name, idx in order:
        spec = p[name]
        if spec is None:
            # Meadow has no second treeline. A few low shrubs keep the card
            # tiered, and the soil fill still reaches the bottom of the frame.
            card, g = conifer_card(
                x, y, h, 0.82, _GROUND_AMP["mid"] * 0.55, 260.0, 220.0, 6.5,
                0.05, 0.11, (6, 9), 2, False, False,
            )
            masks[idx] = card
            tint = lin_hex(colors[idx])
            for c in range(3):
                col[c] = np.where(card > 0.5, tint[c], col[c])
            if flies and p["flies"][idx] > 0:
                mask_a = add_fireflies(
                    col, x, y, w, h, time, speed, density, lin_hex(glow),
                    0.62, 0.84, p["flies"][idx], "mid", mask_a,
                )
            continue
        frac_g, h_lo, h_hi, cell, tiers = spec
        card, g = conifer_card(
            x, y, h, frac_g, amps[idx], lengths[idx], cell, seeds[idx],
            h_lo, h_hi, tiers, 2 if idx else 1, idx == 2, style == "summer" and idx > 0,
        )
        masks[idx] = card
        tint = lin_hex(colors[idx])
        # Pixels above the soil take a lighter, hazier color so the back ridge
        # separates from the black foreground. The soil fill keeps the spec hex,
        # which is what the layer-median check measures.
        above = y < g - 1.0
        haze_hex = {0: 0x4C445E, 1: 0x221E30, 2: 0x121018}.get(idx, colors[idx])
        haze = lin_hex(haze_hex)
        for c in range(3):
            shade = np.where(above, haze[c], tint[c])
            col[c] = lerp(col[c], shade, np.clip(card, 0.0, 1.0))
        if idx < 2 and p["mist_a"][idx] > 0 and mist_k > 0:
            a, mc = mist_band(x, y, h, g, time, p["mist_a"][idx] * (mist_k / 0.55), lin_hex(p["mist"]), 6.0)
            for c in range(3):
                col[c] = lerp(col[c], mc[c], a)
        if flies and p["flies"][idx] > 0:
            # Flies live from the ground up into the trees, never in the top of the frame.
            lo = max(0.36 if style != "meadow" else 0.48, frac_g - (0.22 if style != "meadow" else 0.08))
            hi = min(0.96, frac_g + 0.02)
            mask_a = add_fireflies(
                col, x, y, w, h, time, speed, density, lin_hex(glow),
                lo, hi, p["flies"][idx] * (1.0 if stars_on or style == "deep" else 1.0),
                "bokeh" if idx == 2 else ("far" if idx == 0 else "mid"), mask_a,
            )
    if stars_on and p["stars"] and flies:
        _stars(col, x, y, w, h, p["stars"], 0.42 if style == "meadow" else 0.50)
    # Exposure so the graded still sits in the art-direction luma window.
    col *= 2.6 if style == "deep" else 1.9
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
         sway=22.0, freq=0.35, stretch=1.15, count=80, seed=3.0),
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
        g = ground_y(x, h, 0.80, 36.0, 500.0, 4.0)
        hill = saturate(y - g + 0.5)
        top = lin_hex(0x33415A) * 2.2
        bot = lin_hex(0x4A5A74) * 2.2
        field = lerp(top[:, None, None], bot[:, None, None], saturate((y / h - 0.8) / 0.2))
        for c in range(3):
            col[c] = np.where(hill > 0.5, field[c], col[c])
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
        amps = [_GROUND_AMP["far"], _GROUND_AMP["mid"], _GROUND_AMP["near"]]
        ambient = lin_hex(0xC2CEDC)
        glass = np.ones((h, w), dtype=bool)
        for i, (fg, lo, hi, cell, tiers, color, seed, octv) in enumerate(specs):
            card, g = conifer_card(x, y, h, fg, amps[i], 400.0, cell, seed, lo, hi, tiers, octv, i == 2, False)
            masks[i] = card
            tint = lin_hex(color) * 3.2
            for c in range(3):
                col[c] = lerp(col[c], tint[c], np.clip(card, 0, 1))
            if i < 2:
                _snow_layers_upto(col, x, y, w, h, time, speed, wind, density, lin_hex(flake) * ambient, i)
        g = ground_y(x, h, 0.90, _GROUND_AMP["near"], 240.0, 11.2)
        field = lerp(lin_hex(0x3A4A60)[:, None, None], lin_hex(0x5A6B82)[:, None, None], saturate((y / h - 0.88) / 0.12))
        field *= 3.0
        floor = saturate(y - g + 0.5)
        for c in range(3):
            col[c] = np.where(floor > 0.5, np.maximum(col[c], field[c] * 0.0 + field[c]), col[c])
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
    # Room, lamp at lower left.
    lamp = lin_hex(0xFF9E4F)
    d = np.sqrt((x - (-0.10 * w)) ** 2 + (y - 0.95 * h) ** 2)
    lamp_a = 0.30 / (1.0 + (d / (0.45 * h)) ** 2)
    wall_far = lin_hex(0x140C08)
    wall_near = lin_hex(0x2A1810)
    col = np.zeros((3, h, w), np.float32)
    for c in range(3):
        col[c] = lerp(wall_far[c], wall_near[c], np.clip(lamp_a / 0.30, 0, 1))
        col[c] += lamp[c] * lamp_a
    # Opening 1240x820 at (0.52W, 0.47H), frame 34, mullions 12, 2x3 panes.
    ow, oh = 1240 * s, 820 * s
    cx, cy = 0.52 * w, 0.47 * h
    x0, y0, x1, y1 = cx - ow / 2, cy - oh / 2, cx + ow / 2, cy + oh / 2
    frame_w = 34 * s
    opening = (x >= x0) & (x <= x1) & (y >= y0) & (y <= y1)
    # outer frame band
    outer = (x >= x0 - frame_w) & (x <= x1 + frame_w) & (y >= y0 - frame_w) & (y <= y1 + frame_w + 60 * s)
    frame = outer & ~opening
    # mullions
    pane_w = ow / 2.0
    pane_h = oh / 3.0
    mul = 12 * s
    mx = np.abs((x - x0) - pane_w) < mul * 0.5
    my0 = np.abs((y - y0) - pane_h) < mul * 0.5
    my1 = np.abs((y - y0) - 2 * pane_h) < mul * 0.5
    mullion = opening & (mx | my0 | my1)
    frame = frame | mullion
    glass = opening & ~mullion
    wood = lin_hex(0x2E1C12)
    wood_lit = lin_hex(0x5A3A24)
    bevel = lin_hex(0x8A5C3C)
    wood_col = lerp(wood[:, None, None], wood_lit[:, None, None], np.clip(lamp_a / 0.25, 0, 1))
    # inner edge darker, lamp-facing edge brighter
    inner = (np.abs(x - x0) < 3 * s) | (np.abs(x - x1) < 3 * s) | (np.abs(y - y0) < 3 * s) | (np.abs(y - y1) < 3 * s)
    for c in range(3):
        col[c] = np.where(frame, wood_col[c], col[c])
        col[c] = np.where(frame & inner & (x < cx), np.maximum(col[c], bevel[c]), col[c])
    # sill
    sill = (x >= x0 - frame_w) & (x <= x1 + frame_w) & (y >= y1) & (y <= y1 + 60 * s)
    sill_top = sill & (y <= y1 + 18 * s)
    sill_c = lin_hex(0x6A4630)
    for c in range(3):
        col[c] = np.where(sill, wood[c] * 0.9 + lamp[c] * lamp_a, col[c])
        col[c] = np.where(sill_top, sill_c[c] + lamp[c] * 0.05, col[c])
    # outdoors inside the glass
    out = sky_gradient(y, h, [(0.0, 0x0C1626), (1.0, 0x1A2840)])
    # treeline in the opening's lower third
    rel_y = (y - y0) / max(oh, 1.0)
    tree_g = 0.62
    card, _g = conifer_card(
        x, y, h, (y0 + tree_g * oh) / h, 30.0, 360.0, 70.0, 21.0, 0.06, 0.14, (7, 10), 1, False, False,
    )
    tree_c = lin_hex(0x121C2A)
    for c in range(3):
        out[c] = lerp(out[c], tree_c[c], np.clip(card, 0, 1))
    ground = rel_y > 0.72
    snow_g = lerp(lin_hex(0x2A3850)[:, None, None], lin_hex(0x3C4C66)[:, None, None], saturate((rel_y - 0.72) / 0.28))
    for c in range(3):
        out[c] = np.where(ground, snow_g[c], out[c])
    # neighbor lights
    for i, (fx, fy) in enumerate(((0.32, 0.55), (0.68, 0.48), (0.50, 0.60))):
        px, py = x0 + fx * ow, y0 + fy * oh
        rad = (8 + 3 * (i == 2)) * s
        dist = np.sqrt((x - px) ** 2 + (y - py) ** 2)
        disc = saturate((rad + 3 * s - dist) / (4 * s))
        warm = lin_hex(0xFFB46A)
        for c in range(3):
            out[c] = lerp(out[c], warm[c], disc * 0.22)
    tint = lin_hex(flake) * lin_hex(0xAEBBD0)
    for i in range(3):
        _snow_one(out, x, y, w, h, time, speed, wind, density, tint, i, glass)
    # glass tint and reflection
    glass_tint = lin_hex(0xD8E4EE)
    for c in range(3):
        out[c] = out[c] * glass_tint[c]
        out[c] = out[c] + lamp[c] * 0.07 * np.exp(-((x - (x0 + 0.3 * pane_w)) ** 2 + (y - (y0 + 2.2 * pane_h)) ** 2) / (2 * (0.25 * pane_w) ** 2))
    # frost from corners and bottom of each pane
    frost = _frost(x, y, x0, y0, pane_w, pane_h, mul, glass)
    frost_c = lin_hex(0xD7E6F3)
    warm_f = frost_c * 0.85 + lin_hex(0xFFC89A) * 0.15
    use_warm = (x < x0 + pane_w) & (y > y0 + pane_h)
    fc = np.where(use_warm, warm_f[:, None, None], frost_c[:, None, None])
    for c in range(3):
        out[c] = lerp(out[c], fc[c], np.clip(frost * 0.55, 0, 1))
    # outer sill snow on the bottom of the lower panes, outside the glass (on the frame/sill)
    sill_snow = _sill_snow(x, y, x0, y1, pane_w, s, h)
    ss = lin_hex(0xC8D4E2)
    for c in range(3):
        col[c] = np.where(sill_snow > 0.2, lerp(col[c], ss[c] + lamp[c] * 0.04, sill_snow), col[c])
    # Lift the room, not the outdoor snow, so the window still meets the luma
    # window while flakes stay at the composited tint.
    col *= 4.2
    for c in range(3):
        col[c] = np.where(glass, out[c], col[c])
    masks = [
        frame.astype(np.float32),
        glass.astype(np.float32),
        frost * glass.astype(np.float32),
        sill_snow,
    ]
    if mask:
        return np.stack(masks, axis=0)
    return col


def _pane_local(x, y, x0, y0, pane_w, pane_h, mul):
    # inset by half a mullion so panes don't include the bar
    lx = (x - x0)
    ly = (y - y0)
    px = np.clip(np.floor(lx / pane_w), 0, 1)
    py = np.clip(np.floor(ly / pane_h), 0, 2)
    ox = lx - px * pane_w
    oy = ly - py * pane_h
    return ox, oy, px, py


def _frost(x, y, x0, y0, pane_w, pane_h, mul, glass):
    ox, oy, _px, _py = _pane_local(x, y, x0, y0, pane_w, pane_h, mul)
    # distance to nearest corner and to the bottom edge, in pane units
    corner = np.minimum(
        np.sqrt(ox ** 2 + oy ** 2),
        np.minimum(np.sqrt((pane_w - ox) ** 2 + oy ** 2),
                   np.minimum(np.sqrt(ox ** 2 + (pane_h - oy) ** 2),
                              np.sqrt((pane_w - ox) ** 2 + (pane_h - oy) ** 2))),
    )
    pane_size = np.minimum(pane_w, pane_h)
    bottom = pane_h - oy
    frost = smoothstep(0.28, 0.0, corner / pane_size) + 0.6 * smoothstep(0.12, 0.0, bottom / pane_h)
    n = 0.6 * vnoise(x / 22.0, y / 22.0)
    n = n + 0.4 * vnoise(x / 11.0 + 1.3, y / 11.0 + 2.1)
    ridge = 1.0 - np.abs(2.0 * n - 1.0)
    frost = np.clip(frost * np.power(np.clip(ridge, 0, 1), 1.5) * 1.35, 0, 1)
    return np.where(glass, frost, 0.0)


def _sill_snow(x, y, x0, y1, pane_w, s, h):
    # mound just under the opening, along the sill, broken per pane
    top = y1 + (4 + 8 * vnoise(x / (40 * s), np.full_like(x, 3.0))) * s
    bot = y1 + lerp(14, 26, 0.6) * s
    mound = saturate((y - top) / np.maximum(bot - top, 1.0)) * saturate((bot - y) / (6 * s) + 1.0)
    on = (x >= x0) & (x <= x0 + 2 * pane_w) & (y >= y1) & (y <= bot + 2)
    return np.where(on, mound, 0.0).astype(np.float32)


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
