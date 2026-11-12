# Stalgia 1.12 art direction

Bobby's correction, which overrides the "painterly" wording below: "Bob Ross" meant quality only. He does not want a painted look. The effects are photoreal, cinematic, and high-detail. Fireflies, snow, stars, and aurora are overlays on real scenery.

The shipped plates are WebP resources in the exe, decoded with WIC on a worker thread at startup and kept on the GPU, so a preset change does not decode them again. Still night and Milky Way share one set. Four layers, back to front: `layer-a-sky` (opaque, empty sky), `layer-b-mountains`, `layer-c-mid`, and `layer-d-foreground`. They are used for Summer dusk, Pine forest, Still night, Milky Way, and the default aurora. The other presets stay procedural. A folder under `STALGIA_PLATES_DIR`, `<exe>\plates`, or the app-data `plates` folder overrides a preset when `layer-a-sky` is present. Each layer drifts slowly, and the farthest moves least. The sample is zoomed in by 6% so that drift, including cursor parallax, never shows the plate edge. Fireflies sit between mid and foreground. Stars, the Milky Way, and the aurora draw in the sky, behind the mountains. Most snow falls in front, with the small flakes behind the mid layer. Alpha at or above 250 snaps to 255. Aurora peaks and ice move toward an equal-channel gray, more on the bright ice than the dark water. The curtain stays in the sky: saturated green, with soft magenta at the top.

The rest of this file is the review of 1.11.0. It covers Bobby's feedback on the released build, which he runs on his own PC:

1. Some fireflies are cut off: on some only the lower half of the glow shows, on others only half of the bug.
2. The art could be better on Stars, Aurora, Snowfall, Fireflies, and Matrix.

Section 1 gives the firefly root cause, the exact fix, and its regression checks. Section 2 covers a second bug found during the review: falling snow disappears within a minute. That bug is worse than the firefly one, so ship both fixes first. Section 3 sums up what the current stills show. Section 4 covers upgrades shared by every effect. Sections 5 to 9 give a prioritized upgrade for Stars, Aurora, Fireflies, Snowfall, and Matrix, with targets that can be checked without a GPU. Section 10 sets the GPU budget, section 11 lists where the checks run, and section 12 gives the order of work.

All measurements below are from `tools/render_sky.py` and `tools/night_scene.py` at `3603250` (the v1.11.0 tag). Those CPU references mirror `src/gfx/Shaders.h` line for line. Matrix has no CPU reference, so its review comes from `src/effects/MatrixRain.cpp` and `src/gfx/GlyphAtlas.cpp`.

---

## 1. Bug: fireflies are cut off

### 1.1 Root cause

`addFlyLayer` in `src/gfx/Shaders.h` (and `add_fireflies` in `tools/night_scene.py`) puts each fly in a grid cell. Each pixel then looks for nearby flies in a 2×2 block of cells:

```hlsl
[unroll] for (int jy = 0; jy < 2; jy++) {
    [unroll] for (int jx = 0; jx < 2; jx++) {
        float2 ix = float2(floor(px.x / cw) - float(jx), floor((px.y - y0) / ch) - float(jy));
```

The offsets are `0` and `-1` only, so a pixel sees the fly in its own cell and in the cells to the **left** and **above**. A fly in cell `k` is therefore drawn only by pixels in cells `k` and `k + 1`. Pixels in cell `k - 1`, above or to the left of the fly's cell, never evaluate it. Any part of a glow that crosses the top or left edge of its cell is cut along a straight line.

The 1.11 doc (section 2.5) asked for a 2×2 block of the **nearest** neighbors, picked by `frac < 0.5`, and for cells at least `2 × (wander + 3σ bloom)`. What shipped always picks left and up, and the cells are much smaller than that:

- Each fly center is placed at 0.15–0.85 of its cell. It then wanders ±14 px vertically (mid layer, at speed 0.32) and rises 6 px while lit.
- The mid layer's halo is `exp(-d²/b²)` with `b` up to 10 px, so it stays visible to about `3b = 30 px`.
- Mid cells are only **49 px tall** at 1920×1080 with default settings, because 18 flies are spread over a band 0.18 of the frame tall in 4 rows.
- The wander amplitude scales linearly with the Speed knob (`* (speed / 0.32)`). At speed 2.0 the vertical wander is 87 px, which is larger than the cell.

Cells are wide (339 px) but short, so cuts are almost always on the **top** edge. That matches "only the lower half of the glow". When a fly wanders more than its core radius past the top of its cell, the cut runs through the core itself, which is "only one half of the bug".

**Simulation.** Each fly's glow was checked against the area that can draw it, over a full motion cycle:

| Resolution, preset, speed, density | Mid flies with halo cut | Mid flies with core cut | Far flies with halo cut |
|---|---|---|---|
| 1920×1080, Summer dusk, 0.32, 1.0 (defaults) | 31 of 39 | 15 | 15 of 39 |
| 1920×1080, Deep woods, 0.22, 0.72 | 12 of 25 | 7 | 7 of 24 |
| 2560×1440, Summer dusk, defaults | 40 of 51 | 21 | 20 of 51 |
| 3840×2160, Summer dusk, defaults | 54 of 65 | 37 | 36 of 65 |
| 1920×1080, speed 2.0 | 33 of 39 | 33 | 32 of 39 |
| 1920×1080, speed 1.0, density 2.2 | 65 of 73 | 65 | 63 of 73 |

The near layer (one row, 400 px tall cells) is fine. The bug also shows in the shipped stills. In `fireflies-meadow` at t = 4.2 s, several horizon glows have a hard flat top, and one core is sliced through its upper half. The CPU reference renders the same cut.

Other candidates, ruled out:

