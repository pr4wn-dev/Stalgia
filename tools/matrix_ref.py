"""Headless stand-in for MatrixRain trail brightness. No glyphs, no private effects."""

import math


def trail_alpha(k, trail):
    """k is cells behind the head. k<=1 is the eased head, not this curve."""
    if k <= 1:
        return 1.0
    tau = max(1.0, trail / 3.2)
    fade = math.exp(-(k - 2) / tau)
    t = k / max(trail, 1.0)
    if t > 0.80:
        fade *= max(0.0, (1.0 - t) / 0.20)
    return fade


def head_weights(frac):
    return frac, 1.0 - frac


def check():
    errors = []
    trail = 22.0
    prev = 1.0
    for k in range(2, 20):
        a = trail_alpha(k, trail)
        if a > prev + 1e-6:
            errors.append(f"trail rose at k={k}")
        if k > 1 and prev - a > 0.22 and k > 3:
            errors.append(f"trail step {prev - a:.3f} at k={k}")
        prev = a
    if trail_alpha(int(trail), trail) > 0.03:
        errors.append("trail end is not dark")
    enter, leave = head_weights(0.25)
    if abs(enter + leave - 1.0) > 1e-6:
        errors.append("head weights do not sum to 1")
    # Per-glyph ticks must not share one global 8 Hz bucket.
    changed = []
    for frame in range(30):
        t = frame / 30.0
        n = 0
        for g in range(200):
            rate = 0.45 + 1.1 * ((g * 17) % 255) / 255.0
            phase = ((g * 13) % 255) / 255.0
            tick = math.floor(t * rate + phase * 8.0)
            prev_tick = math.floor((t - 1.0 / 30.0) * rate + phase * 8.0)
            if tick != prev_tick:
                n += 1
        changed.append(n / 200.0)
    if max(changed) > 0.20:
        errors.append(f"mutation burst {max(changed):.2f}")
    return errors