- **Sprite quad, scissor, or viewport:** fireflies are drawn by a fullscreen pixel shader, with no quads and no scissor.
- **Atlas bleed or UV offset:** no texture is involved.
- **Horizon or ground mask:** the cuts sit at cell edges, not at the ground line.
- **Band culling:** `c.y > y0 - 8 && c.y < y1 + 8` removes a whole fly, not half of one. It does cause pops (fix in 1.2, step 5).

### 1.2 Exact fix

Make the same change in `addFlyLayer` (HLSL) and `add_fireflies` (Python) so the CPU reference stays in step.

1. **Bound the wander.** The Speed knob should speed up motion, not stretch it across the screen. The `k` values must stay integers so motion still loops every 3600 s.

   ```hlsl
   float spK  = speed / 0.32;
   float ampK = clamp(spK, 0.25, 1.5);
   float kMul = max(1.0, round(spK / 1.5));        // integer, keeps the 3600 s loop
   float ax = baseAx * s * ampK;                   // baseAx: far 10, mid 20, near 36
   float ay = baseAy * s * ampK;                   // baseAy: far 7,  mid 14, near 16
   // k1 = (180 + floor(hs * 200)) * kMul;  k2 = (260 + floor(h9 * 180)) * kMul;
   ```

2. **Size the cells from the glow reach.** In the code, `b` is the halo radius in `exp(-d²/b²)`, not σ, so the visible halo ends at `3b` (e⁻⁹).

   ```hlsl
   float bMax   = (kind == 0) ? 6.0 * s : ((kind == 1) ? 10.0 * s : 8.0 * s);
   float reachY = 3.0 * bMax + ay + 6.0 * s + 1.0;   // halo + wander + rise-while-lit + 1 px
   float reachX = 3.0 * bMax + ax + 1.0;
   float bandH  = max(y1 - y0, 8.0);
   rows = min(rows, max(1.0, floor(bandH * 1.15 / reachY)));
   cols = max(cols, ceil(n / (rows * 0.85)));       // keep the fly count when rows shrink
   cols = min(cols, max(1.0, floor(width * 1.15 / reachX)));
   float occ = min(0.92, n / (cols * rows));
   ```

3. **Search a 3×3 block.**

   ```hlsl
   [unroll] for (int jy = -1; jy <= 1; jy++) {
       [unroll] for (int jx = -1; jx <= 1; jx++) {
           float2 ix = float2(floor(px.x / cw) + float(jx), floor((px.y - y0) / ch) + float(jy));
   ```

   Why this is enough: a fly in cell `k` has its center in `[(k + 0.15)·c − a − rise, (k + 0.85)·c + a]`. A 3×3 search draws it on every pixel in `[(k − 1)·c, (k + 2)·c)`. The glow therefore fits when `c ≥ (R + a + rise) / 1.15`, and step 2 enforces exactly that. At defaults this changes nothing about the layout: mid rows stay at 4 at 1080p. At 4K, and at high density or speed, it trades a row for a column.

4. **Skip pixels that can't reach the band.** If `px.y` is outside `[y0 − reachY, y1 + reachY]`, skip the layer. With the 3×3 block, the shader evaluates 27 flies per pixel instead of 12. The skip limits that to rows that can see a fly, which is about 35–45% of the frame. Net cost stays at or below 1.11 (see 10).

5. **Fade at the band edge instead of popping.** Replace the hard `c.y` gate with `smoothstep(y0 − 8 − 12s, y0 − 8, c.y) * smoothstep(y1 + 8 + 12s, y1 + 8, c.y)`, multiplied into `shade`.

6. **Match the blink between CPU and GPU.** HLSL `blinkEnv` has a fixed 8 s period with no tempo and no double flash. Python `blink_env` has the `lerp(0.7, 1.3, speed / 2)` tempo and a one-in-eight double flash. The stills show double flashes the app never draws. Port the Python envelope to HLSL, or better, the new patterns in F1 (section 7).

Apply the same changes to the fly-coverage alpha (`maskA`) so the debug mask matches.

### 1.3 Regression checks

These are C19–C22 in section 11.

- **C19, reach proof (Python, fast).** For every layer, preset, resolution {960×540, 1920×1080, 1920×1200, 2560×1080, 2560×1440, 3840×2160}, speed {0.05, 0.32, 1.0, 2.0}, and density {0.15, 1.0, 2.2}, compute the cell size, `reachX`, and `reachY` the same way the shader does. Assert `ch·1.15 ≥ reachY` and `cw·1.15 ≥ reachX`. Today this fails at defaults (mid: 49 px × 1.15 < 50 px reach).
- **C20, rendered symmetry (Python).** Render only the flies, on black with mist off, using `add_fireflies` itself. Enumerate the flies analytically (center, `coreR`, `b`, envelope). For each fly that has envelope ≥ 0.3, has no other fly within `2R`, and is fully on screen, compare against the ideal glow over a disk of radius `R = 3b`:
  - The rendered-to-ideal energy ratio is ≥ 0.99.
  - The maximum pixel error is ≤ 2% of the fly's peak.
  - Top/bottom and left/right half-disk energy agree within 3%: `|E_top − E_bot| / (E_top + E_bot) ≤ 0.03`.

  Sweep 16 times evenly over 0–3600 s, all three presets, speed {0.32, 2.0}, density {1.0, 2.2}, at 960×540 and 1920×1080. Today Meadow at t = 4.2 fails on its horizon flies.
- **C21, GPU symmetry (`--selftest`).** Add a debug value `p3 = −2` that returns fly light only, on black. For each Fireflies preset, render 3 frames with app time forced to 4.2, 600, and 1800 s, then read back the scene target. Find local maxima above 0.05. For every isolated peak, compare luma at ±3, ±6, and ±9 px up/down and left/right: each pair's ratio must be within 0.8–1.25. Print `FlyGlow: PASS peaks=N worst=0.97`.
- **C22, neighbor-loop guard (Python, static).** `scene_check.py` greps `Shaders.h` `addFlyLayer` for `jy = -1; jy <= 1` and `jx = -1; jx <= 1`, and `night_scene.py` for `(-1.0, 0.0, 1.0)` in `add_fireflies`. That fails if either side drops back to a 2×2 block.

---

## 2. Bug found in review: falling snow disappears within a minute

### 2.1 Root cause

`snowDisc` (HLSL) and `snow_layer` (Python) give each column a fall-speed variation:

```hlsl
float jitter = 1.0 + 0.2 * (hash12(float2(cid.x, seed + 3.0)) - 0.5);   // 0.9 .. 1.1
...
float2 c = float2(..., (cid.y + 0.5 + (j.y - 0.5) * 0.25) * cellY + shiftY * jitter);
```

The pixel's cell row comes from `origin = floor((px - float2(shiftX, shiftY)) / cell)`, which uses `shiftY` **without** the jitter. Each flake therefore drifts away from the cell row that looks it up, by `shiftY · (jitter − 1)`, which is up to 10% of the total fall distance and grows with time. The shader only checks one row (`oy` is fixed at 0). Once the drift passes about half a cell, the flake is only partly drawn: clipped flat on top or bottom. After that it is not drawn at all. Shader time runs from 0 to 3600 s, so the sky fills again every hour and then empties.

Measured on `snow-pine-forest`, 960×540, counting flake blobs in the top 200 rows:

| Time | 3.1 s | 30 s | 120 s | 900 s |
|---|---|---|---|---|
| Flakes | 289 | 70 | 22 | 10 |

The stills, the scene checks, and the art review for 1.11 all sampled t = 3.1 s, so none of them could see it. On a real desktop the snowfall is nearly empty one minute after start. All three Snowfall presets are affected, including the snow behind the Window glass.

The same term also breaks the hourly loop. `fallQ · 3600` is a whole number of cells, but `fallQ · jitter · 3600` is not, so every flake jumps when time wraps.

### 2.2 Exact fix

Move the jitter into the column's own fall rate, quantize it, and compute the row lookup per column. Make the same change in HLSL and Python.

```hlsl
float originX = floor((px.x - shiftX) / cellX);
[unroll] for (int ox = -1; ox <= 1; ox++) {
    float colX   = originX + float(ox);
    float jit    = 1.0 + 0.2 * (hash12(float2(colX, seed + 3.0)) - 0.5);
    float fallC  = round(rate * jit / max(qy, 1e-5)) * qy;    // whole cells per 3600 s
    float shiftC = fallC * time;
    float rowY   = floor((px.y - shiftC) / cellY);
    float2 cid   = float2(colX, rowY);
    ...
    float2 c = float2(..., (cid.y + 0.5 + (j.y - 0.5) * 0.25) * cellY + shiftC);
```

This adds one hash, one `round`, and one `floor` per column iteration. One row is still enough: the flake center stays within ±0.125 of a cell vertically, and the biggest flake reach (bokeh layer: 15 px radius + 4 px blur at 1080p) is far below `0.375 × 480 px`. If the motion blur in W2 (section 8) makes near flakes longer than `0.3 × cellY`, add a second row on the side the streak trails.

### 2.3 Regression checks

These are C23–C25 in section 11.

- **C23, flake persistence (Python).** Render the flake layers only, on black, at t ∈ {3.1, 60, 600, 1800, 3599.9} for all three presets. The flake count at each time is within ±15% of t = 3.1, and within ±25% of `count × density` summed over the layers that are on screen. Today: −97% at 900 s.
- **C24, no clipped flakes (Python).** For isolated flake components at least 3 px across, the height/width ratio is within ±15% of the layer's `stretch`. Top-half and bottom-half energy agree within 5%, the same test as C20.
- **C25, GPU persistence (`--selftest`).** For each Snow preset, force app time to 3 s and then 600 s and read back `lit` with the debug value `p3 = −3` (flakes only). The ratio `lit(600) / lit(3)` must be ≥ 0.85. The current `cover` check passes because the backdrop alone lights enough pixels.

The 1.11 time-wrap check (C15 in the 1.11 doc) should then pass for Snow, because every column's offset is a whole number of cells over 3600 s.

---

## 3. What the stills show today, in one paragraph each

- **Stars.** A uniform salt of white points with soft halos. The three brightest brightness buckets hold about the same number of stars (about 1,000 each per quarter-decade at 1080p), so bright stars are as common as mid ones and nothing reads as a hero star. The Milky Way is one soft diagonal blob with a single straight dark smear for a dust lane, and the core is olive-brown. All four presets share this composition. A meteor crosses every 9 s exactly. Twinkle is a pure sine at ±10%. Nebula is two Gaussian blobs, the right one flat lilac. Warp streaks all have the same length.
- **Aurora.** The curtain's bright lower edge sits at 0.62–0.72 of the frame height (median 0.674 for Classic). That is where a horizon would be, so the two ribbon layers read as glowing green hills, not sky. There is no ground in Classic, Magenta, or Polar. Stars are effectively off: 0 above 0.05 linear in the top 30% of three presets. A horizontal crease runs across the sky at about 0.545 H: the third diffuse layer in `auroraRibbons` starts with a 1.4%-tall `smoothstep`, and the row-mean second derivative there is about 0.85 (8-bit) against under 0.3 elsewhere. Rays are evenly spaced and the same length.
- **Fireflies.** The tree layering from 1.11 works. The lights are the weak part: glows are cut (bug 1), every fly is the same small Gaussian, near "bokeh" flies are only slightly larger, the blink is a single fixed 8 s envelope on the GPU, and the light never touches anything. The Meadow horizon band is fully saturated orange, and with clipped orange glows on it, those glows read as dashes.
- **Snowfall.** Besides bug 2: flakes are flat-alpha discs ("coins") with no bright core, no motion blur, and no tumble. In Night sky, flakes and stars look the same in a still. The ground is a flat blue slab with a lumpy dark-rimmed edge, there is a moon glow with no moon, and the treeline is barely visible. Pine forest trees carry no snow. The Window frost is a bubbly cellular noise that looks like coral, not ice ferns. The room is a flat orange gradient.
- **Matrix** (from source). The atlas is drawn once with GDI at the cell size, with no mipmaps. Far columns are drawn at 0.62× scale with bilinear filtering, so they shimmer. The first font with katakana is MS Gothic, which uses embedded bitmaps at small sizes, so glyphs are jagged despite `ANTIALIASED_QUALITY`. The head "bloom" is four copies of the head glyph offset left, right, up, and down, which makes a plus-shaped ghost, not a glow. All glyph mutations happen on one global 8 Hz tick (`timeSalt = llround(time * 8)`), so the whole screen flickers in lockstep. The head jumps a whole cell at a time. Depth only scales size and brightness, and all columns stay on the same x grid.

---

## 4. Shared upgrades (do these first, they feed every effect)

Priority order: **G1 > G2 > G3**.

### G1. One shared bloom pass

There is no bloom in the renderer today. Every glow is an analytic `exp()` inside each effect, and Matrix fakes one with offset copies.

- Add a dual-filter (Kawase) bloom between the scene target and `PSGrade`. Downsample 1/2 → 1/4 → 1/8 → 1/16, then upsample and add back.
- Use a soft-knee threshold: knee 0.6, threshold 0.8 linear, on pre-grade scene color.
- Make it energy-conserving and per-effect: add `bloomStrength` and `bloomRadius` to the constants. Defaults: Stars 0.35, Aurora 0.25, Fireflies 0.55, Snow 0.15, Matrix 0.6, Lava 0.
- Use it for falloff and stop adding analytic halos where it covers the same job. Drop the star `halo` term for sky stars and Matrix's offset copies. The firefly halo stays analytic, because it must be clipped by the near trees.
- **Target.** An impulse response that falls off softly: about three Gaussians, σ ≈ 2, 8, and 24 px at 1080p, weighted 0.6 / 0.3 / 0.1.
- **Check C26 (Python).** Mirror the bloom in `tools/bloom_ref.py`. Pass a single-pixel impulse through it, then check:
  - The radial profile is monotonic.
  - Anisotropy is ≤ 5%: the profile along x vs. y and along the diagonals.
  - The half-energy radius is 4–8 px and the 99% energy radius is ≤ 60 px at 1080p.
  - Total energy out ÷ energy in is 1.00 ± 0.02 at strength 1.

### G2. Filmic tone map and grade

`tonemap = 1 − exp(−1.1c)` desaturates and hue-shifts bright greens and oranges toward white. That flattens the aurora hem, firefly cores, and the dusk band.

- Replace it with a hue-preserving curve: an ACES-fit or AgX-style shoulder applied to max(RGB), with the ratios kept. Do this in every effect's `toLin(tonemap(col))`.
- In `PSGrade`, add split toning per effect: shadows toward the effect's ambient color, highlights toward warm, each at ≤ 6% strength.
- **Check C27 (Python).** Ramp a saturated green (`#3CFF78`) and orange (`#FF6A20`) from 0 to 8 linear through the tone map. Hue drift is ≤ 4° up to output luma 0.85. The output stays monotonic. 8-bit banding: no step larger than 2 codes after the grade's dither.

### G3. Parallax depth everywhere

Today Matrix and Aurora use `lookX`/`lookY`, Fireflies only through `look.parallax`, and Stars not at all.

- Give every layered effect 3 depth planes with a shift at 1080p of 2 px (far), 6 px (mid), and 12 px (near) at full look deflection.
- Stars: scale the drift rate the same way, with the near layer moving 3× faster than the far one.
- **Check C28 (Python).** Render at `lookX = −1` and `+1`. Per layer mask, the measured horizontal shift is within ±25% of the target. The sky gradient and background stay put (shift < 0.5 px).

---

## 5. Stars

Priority order: S1 > S2 > S3 > S4 > S5 > S6 > S7.

**S1. A natural brightness distribution with a few hero stars.** Generate star flux from a power law: each brighter quarter-decade bucket holds 0.45–0.65× the previous one. Keep 3–8 hero stars per 1080p frame. The brightest 0.2% get subtle 2-spike diffraction: 12–24 px long, ≤ 12% of core intensity, and the same angle for all stars, as a real lens would give. Color temperature spans K to B with saturation ≤ 0.25. Today the three brightest buckets are equal.
- **Check C29.** Over local maxima above background + 0.06, the ratio of consecutive quarter-decade bucket counts is 0.45–0.65 for the top 4 buckets. Hero-star count is 3–8. Spikes, if present, have equal length within ±10%.

**S2. A real Milky Way.**
- Star density inside the band (|diag| < 0.12) is 2.5–4× outside (|diag| > 0.4), counting only stars above background + 0.06. Turn off the haze grain for this count.
- Replace the single straight rift with 3–5 branching dust filaments, built from domain-warped fbm and ridged noise along `along`. Their edges should be soft and should occlude stars as well as glow.
- The core is warm cream (`#E8D6B0` region): hue 30–45°, saturation ≤ 0.30. Today it is olive-brown, about hue 45–55° at higher saturation. Band edges are blue-grey.
- **Check C30.** The density ratio above. Take 20 cross-sections perpendicular to the band inside `riftSpan`: at least 70% show ≥ 2 local luminance minima deeper than 15%, because a single smear shows only 1. Core hue and saturation are in range.

**S3. Twinkle.**
- Replace `0.90 + 0.10·sin(...)` with band-limited value noise at 0.5–3 Hz per star.
- Amplitude depends on altitude: 20–45% in the bottom 40% of the sky, 5–15% near the top.
- Bright stars get a slight color scintillation, ±4% per channel, out of phase.
- **Check C31.** Over 60 frames at 30 fps, the per-star coefficient of variation of the core is: median 0.08–0.20, top-40% median ≥ 1.8× the median for the top 20% of the frame, and 95th percentile ≤ 0.5. The pairwise phase correlation between stars is ≤ 0.2.

**S4. Depth.** Use three star layers, far, mid, and near, with drift rates 1 : 1.8 : 3 and parallax per G3. Near stars are the brightest bucket.
- **Check C32.** Optical flow between t and t + 2 s, bucketed by brightness: the median speed of the top bucket is ≥ 2.5× that of the bottom bucket.

**S5. Meteors.** Make the gap between meteors random, 20–90 s, seeded per hour so the 3600 s loop holds. Vary peak brightness 3×, and give one in six a brief green-to-orange color change. Today a meteor appears every 9 s, 400 per hour, which reads as a screensaver.
- **Check C33.** Simulate 3600 s: 40–120 meteors, no two within 15 s.

**S6. Nebula preset.**
- Replace the two Gaussian blobs with domain-warped fbm emission clouds: Hα pink-red `#D0506A` mixed with OIII teal `#3FA7A0`, at saturation ≤ 0.45.
- Add darker dust filaments in front that dim stars behind them by 40–80%.
- **Check C34.** The nebula covers 15–35% of the frame above a threshold. High-frequency energy, the share of variance in a 1/8-res Laplacian, is ≥ 2× today's. The median luma of stars inside dust is ≤ 0.6× the median outside.

**S7. Warp.** Make streak length proportional to radial speed, short near the center and long at the edges, 4–80 px at 1080p. Keep a dark clear zone of radius 0.06 H at the center. Add a slight blue shift on the leading end.
- **Check C35.** Streak length correlates with radius at ρ ≥ 0.8. Mean luma inside the center zone is ≤ 0.3× the frame mean.

---

## 6. Aurora

Priority order: A1 > A2 > A3 > A4 > A5 > A6.

**A1. Lift the curtains into the sky and add ground under them.**
- Move `base` from `lerp(0.92, 0.64, heightK)` to `lerp(0.58, 0.40, heightK)`. The curtain's lower hem must never fall below 0.62 H.
- Add a dark landscape to every preset, not only Horizon:
  - Classic and Polar: snowy rolling hills with a pine treeline, reusing `groundPx` and `pineCard` from Fireflies, with the snow lit faintly by aurora color, ≤ 12% of the sky's aurora luma.
  - Magenta: a lake with a blurred reflection, as in A6.
- Lit hills are the biggest single reason the effect reads as flat.
- **Check C36.** For each column, the row of maximum luma above the silhouette is ≤ 0.62 H in ≥ 95% of columns. Today the median is 0.674 and the 95th percentile 0.703. A ground mask covers ≥ 95% of the bottom 18% of rows. Mean luma in the bottom 15% is ≤ 0.25× the mean of the aurora band.

**A2. Remove the sky crease.** In the third diffuse layer, change `i3 = smoothstep(-0.01, 0.004, a3)` to `smoothstep(-0.10, 0.06, a3)` and fold it into the main curtain's `glow`. Then check for the same pattern anywhere a `smoothstep` narrower than 0.02 multiplies a broad term.
- **Check C37.** Outside silhouette rows, the largest |second derivative| of the row-mean graded sRGB (8-bit units) is ≤ 0.4. Today it is 0.85 at about 0.545 H in Classic and 0.80 in Polar.

**A3. Stars behind the aurora.** Turn the star layer on by default: 150–400 stars at 1080p in the sky above the curtain, dimmed by `1 − 0.8·saturate(curtainLuma / 0.4)` so bright curtain areas wash them out.
- **Check C38.** 150–400 local maxima above background + 0.04 in the top 35% of the frame. The median star luma inside the curtain's bright region is ≤ 0.4× the median outside it.

**A4. Curtain depth and shape.**
- Draw 2–3 curtains at different depths with lift offsets of 0, +0.06, and +0.12. Give them parallax per G3 and speeds 1 : 0.7 : 0.5.
- Bend each curtain's path in plan and project it, so folds overlap and the edge-on parts brighten. Keep the existing `gain` from `dqx`, but clamp it to 2.0 and smooth it over 0.01 to stop sparkle.
- Make rays converge toward a vanishing point 0.4 H above the frame (a corona hint), with lengths varying 2–5× from the ray noise.
- Color by altitude:
  - Green (557.7 nm, `#5CFF8A` region) from the hem to +60%.
  - Faint red-magenta (630 nm) above that, ≤ 35% of the green's luma.
  - A thin blue-purple fringe under the hem only during surges (A5).
- **Check C39.** Ray length coefficient of variation is ≥ 0.35. Ray angles spread at least 8° across the frame. In the top 25% of the curtain, the R/G ratio is greater than in the bottom 50%.

**A5. Motion variety.**
- Keep the slow drift, and add surge events every 40–120 s, seeded per hour. Brightness rises 1.5–2× over 4–8 s, rays pulsate at 0.5–2 Hz for 10–20 s, then calm down.
- Between surges, the hem ripples sideways at 0.05–0.15 Hz.
- **Check C40.** Over 600 s of total aurora luma, sampled at 2 Hz: coefficient of variation 0.10–0.35, at least 3 surges (peaks ≥ 1.4× the rolling median), and no frame-to-frame jump over 8%.

**A6. Water (Horizon and Magenta).** Blur the reflection vertically by 6–14 px at 1080p, increasing with distance from the shore. Darken it by 0.45–0.6 and add 0.5–1.5 px horizontal ripple displacement. The hard silhouette reflection stays, but softened by 2 px.
- **Check C41.** The vertical-gradient energy of the reflection is ≤ 0.5× the sky's mirrored rows. Its mean luma is 0.45–0.6× the mirrored sky.

---

## 7. Fireflies

Priority order: F0 (bug 1) > F1 > F2 > F3 > F4 > F5.

**F1. Blink patterns that read as real fireflies.** Each fly gets a pattern from its hash:

| Pattern | Share | Shape |
|---|---|---|
| Slow glow | 55% | One 0.5–0.9 s flash every 4–7 s, attack 0.15 s, release 0.4 s |
| Double flash | 25% | Two 0.25 s pulses 0.35 s apart, every 5–8 s |
| J-stroke | 20% | A 0.6–1.0 s flash while the fly climbs 10–18 px in a J-curve, as *Photinus pyralis* does |

- Once every 60–150 s, seeded per hour, a cluster within 300 px blinks together, and the blink travels outward at 150–300 px/s. This is the "synchronous fireflies" moment.
- Off time is truly dark: the envelope stays under 0.02.
- The same function must exist in HLSL and Python (see 1.2, step 6).
- **Check C42.** Classify 200 flies by envelope over 30 s: shares within ±6% of the table. During a wave, ≥ 60% of the cluster flashes within its arrival window. Envelope max step per 1/30 s is ≤ 0.12 (the 1.11 C12 still applies).

**F2. Warm glow that falls off softly and lights the scene.**
- **Core and glow color.** The core is `#FFF6C8`. The glow is yellow-green, 555–570 nm (`#C8F060` region). The near layer is 20% less saturated.
- **Halo shape.** Replace the second Gaussian with `1 / (1 + (r/r0)²)^1.5`, r0 = 2.5–4 px at 1080p, multiplied by the Gaussian window from bug 1 so the reach stays bounded. The long soft tail beyond that comes from G1.
- **Local light.** Grass-fringe and pine-card pixels within 28 px of a lit fly get `+ glow · 0.08 · (1 − d/28)²`, so blades near a fly pick up a rim. Look up only flies already evaluated in the 3×3 block, at no extra fly cost.
- **Check C43.** On an isolated lit fly:
  - Luma at 3× core radius is ≥ 8% of peak, and at 8× is ≤ 3%.
  - The profile is monotonic and symmetric (C20).
  - Mean grass-fringe luma within 20 px of lit flies is ≥ 1.3× the fringe luma 60 px away.

**F3. Depth.**
- **Near layer.** Real defocused bokeh: discs 22–40 px wide with a 3–4 px soft edge, a faint brighter rim at 8% (lens-like), 3–8 on screen, and the same blink patterns. The 1.11 spec asked for this, but the shipped near layer is a small core.
- **Far layer.** Flies are 0.6× the mid size, 0.5× the brightness, and 10% bluer.
- **Parallax** per G3, applied to both trees and flies.
- **Check C44.** Mean apparent radius: near/mid ≥ 2.5 and far/mid ≤ 0.65. 3–8 near discs on screen across 16 sampled times.

**F4. Motion variety.**
- Give each fly a speed class: 70% drifters, 20% darters (3× speed in 1–2 s bursts), and 10% resters that hold still for 2–6 s.
- Keep the Lissajous paths. A rester's amplitude is multiplied by an envelope that goes to 0.
- **Check C45.** Over 30 s, per-fly mean speed has a 90th/10th percentile ratio ≥ 3, and ≥ 8% of flies are still (< 1 px/s) at any sampled instant.

**F5. Grade per preset.**
- **Summer dusk:** keep it.
- **Meadow:** pull the horizon band from saturated orange to hue 22–30° at saturation ≤ 0.75 at the brightest row, and give the field mist a 6% cool lift so flies separate from the band.
- **Deep woods:** add a faint moonlit rim to the mid trees (+15% on the sky-facing edge, 2 px).
- **Check C46.** The horizon band's peak saturation is ≤ 0.75 (Meadow). Fly-to-local-background contrast is ≥ 3:1 for 90% of lit mid flies in all presets.

---

## 8. Snowfall

Priority order: W0 (bug 2) > W1 > W2 > W3 > W4 > W5 > W6.

**W1. Flake shapes.**
- **Far (L1, 1–2 px):** points with a slight Gaussian.
- **Mid (L2, 3–5 px):** soft discs with a brighter core. The center/edge luma ratio is 1.6–2.2. Not flat alpha.
- **Near (L3, 6–9 px):** soft, irregular aggregate flakes. Use 4–6 variants in a 64×64 R8 sprite atlas generated by a script (`tools/gen_flakes.py`, baked into the exe). Each flake is rotated by its hash and spins slowly at 0.1–0.4 rev/s.
- **Bokeh (L4):** out-of-focus discs with a soft rim, alpha 0.08–0.15, 3–8 on screen.
- No hexagonal "clip-art" stars. At this size real flakes read as soft aggregates.
- **Check C47.** For components ≥ 4 px, the median center/edge luma ratio is 1.6–2.2 (today about 1.0). For L3, the boundary irregularity (perimeter² / 4π·area) has a median of 1.15–1.5. Circles score 1.0.

**W2. Motion blur.** Stretch each flake along its velocity, wind included, by `1 + v · (1/60 s) / diameter`, clamped to 1–2.2. Near flakes end up 1.3–2.0× longer. Far flakes stay round.
- **Check C48.** For the largest-area quartile, the ratio of extent along the velocity to across it is 1.3–2.0, and the orientation is within 10° of the measured motion vector (from C10's tracking). The smallest quartile's ratio is ≤ 1.15.

**W3. Motion variety.**
- Add flutter: per-flake zigzag or helix at 0.3–1.2 Hz, with amplitude proportional to size, 2–14 px at 1080p.
- Add gusts every 20–60 s, seeded per hour: wind rises 2× over 1.5 s and lasts 3–6 s. Near layers respond first, and far layers lag 0.5–1 s.
- **Check C49.** Over 60 s, the horizontal velocity variance of the near layer is ≥ 3× that of the far layer. At least 1 gust is detected, with a mean horizontal speed ≥ 1.6× the baseline for ≥ 2 s.

**W4. Depth and light.**
- Keep the 1 : 1.8 : 3 : 5 speed ratio across L1–L4 (C10 still applies).
- Tint far flakes 35% toward the sky color.
- In Night sky, flakes within 0.25 H of the moon are up to 1.4× brighter, as if lit from behind.
- **Check C50.** The median luma ratio of L3 to L1 flakes is ≥ 1.8. Night sky flakes inside the moon radius are ≥ 1.25× brighter than those outside.

**W5. Scenes.**
- **Night sky:**
  - Draw the moon: a 28–40 px disc at the existing glow center with a soft 3-ring halo.
  - Replace the slab with 2–3 rolling snow hills shaded from the height gradient with a moon-side light, plus faint blue shadows.
  - Add rare glints on the snow surface: 0.05–0.15% of snow pixels, 1–2 px, twinkling at 1–4 Hz.
  - Add a soft treeline on the far hill.
- **Pine forest:** snow caps on each tier's upper face for mid and near trees, 15–30% of the tier height and brightest toward the moon. The ground drift has soft blue shadows. Drop the dark rim on the drift edge.
- **Window:**
  - Replace the cellular frost with fern-like ice, using directional noise that grows inward from the pane edges and corners (stretch along the gradient of distance-to-edge).
  - Add a soft cool fog band at the bottom of each pane, about 12% of its height.
  - Have the lamp cast the window frame's soft shadow on the wall: a 4–8% darker cross, offset toward the lamp side.
  - Vary the wall with a 3–6% plaster texture so it is not a flat gradient.
- **Check C51.**
  - Night sky: the moon disc is detected (a radial peak ≥ 0.6 linear, radius 14–20 px). Glints are 0.05–0.15% of the snow mask and ≥ 40% of them change state across 1 s.
  - Pine forest: in the near tree mask, ≥ 10% of pixels are snow (luma ≥ 4× the tree median).
  - Window: frost structure-tensor coherence has a median ≥ 0.30 (directional ferns; cellular noise scores about 0.1). C14 still passes (frost 6–14% of glass). Wall luma has a coefficient of variation ≥ 0.03 inside the lamp falloff.

**W6. Stars versus flakes.** In Night sky, stars must be ≤ 1.2 px and twinkle (C31). Flakes must move (C9 still applies). With W1 and W2 in place, a still should tell them apart.
- **Check C52.** In a single frame, a classifier with two features (peak-to-area ratio and elongation) separates the star mask from the flake mask with ≥ 95% accuracy on the CPU reference.

---

## 9. Matrix

Priority order: M1 > M2 > M3 > M4 > M5 > M6.

Matrix needs a CPU reference before any of its checks can run headless. Add `tools/matrix_ref.py`, a Python port of `MatrixRain::rebuildColumns`, `update`, and `render`, at the level of the glyph instance list (rects, glyph index, color, alpha). Pure math, no font. Most of the checks below run on that list. Glyph shape checks run on the atlas from M1.

**M1. Glyph quality.**
- Stop depending on whichever Windows font has katakana.
- Ship a subset of an OFL font: halfwidth katakana U+FF66–FF9D, digits, `:*+-=.<>/|`, and the Latin set for glyph set 1. Noto Sans Mono CJK JP or M PLUS 1 Code both work, and the subset is ≤ 60 KB.
- Generate the atlas at build time with `tools/gen_glyph_atlas.py` as a 64 px single-channel distance field. Embed it as a resource and sample it with a 1 px smoothstep at draw size. Do mirrored glyphs with UV flips, not a second raster.
- Keep the GDI path only for the custom glyph set. When it is used, draw at 4× and box-downsample with mipmaps.
- This also gives the CPU reference the same glyphs.
- **Check M-C1 (Python, on the generated atlas).**
  - At 12, 18, 28, and 40 px draw sizes, ≥ 30% of inked pixels have partial coverage (16–239). A bitmap font scores about 0.
  - Stroke width varies ≤ 25% across glyphs.
  - No glyph's ink bounding box touches its cell edge, so there is ≥ 1 px of padding at 64 px.
- **Check M-C2 (`--selftest`).** Read back the atlas texture with the same partial-coverage rule. Print `MatrixAtlas: PASS partial=0.42`.

**M2. Trail falloff and color.**
- Use a two-part brightness curve along the trail:
  - k = 0 (head): 1.0, white-mint `#E8FFF0`.
  - k = 1–2: 0.85–0.75, bright green.
  - Then `exp(−k / τ)`, with τ = trail / 3.2.
  - The last 20% of the trail goes smoothly to 0 with smoothstep.
- Shift hue from the head's green to −10° teal-green at the tail, with saturation rising 10% as the trail darkens.
- Give each glyph a random brightness of ±12%.
- **Check M-C3 (instance list).** Along every trail, the envelope (excluding the ±12% random part) is monotonic after k = 2. The end alpha is ≤ 0.03. No step between adjacent cells is larger than 0.22, except head to k = 1. The tail hue is 6–14° away from the k = 3 hue.

**M3. Smooth head and real glow.**
- The head still steps cell by cell, as in the film. To keep that from reading as jumps, blend brightness over two cells: the glyph entering takes `frac(head)` of head brightness, and the one leaving takes `1 − frac(head)`.
- Remove the 4 offset copies. The glow comes from G1 bloom, with an extra 1.4× weight on heads, done by writing head intensity above 1.0 into the scene target before bloom.
- **Check M-C4 (instance list).** Sum the head brightness at 120 Hz sampling over 2 s for one column: the frame-to-frame change is ≤ 0.35 (today there are jumps of 1.0). **Check M-C5 (bloom reference):** the glow around a head is radially symmetric, with anisotropy ≤ 10% (today's plus shape scores about 60%).

**M4. Depth.**
- Use three discrete layers:

  | Layer | Share of columns | Glyph size | Brightness | Speed |
  |---|---|---|---|---|
  | Far | 35% | 0.55× | 0.35× | 0.5× |
  | Mid | 40% | 0.8× | 0.65× | 0.8× |
  | Near | 25% | 1.0× | 1.0× | 1.0× |

- Each layer has its own x grid, spaced at its own glyph size, so far columns fall between near ones. That creates density without lining up.
- The far layer samples one mip level down (slightly soft) and shifts 15% toward teal.
- Add parallax per G3.
- **Check M-C6 (instance list).**
  - The x positions modulo the near cell size spread over ≥ 6 distinct buckets of 8.
  - The size histogram has three peaks within ±5% of 0.55, 0.8, and 1.0.
  - The per-layer median speeds follow 0.5 : 0.8 : 1.0 within ±10%.

**M5. Glyph mutation.**
- Give each glyph its own change times, a Poisson process with a mean of 0.4–1.5 s. Heads change at 10–15 Hz.
- Remove the global `timeSalt` tick. Hash `(glyph, floor(time · rate_g + phase_g))` instead.
- **Check M-C7 (instance list).** At 30 fps over 10 s: 2–8% of trail glyphs change per frame, no frame exceeds 15%, and the per-frame change autocorrelation at lag 1 is ≤ 0.2. Today the 8 Hz tick makes every 4th frame spike.

**M6. Background and finish.**
- Keep black, but let the bloom leave a faint green ambient: frame-mean linear 0.002–0.006 at density 1.
- Add an optional slow 0.5% scanline at 3 px pitch, default off.
- Once every 20–60 s, a column briefly runs 2× faster with a brighter head (a "highlight column"), seeded per hour.
- **Check M-C8.** The frame-mean linear luma is in range. Over 600 s of simulation, there are 10–30 highlight columns.

---

## 10. GPU budget

The goal is to keep the GPU cost about where 1.11 is.

- **Baseline.** Before changing anything, record the `gpu=` value for every preset from Bobby's 1.11.0 `selftest.txt`, at his resolution, in a table at the top of the implementing PR. The only number in this review is Window: 3.38 ms at 1920×1200.
- **Rule.** Each preset's `gpu=` must be ≤ its 1.11 value × 1.10, plus 0.25 ms if G1 bloom is on. Bloom runs at 1/2 resolution and below. On WARP or at quality 0, bloom is off and the rule is × 1.10 alone.
- **Per-pixel cost changes and offsets:**

| Change | Cost | Offset |
|---|---|---|
| Fireflies 3×3 search (bug 1) | 12 → 27 fly evaluations | Band skip to rows within reach (about 35–45% of rows). Net ≤ 12 per pixel on average at 1080p |
| Snow per-column row (bug 2) | +1 hash, +1 round, +1 floor per column iteration | none needed |
| Snow sprites and motion blur (W1, W2) | +1 texture fetch on L3 only | L3 has about 80 flakes at 1080p, and the fetch only runs inside its disc test |
| G1 bloom | about 0.15–0.25 ms at 1080p on UHD 620 class | Drop analytic star halos and Matrix offset copies (−4 instances per head) |
| G2 tone map | +4 ALU per pixel | none |
| Aurora ground (A1) | reuses `pineCard` × 2 layers below the hem only | Skip `auroraRibbons` for rows below the silhouette (saves about 25% of pixels) |
| Stars Milky Way filaments (S2) | +1 fbm2 inside the band | Existing branch on `env > 0.35` |
| Matrix distance-field atlas (M1) | smoothstep per fragment | Fewer overdraw instances (M3) |

- **Check C53 (`--selftest`).** The rule above per preset, printing `budget=1.07x` on each preset line. The 1.11 doc's C17 (GPU time) stays.

---

## 11. Check numbering and where the checks run

The 1.11 doc used C1–C18. `tools/scene_check.py` implements C17 as the stair-step ground check. These new checks start at C19 to avoid a clash.

| Range | Effect | Runs in CI (`render_sky.py --check`) | Runs in `--selftest` |
|---|---|---|---|
| C19, C20, C22 | Fireflies clipping | yes | no |
| C21 | Fireflies clipping (GPU) | no | yes |
| C23, C24 | Snow persistence and clipping | yes | no |
| C25 | Snow persistence (GPU) | no | yes |
| C26–C28 | Bloom, tone map, parallax | yes | no |
| C29–C35 | Stars | yes | no |
| C36–C41 | Aurora | yes | no |
| C42–C46 | Fireflies art | yes | no |
| C47–C52 | Snowfall art | yes | no |
| M-C1, M-C3–M-C8 | Matrix (instance list and atlas) | yes | no |
| M-C2 | Matrix atlas readback | no | yes |
| C53 | GPU budget | no | yes |

- Every check must print its measured value next to PASS or FAIL, like the existing scene checks.
- Every check gets one test fixture that fails on 1.11.0. For the firefly and snow bugs those are the cases measured in sections 1 and 2.
- The CI step stays at half scale. Sweeps with many time samples (C20, C23, C40, M-C7) use 480×270 where the measurement allows it, keeping the CI scene step under 90 s.
- Regenerate the README stills with `--out docs` after the art lands. Stills should use t = 600 s, not 3.1 s, so a time-dependent bug like bug 2 cannot hide again.

---

## 12. Order of work for 1.12

1. Bug 1 (fireflies) and bug 2 (snow), with C19–C25. These can ship alone as 1.11.1 if Bobby wants the fixes sooner.
2. G1 bloom and G2 tone map, with C26 and C27. Re-tune the C6 luma ranges after G2, because the tone curve moves means slightly.
3. Matrix M1–M3 with `tools/matrix_ref.py`.
4. Aurora A1–A3, Snowfall W1–W2 and W5, Fireflies F1–F3, and Stars S1–S3.
5. The remaining items in each effect's priority order.
6. G3 parallax across all effects last, once the layers are final.

Keep the public repository free of private effect sources and assets. Nothing in this plan needs them.
