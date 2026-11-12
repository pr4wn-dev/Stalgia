# Stalgia 1.11 art direction: Fireflies and Snowfall

Implementation spec for the second pass on the Fireflies (`fireflies`) and Snowfall (`snow`) wallpapers. The first pass is commit `cf32f57` on `v1.11.0`. Everything below targets `PSFireflies` / `PSSnow` in `src/gfx/Shaders.h`, the CPU mirror in `tools/render_sky.py`, the presets in `src/effect/Presets.cpp` and `src/core/Config.h`, and the selftest in `src/host/Application.cpp`.

Conventions used throughout:

- **Sizes are pixels at 1920×1080.** Scale every length by `height / 1080`. Never derive a length from `uv` alone (see bug 5): work in pixel space, `px = uv * float2(width, height)`, and convert to cells with a cell size in pixels.
- **Colors are sRGB hex for the final graded look.** Convert to linear (`pow(c, 2.2)`) before using them in the scene. The scene is tonemapped with `1 - exp(-1.1 c)`, then graded. The values are tuned for brightness 1.0 with the night palette off.
- **"Alpha" means `lerp(col, layerColor, alpha * mask)`.** "Additive" means `col += ...`. Snow must be alpha-composited, not additive (see Snowfall).
- **Densities are on-screen counts at density 1.0.** The density knob scales counts linearly (0.15–2.2), never sizes.
- **Depth order is back to front.** Every list below is in draw order.

---

## 1. Diagnosis of the first pass

| Symptom | Cause in code |
|---|---|
| Trees read as hills or shark fins | `treeLine` is one smooth cone per cell, profile `pow(1 - t, 0.65)`, no tiers and no edge noise. |
| Back tree layer floats with a hard cut at ~65% | The far layer is called with `fill = 0`, so nothing paints below its ground line at 0.66, and `tt > 0.0` cuts every tree base on that row. |
| Hard flat ground edge | The ground for the mid and near layers is one perfectly horizontal `smoothstep` at a fixed `y`. |
| No atmospheric depth | The three layers use nearly black colors (`#0E0D0F`-ish) with no haze toward the horizon color. Mist is one flat band at the bottom. |
| Fireflies too uniform and too many | A single tint and a single `pow(sin, 6)` blink. Every fly has the same core and bloom per layer. The `0.22 * density` keep rate on a 10/22-cell grid puts 60–110 flies on screen, many in the open sky (`band` starts at `uv.y = 0.16`). |
| Deep woods unreadable | The sky is darkened toward `#040610` by `woods`, brightness is 0.82, and the night palette is on. Silhouettes and sky land within 1–2 code values of each other. |
| Snow reads as bokeh orbs | Snow is additive Gaussian glow (`core + halo`). Near flakes have σ ≈ 20 px with a 28 px halo, and about 22% of near cells are occupied. |
| Star dots mixed into the snow | Night sky draws `starLayer`. The far snow layer is 1 px sharp dots falling at about 7 px/s, so it also reads as stars. |
| Pine forest trees almost invisible | Tree colors `#0A0F12`/`#030405` against a sky of `#0C1020`, plus snow orbs over the top. |
| Window is two orange blobs | No frame, glass, frost, sill, or outdoor scene. Only two Gaussians. |

---

## 2. Fireflies

### 2.1 Presets

All three presets keep the existing knobs: brightness, saturation, vignette, FPS (30 by default), night palette, parallax, density, the Stars toggle, and the Glow color.

**Summer dusk (default, index 0).** Twenty minutes after sunset in a mixed forest. A thin warm band stays on the horizon, deep blue sits overhead, a few early stars show, and fireflies gather low among the trunks.

| Role | Hex |
|---|---|
| Zenith | `#0B1030` |
| Upper sky | `#1C1F45` |
| Mid sky (≈ 55% down) | `#3A2D52` |
| Horizon glow band (thin, 4–6% of H, centered on far ground line) | `#C2643C` → `#E8935A` at its core |
| Far trees (after haze) | `#2A2438` |
| Mid trees | `#141320` |
| Near trees and ground | `#07080B` |
| Mist | `#7A7F96`, alpha 0.18 |
| Firefly glow (knob default) | `#D2F26A`; core whitens to `#FFFBE0` |

Stars: 60–90 visible, only above the far treeline. Limit them to the top 45% of the frame and fade them out over the 10% above the horizon band.

**Deep woods (index 1).** Inside a dense conifer stand on a moonlit night. It is dark but readable: tree layers separate by value, and gaps between trunks show a cool blue-green sky. Fewer fireflies, but they are brighter and more saturated.

| Role | Hex |
|---|---|
| Sky between trunks (top) | `#0B1420` |
| Sky glow behind far layer | `#1E3138` |
| Far trees (after haze) | `#1C2A2E` |
| Mid trees | `#0F181B` |
| Near trees (frame the shot, may exit top of frame) | `#05080A` |
| Forest floor | `#0A100F` |
| Mist | `#4A6A6A`, alpha 0.28 (thicker, lower) |
| Firefly glow | `#B6E86A` |

The look settings change: brightness 0.95 (was 0.82), saturation 0.95, vignette 0.38, night palette off. The night palette crushes this preset. If the owner wants it, the tint has to be applied after the layer contrasts are met. No stars.

**Meadow (index 2).** An open field at late dusk. The treeline is low and far, and most of the frame is sky. Fireflies rise out of tall grass in the foreground.

| Role | Hex |
|---|---|
| Zenith | `#0A1236` |
| Mid sky | `#2B2C5E` |
| Horizon band | `#D9784A` → `#F2A766` |
| Far treeline (low, 8–12% of H tall) | `#2F2A3E` |
| Grass field | `#0B0D0C` → `#121612` toward the horizon |
| Foreground grass tufts | `#050605` |
| Mist | `#8A8FA0`, alpha 0.12, very low |
| Firefly glow | `#E8FF8A` |

Stars: 140–200 visible, starting just above the treeline.

### 2.2 Layer structure

Draw order, with heights given as fractions of H measured up from each layer's own ground line:

| # | Layer | Summer dusk | Deep woods | Meadow |
|---|---|---|---|---|
| 0 | Sky gradient + horizon band | yes | yes (dim) | yes |
| 1 | Stars | 60–90 | none | 140–200 |
| 2 | Far trees | ground 0.64, height 0.10–0.18, haze 0.60 | ground 0.58, height 0.18–0.30, haze 0.55 | ground 0.70, height 0.05–0.11, haze 0.65 |
| 3 | Mist band A (on far ground) | alpha 0.18 | alpha 0.28 | alpha 0.12 |
| 4 | Fireflies, far | 14–20 | 6–10 | 25–35 |
| 5 | Mid trees | ground 0.78, height 0.20–0.34, haze 0.28 | ground 0.74, height 0.35–0.60, haze 0.30 | none |
| 6 | Mist band B (on mid ground) | alpha 0.12 | alpha 0.18 | none |
| 7 | Fireflies, mid | 14–20 | 8–12 | 25–35 (in grass) |
| 8 | Near trees + ground | ground 0.93, height 0.40–0.75, haze 0.03 | ground 0.95, height 0.75–1.20 (exit top), haze 0.0 | grass tufts only, 6–12% of H |
| 9 | Fireflies, near (bokeh) | 1–3 | 0–2 | 2–4 |

Parallax: the parallax knob multiplies `lookX` per layer as stars 0.05, far 0.15, mid 0.45, near 0.90, and near bokeh 1.2. `lookY` uses half of those factors. The max offset at `lookX = 1` is 1.5% of W for the far layer and 9% of W for the near layer. Pad the horizontal tree lattice so a layer never reveals an edge.

Ambient drift: the tree layers sway by 0 px; trees are static. Mist scrolls at 4–8 px/s horizontally.

### 2.3 Conifer silhouette generation

Replace `treeLine` with a tiered conifer generator. It still uses analytic coverage per pixel with no textures.

1. **Placement.** Tile x in pixels with a cell width of 55 px (far), 95 px (mid), and 190 px (near). Each cell holds up to two trees. Tree A is present with probability 0.92 and tree B with 0.45. Each tree's center is jittered across the whole cell width, so neighbors overlap and spacing looks Poisson-like, not periodic. Evaluate cells `k-1..k+1` (three cells × two trees = six trees per layer).
2. **Height.** `h = layerHeight * lerp(0.55, 1.0, hash)^0.7`. Pick one tree in eight to be 1.25× taller, which breaks the rhythm.
3. **Base profile.** Half-width at normalized height `t` (0 at the base, 1 at the tip): `w(t) = baseHalfW * (1 - t)^0.85`. Use `baseHalfW = h * lerp(0.16, 0.24, hash)` for spruce and fir. In Summer dusk, one tree in four is a narrow cypress-like shape at `0.10`.
4. **Branch tiers (the important part).** Tier count `N = round(lerp(7, 12, hash))`, with fewer tiers near the top. With `s = frac(t * N + tierJitter)`, the tier droop is `tier = lerp(0.62, 1.0, pow(1 - s, 1.6))`. Each tier is widest at its bottom edge and steps in at the next tier up, which gives the sawtooth outline of real conifers. Multiply: `w(t) *= tier`.
5. **Needle noise.** Displace the edge with `w += baseHalfW * 0.10 * (vnoise(float2(y_px / 6.0, treeSeed)) - 0.5)`. Add a second octave at `y_px / 2.5` with amplitude 0.05 for mid and near layers only. Far layers skip the noise on WARP.
6. **Asymmetry.** Left and right edges use different noise seeds. Lean the tip by up to ±3% of `h`, linearly with `t`.
7. **Tip.** The top 6% of the height is a 1–2 px wide spike. Do not round it off.
8. **Trunk.** Near layer only: draw a trunk 4–10 px wide from the ground to `t = 0.08`, visible under the lowest tier. Deep woods near trees are trunk-dominant: bottom tier at `t = 0.25`, with bare trunk below.
9. **Antialiasing.** Coverage is `saturate((w(t) - |dx_px|) + 0.5)`. That is a 1 px ramp, the same at every layer and resolution. The current code uses `(height / scale) * 0.55`, which gives a soft 3–8 px edge.
10. **Deciduous mix (Summer dusk only).** One tree in five in the mid and near layers is a broadleaf. Its canopy is the union of four or five circles. Radii are 0.25–0.45 of `h`, placed on an arc and displaced with fbm (2 octaves, 30 px scale, amplitude 15% of radius), on a 6–12 px trunk.

**Meeting the ground without cut-offs.** Every tree layer is a full card, opaque from its ground line to the bottom of the frame. There is no `fill = 0` mode anymore.

- Each layer's ground line undulates: `ground_px(x) = ground0 + A * (vnoise(x / L) - 0.5)`. Far uses A = 6 px, L = 400 px; mid uses A = 10 px, L = 300 px; near uses A = 16 px, L = 220 px. Meadow grass uses A = 8 px.
- Trees root below the local ground line. The tree's `t = 0` sits at `ground_px(treeX) + 0.02 * h`, so the lowest tier is partly buried and no base edge is ever visible.
- Remove the `tt > 0.0` gate. Coverage is `max(groundMask, treeMask)`, where `groundMask = saturate(y_px - ground_px(x) + 0.5)`.
- Meadow foreground grass is a tuft field along the bottom 6–12% of H. Blades are 2–5 px wide at the base, 15–60 px tall, and lean ±15°. Use a cell width of 9 px with three blades per cell, drawn as tapered wedges with the same 1 px antialiasing.

### 2.4 Atmospheric perspective and mist

- **Haze per layer.** `layerColor = lerp(silhouetteColor, skyColorAt(ground_px), haze)`, where `skyColorAt` is the sky gradient sampled at that layer's ground line. Haze values are in the table in 2.2. The far layer must be visibly lighter and bluer or purpler than the mid layer. Required contrast is in 2.7.
- **Height fog on tree layers.** Within each layer, darken toward the top: `layerColor *= lerp(1.0, 0.85, t)`. That is the opposite of a rim light, and it keeps the tips crisp against the bright horizon band.
- **Horizon band.** It is a warm Gaussian row centered just above the far ground line: σ = 3% of H, plus a softer σ = 12% at 0.35 strength. It is the brightest part of the sky and sits behind the far trees, so the far silhouette reads strongly against it.
- **Mist bands.** One band sits at each layer's ground line, 6–12% of H tall. Its density is `fbm2(float2(x_px / 380 + t * 0.012, y_px / 140))` with two octaves, remapped `smoothstep(0.35, 0.75, n)`. Its vertical profile is `exp(-(dy / bandHalfHeight)^2)`. Its color is the horizon color desaturated by 60% and darkened by 0.6. Max alpha is per preset (2.1). Mist goes over the tree layer it belongs to and under the fireflies of the same depth. A far firefly inside mist gets its bloom multiplied by `1 - 0.6 * mistAlpha`.
- **WARP / quality 0:** use one fbm octave and skip mist band B.

### 2.5 Fireflies (the lights)

- **Counts** are per preset in 2.2. Total on screen at density 1.0 is 30–45 for Summer dusk, 15–24 for Deep woods, and 55–75 for Meadow. About 55% are lit at any instant.
- **Altitude.** 85% of flies live between their layer's ground line and 0.6 of that layer's tree height. 15% may wander up to 1.2 of the tree height. None appear in the top 30% of the frame in Summer dusk or Deep woods. Meadow allows them up to 45% from the top, and they thin out with height.
- **Size at 1080p**, per layer:

| Layer | Core radius | Bloom σ | Peak linear (core) | Bloom gain |
|---|---|---|---|---|
| Far | 0.8–1.2 px | 4–6 px | 1.2 | 0.35 |
| Mid | 1.3–2.0 px | 7–10 px | 2.0 | 0.55 |
| Near bokeh | n/a (defocus disc 22–40 px diameter, flat with a 4 px soft edge) | n/a | 0.18 alpha-ish additive | n/a |

- **Color variation.** Per fly, hue shifts ±8° around the Glow knob and saturation varies ±10%. The core blends toward `#FFFBE0` by `0.6 * pulse`, so brighter means whiter. Near bokeh discs are 20% less saturated.
- **Blink.** Each fly has a smooth envelope, not `pow(sin)`. On time is 0.6–1.6 s, off time is 1.8–6.0 s, attack is 0.25 s, and release is 0.55 s with smoothstep curves. Period and phase are random per fly. One fly in eight uses a double-flash pattern (two 0.3 s pulses 0.35 s apart). The blink clock scales by `lerp(0.7, 1.3, speed / 2)`, so speed changes blink tempo only slightly.
- **Motion.** Each fly wanders on a slow Lissajous path made of two sines per axis with incommensurate frequencies (0.05–0.17 Hz). Amplitude is 20–60 px horizontally and 10–35 px vertically for mid, scaled by 0.5 for far and 1.6 for near. Fireflies tend to rise slightly while lit: +6 px over the on time. Speed is 8–25 px/s, scaled by the speed knob. Note: the first pass's `p.y += time * ...` makes the whole field scroll upward forever. Do not do that.
- **Cell evaluation.** With the wander amplitudes above, a fly can travel past its cell. Use cells of at least 2 × (max wander + 3σ bloom) and evaluate a 2×2 neighborhood: the cell plus the nearest neighbors on x and y, picked by `f < 0.5`. That replaces the current 1×1 lookup, which clips glows (bug 4).
- **Compositing.** Additive, after the layer's mist. Bloom must not light the trees in front of it: a mid fly is drawn before the near trees, so the near card covers it naturally. Keep that draw order.

### 2.6 Performance (Fireflies)

- **Budget:** ≤ 1.2 ms GPU at 1920×1080 on Intel UHD 620 or Radeon Vega 8. ≤ 0.5 ms on a GTX 1050-class card.
- **Per-pixel cost:**
  - Trees: three layers × three cells × two trees. Skip a layer with an early `if` when `y_px` is above `ground - maxHeight`, so the sky rows never pay for trees.
  - Fireflies: three layers × a 2×2 neighborhood, for 12 evaluations. Use one `exp` for the core and one for the bloom.
  - fbm: two octaves, mist rows only. Branch on `y_px` within the band ±3σ.
- WARP drops the far firefly layer, needle noise octave 2, and mist band B.

### 2.7 Fireflies acceptance numbers (CPU reference and GPU capture)

These feed the checks in section 5.

- **Display luma** (graded sRGB, mean over the frame): Summer dusk 0.07–0.14, Deep woods 0.045–0.09, and Meadow 0.08–0.16.
- **Layer separation:** median linear luma ratio far/mid ≥ 1.6 and mid/near ≥ 1.4, sampled inside each layer's mask with the fireflies off.
- **Lit fraction** (linear ≥ 0.02, the existing selftest threshold, measured on the scene without the sky): 0.3–4% of pixels. The sky band itself may exceed 0.02 in Summer dusk and Meadow, so measure "flies lit" against a fireflies-off render, or use the debug mask (5.2).

---

## 3. Snowfall

### 3.1 Principles

- Snow is **alpha-composited white-gray discs** over the backdrop, not additive glow. A flake can never be brighter than its color times its alpha. That alone removes the bokeh look.
- **Size, blur, opacity, and speed all come from depth.** Near flakes are bigger, softer, faster, and fewer. Far flakes are tiny, sharp, slower, and many.
- **No stars while it is snowing.** Snow falls from overcast skies. Night sky becomes an overcast night with a moon glow through the clouds.
- **The backdrop must be lighter at the bottom than the top.** Snow on the ground reflects the sky. This is what makes a night snow scene read as snow.

### 3.2 Flake layers

Use four layers. Counts are flakes visible on screen at density 1.0. Speeds are in screen px/s at speed 0.40, the default, and scale linearly with the speed knob.

| Layer | Diameter (log-normal, ±30%) | Blur σ | Alpha | Count | Fall speed | Sway amplitude / freq | Stretch |
|---|---|---|---|---|---|---|---|
| L0 far | 1.2–2.0 px | 0 (1 px antialiased edge) | 0.35–0.50 | 900–1300 | 28–40 | 3–6 px / 0.5–0.9 Hz | none |
| L1 mid | 2.5–4.0 px | 0.5 px | 0.55–0.75 | 300–450 | 55–80 | 8–14 px / 0.35–0.7 Hz | none |
| L2 near | 5–8 px | 1.0–1.8 px | 0.70–0.90 | 70–120 | 100–150 | 15–28 px / 0.25–0.5 Hz | ×(1 + 0.04 × px/frame), ≈ 1.15–1.2 |
| L3 foreground (optional) | 18–36 px | defocus disc, flat with a 3–5 px soft rim, *not* Gaussian | 0.10–0.18 | 3–7 | 180–260 | 30–50 px / 0.2–0.35 Hz | ×1.3 along velocity |

- **Flake shape.** L0 and L1 are discs. L2 is a disc with `vnoise` edge displacement of 12% of the radius, so clumps look irregular. L3 is a disc whose interior is 20% brighter at the rim, like a lens defocus ring, kept very subtle.
- **Color.** The flake color knob (default `#F7F8FF`) is multiplied by the backdrop's ambient: Night `#B9C6DA`, Pine `#C2CEDC`, and Window outside `#AEBBD0`. That keeps flakes from glowing pure white against a dark sky. L3 is tinted 15% toward the backdrop color.
- **Wind** (knob −1..1) adds horizontal drift of `wind × fallSpeed × 0.7` per layer and tilts L2/L3 stretch along the velocity vector. Add gusts: `wind_eff = wind × (1 + 0.35 × (vnoise(t × 0.07) − 0.5))`, shared by all layers, with lag per layer (far lags 0.6 s behind near).
- **Sway** is per flake, not per row. The phase is `hash(cell)`. The first pass's `sin(... + uv.y * 3.0 ...)` shears whole rows together, which reads as a wavy grid.
- **Fall-speed jitter** is ±20% per flake within a layer, so a layer never moves as one rigid sheet. Implement it by giving each column of cells its own speed: offset y per column `i` by `t × v × (1 + 0.2 × (hash(i) − 0.5))`. That is cheap and removes the rigid sheet look.
- **Cell sizing.** Choose the cell size so the max visible radius plus sway is under half a cell. Then each pixel needs only its own cell plus one neighbor in x (for sway): 2 evaluations per layer instead of 9. L3 uses a 2×2 neighborhood. Approximate cell sizes at 1080p:

| Layer | Cell size (x by y) | Occupancy |
|---|---|---|
| L0 | 48 × 40 px | ≈ 90% |
| L1 | 80 × 72 px | ≈ 85% |
| L2 | 150 × 140 px | ≈ 70% |
| L3 | 420 px | ≈ 25% |

Tune occupancy to hit the counts.

- **Ground interaction.** None. Flakes pass behind the near ground silhouette in Pine forest, because L0–L2 are drawn before the near tree card. L3 is drawn last.

### 3.3 Presets

**Night sky (default, index 0).** An overcast winter night over a gentle snowy field. It is quiet, with a faint moon glow through the clouds.

| Role | Hex |
|---|---|
| Sky top | `#0A1220` |
| Sky horizon | `#1B2A3F` |
| Cloud texture (fbm, 2 octaves, 600 px scale, drifts 3 px/s) | `#142033` at alpha 0.35 |
| Moon glow (σ = 18% of H, centered at 0.72 W / 0.22 H, *no disc*) | `#9DB4D6`, peak alpha 0.22 |
| Distant hill line (ground ≈ 0.80, A = 10 px, L = 500 px) | `#2A3546` |
| Snow field | `#33415A` at the hill line → `#4A5A74` at the bottom |

No stars. Layers L0–L2 are on; L3 is off. Speed 0.40, density 1.0, wind 0.22. Look: brightness 1.0, saturation 0.9, and vignette 0.22.

**Pine forest (index 1).** A snowy spruce forest at night. The tree layers must be clearly visible, with snow on the branches.

| Role | Hex |
|---|---|
| Sky top | `#0E1828` |
| Sky horizon | `#26364C` |
| Far pines (haze 0.55) | `#2B3A4E` |
| Mid pines | `#18222F` |
| Near pines | `#0B1018` |
| Snow on branches (top edge of every tier, 1–3 px thick far, 3–7 px near) | `#B9C8DA` at 0.55 far, `#D3DEEA` at 0.75 near |
| Ground snow | `#3A4A60` → `#5A6B82` |

Use the same conifer generator as Fireflies (2.3), with heavier tiers: tier droop `lerp(0.55, 1.0, …)` and 8–11 tiers. The branch snow is a band just under each tier's top step. Its coverage is `saturate(1 - (s / snowThickness))` inside the tree mask, broken up by `vnoise(x_px / 5)` at 40% so the band is patchy. Layer grounds: far 0.62, mid 0.76, near 0.92, each undulating as in 2.3. Snow layers draw in this order: far pines, L0, mid pines, L1, near pines + ground, L2. All three snow layers run, L3 is off, and mist band A runs at alpha 0.10 in `#5E708A`. No stars. Speed 0.48, density 1.15, wind 0.38. Look: brightness 1.0 (not 0.96), saturation 0.9, vignette 0.30, night palette off.

**Window (index 2).** See section 4.

### 3.4 Performance (Snowfall)

- **Budget:** ≤ 1.5 ms GPU at 1080p on UHD 620 or Vega 8. Window is allowed 2.0 ms.
- **Per-pixel cost** at default density: L0 + L1 + L2 take 2 evaluations each, and L3 takes 4, for 10 disc evaluations. Each evaluation is one `hash22`, one length, and one smoothstep. No `exp` is needed: discs are `smoothstep(r + blur, r - blur, d)`. Trees cost the same as Fireflies. Clouds use one fbm with two octaves.
- **If the fullscreen pass misses budget** on UHD 620, move L0–L3 to instanced quads, as Matrix already does. That is about 2,000 instances, a 4-vertex quad each, positions integrated on the CPU in double precision. That path is cheaper than any fullscreen loop and also fixes the time-wrap pop (bug 6).
- WARP drops L0 and the cloud fbm.

---

## 4. Window preset

The viewer is **inside a warm room at night, looking out through a wooden window** at a snowy night scene. The "soft blurred warm interior glow" is the lamplight in the room. It falls on the wall, the frame, and the sill, and it shows as a faint reflection in the glass. It is not two blobs floating in the dark.

### 4.1 Composition (1080p)

- **Window opening.** A rectangle 1240 × 820 px centered at (0.52 W, 0.47 H), slightly off-center and slightly above the middle.
- **Outer frame.** 34 px wide.
- **Mullions.** 12 px wide, splitting the opening into a 2 × 3 grid: two columns and three rows, six panes.
- **Inner sill.** A 60 px tall ledge under the opening, top face 18 px, in perspective as a lighter strip.
- **Room wall.** Everything outside the frame.

### 4.2 Palette and lighting

| Role | Hex |
|---|---|
| Room wall | `#140C08` (far corner) → `#2A1810` (near lamp) |
| Frame wood (unlit) | `#2E1C12` |
| Frame wood (lamp side) | `#5A3A24` |
| Frame inner bevel highlight (1–2 px on edges facing the lamp) | `#8A5C3C` |
| Sill top face | `#6A4630` |
| Lamp light | `#FF9E4F` |

- **Lamp.** An off-screen point at (−0.10 W, 0.95 H), lower left. Its contribution is `lampColor × 0.30 / (1 + (d / 0.45H)^2)`, applied to the wall, frame, and sill only. That is the warm interior glow.
- **Frame shading.** Lit faces get a bevel term: the frame edges whose normal faces the lamp get the highlight, and the opposite edges get `#1C110B`. The frame must read as 3D wood: inner edge darker, outer edge lighter on the lamp side. Optional wood grain is `vnoise(float2(x_px / 2, y_px / 60))` at ±6% on the vertical members, transposed on the horizontals.

### 4.3 Through the glass (outdoor scene)

Draw this inside the pane masks only. It is slightly defocused, because the focus is on the frame:

| Role | Hex |
|---|---|
| Sky | `#0C1626` → `#1A2840` |
| Distant treeline silhouette (conifer generator, far settings, extra blur, ground 0.70 of the opening) | `#121C2A` |
| Snow ground | `#2A3850` → `#3C4C66` |
| Neighbor window lights: 2–3 small warm bokeh discs, 8–14 px, on the treeline, flat with soft edges | `#FFB46A`, alpha 0.5 |

- **Snow outside.** Layers L0–L2 per 3.2, masked to the glass, with blur σ +0.4 px on every layer for the defocus. Density 0.75, wind 0.10, and speed 0.28 by default.
- **One or two flakes stuck to the glass.** Static, 3–5 px, alpha 0.4, positioned by hash so they are stable.

### 4.4 Glass, frost, and reflection

- **Glass tint.** Multiply outdoor color by `#D8E4EE` (a slight cool loss). Add a vertical sheen: `+0.012` linear in a 40% wide diagonal band across each pane.
- **Interior reflection.** Add a faint mirrored copy of the lamp glow on the glass: `lampColor × 0.07`, a large soft blob (σ = 25% of pane width) on the lower-left panes. That gives the warm reflection.
- **Frost.** Frost grows from each pane's corners and bottom edge.
  - Mask: `frost = smoothstep(0.28, 0.0, cornerDist / paneSize) + 0.6 × smoothstep(0.12, 0.0, bottomDist / paneHeight)`.
  - Modulation: multiply the mask by ridged noise, `1 - abs(2 × fbm2(px / 22) - 1)` with two octaves, then raise it to the power 1.5 so it looks dendritic, not cloudy.
  - Color: `#D7E6F3`, max alpha 0.55. The frost picks up lamp light on lower-left panes, tinted 15% toward `#FFC89A`.
  - Coverage target: 6–14% of the glass area.
- **Outer sill snow.** A soft white mound along the bottom of each bottom-row pane, *outside* the glass. Height 14–26 px, top edge `vnoise`-displaced at 6 px amplitude, color `#C8D4E2`. It is lit faintly by window spill on its top: `+lampColor × 0.04`.
- **Condensation (optional).** A slight blur and brightness lift (+0.008) in the bottom 15% of each pane.

### 4.5 Look settings

Brightness 1.0, saturation 1.05, vignette 0.30, night palette off. The flake color knob (default `#FFF4E4`) affects the outdoor snow only.

### 4.6 Performance

The frame, mullions, and sill are analytic boxes. Frost is two octaves inside a corner and bottom-edge mask, so branch it out elsewhere. Budget is ≤ 2.0 ms at 1080p on UHD 620.

---

## 5. Objective checks for selftest, capture, and CI

The current selftest only checks `max luma ≥ 0.02`, `lit ≥ 48`, and `0.2% ≤ lit fraction ≤ 40%`. The bokeh snow passed at 17%. Add the checks below. Implement each one twice: in C++ against the scene readback in `--selftest`/`--capture`, and in `tools/render_sky.py --check`. The Python version lets CI run the checks on Linux without a GPU (see 5.4).

### 5.1 Deterministic capture

- Add `--capture-time T`, with a default of 4.2 s for Fireflies and 3.1 s for Snow, matching `render_sky.py`. `--capture` then renders each preset at a fixed time and seed and writes the PNG.
- Capture a second frame at T + 0.5 s for the motion checks.

### 5.2 Debug mask mode

Add a debug value on an unused constant-buffer slot. `p3 = -1` when the selftest asks for masks, and the effect writes masks instead of color:

- Fireflies and Pine forest: R = far tree card, G = mid card, B = near card, A = fly coverage.
- Window: R = frame, G = glass, B = frost, A = sill snow.

Render one extra frame per preset in debug mode during the selftest. `render_sky.py` returns the same masks from the same functions.

### 5.3 Checks

| # | Check | Applies to | Pass condition |
|---|---|---|---|
| C1 | **No floating card.** Every tree card covers the bottom row of the frame. | Fireflies, Pine forest | 100% of columns have mask ≥ 0.5 at `y = H - 1` for R, G, and B. This catches the 65% cut. |
| C2 | **No hard horizontal cut.** For each card, find the lowest row where coverage goes from 1 to 0 in each column (bottom of a floating card), or the ground line. | Fireflies, Pine forest | No single row holds more than 3% of the columns' ground-line transitions. This catches flat ground and floating cards. |
| C3 | **Silhouette roughness.** For the G and B cards: skyline `h(x)` = topmost covered row per column. Edge pixel count of the mask (pixels with a 4-neighbor of different coverage) divided by W. | Fireflies, Pine forest | Mid ≥ 3.0 and near ≥ 3.5. Smooth cones measure 1.6–2.2; tiered conifers measure 4–7. Also: skyline local maxima (tree tips) per 1000 px are within [5, 25] for mid and [2, 12] for near. |
| C4 | **Tiered outline.** For each skyline maximum, sample half-width at 10 steps down the tree. | Fireflies, Pine forest | At least 3 decreases in half-width while going down, which proves tiers exist. Smooth cones have 0. |
| C5 | **Layer separation.** Median linear luma inside R, G, and B (fireflies off, from the mask frame). | Fireflies, Pine forest | far/mid ≥ 1.6, mid/near ≥ 1.4, and near ≥ 0.0015 linear, so it never crushes to 0. |
| C6 | **Readability.** Graded sRGB luma, 5th to 95th percentile spread, and mean. | All presets | Spread ≥ 0.05. Mean is within the 2.7 ranges (Fireflies). For Snow: Night 0.08–0.16, Pine 0.07–0.15, and Window 0.08–0.18. Deep woods fails this today. |
| C7 | **Blob statistics.** Threshold `luma > localMedian(31×31) + 0.03` (linear) and take connected components (8-connectivity). | Snow (all), Fireflies | Snow: 600–3000 components at 1080p (scaled by H²). Max component area ≤ 1600 px². 95th percentile area ≤ 90 px². The current pass has components of 3,000–8,000 px². Fireflies: 12–90 components, max area ≤ 700 px² (bokeh excluded via mask A). |
| C8 | **Compositing sanity.** The brightest snow pixel is not brighter than the flake color times the backdrop ambient times 1.1. | Snow | Max linear luma over flake pixels ≤ `luma(flakeColor × ambient) × 1.1`. That catches additive glow sneaking back. |
| C9 | **Stars vs. snow (motion).** Match components between T and T + 0.5 s within 1.5 px. | Snow | ≤ 3% of components are stationary (stuck glass flakes are allowed in Window and are excluded by position hash). Stars or a static far layer fail this. |
| C10 | **Fall speed by depth.** Track components between T and T + 0.5 s and bucket by area. | Snow | The median vertical speed of the largest-area quartile is ≥ 2.5× that of the smallest-area quartile. The first pass is about 3× on paper but the far layer looks static. Also: no component moves upward. |
| C11 | **Fireflies stay low.** Fly pixels (mask A). | Summer dusk, Deep woods | ≤ 2% of fly pixels are in the top 30% of the frame. |
| C12 | **Blink is smooth.** Render 6 frames at 1/30 s steps. | Fireflies | The per-frame change of any fly's core luma is ≤ 0.12 linear. At least 25% and at most 80% of flies change state across 3 s. |
| C13 | **Calm motion.** Mean absolute linear difference between consecutive frames at 30 fps. | All presets | Between 0.0003 and 0.006. Static or frantic both fail. |
| C14 | **Window structure.** From the Window mask frame. | Window | Frame coverage 8–20% of the screen. Glass forms exactly 6 connected components. Frost covers 6–14% of glass. Sill snow > 0. Outside snow pixels (C7 components) exist only inside glass. |
| C15 | **Time wrap.** Render at `T = 3599.99` and at the wrapped time `0.0 + (T - 3600 + step)`. | Fireflies, Snow | Mean absolute difference ≤ 3× the C13 value. That fails today (bug 6). |
| C16 | **Aspect invariance.** Render the default preset at 1920×1080 and 2560×1080, then crop the central 1920 px. | Fireflies, Snow | C7 component median area within ±15% and the median component aspect ratio (bbox w/h) of L1 flakes within 0.8–1.25. That catches bug 5. |
| C17 | **GPU time.** `lastGpuMs` on non-WARP adapters. | All presets | ≤ 2.5 ms at the selftest preview size, scaled to 1080p by pixel count. Log a warning above 1.5 ms. |
| C18 | **CPU/GPU parity.** Downsample the GPU capture and the `render_sky.py` output of the same preset and time to 480×270. | All presets | Mean absolute sRGB difference ≤ 0.03. That keeps the README stills honest. |

### 5.4 Where they run

- **`--selftest` (desktop, real GPU):** C1–C17 on every Fireflies and Snow preset. Print one line per preset with the values, like the existing `luma= lit= cover=` line, so a regression is visible in the log.
- **`--capture`:** writes the color PNG, the mask PNG, and a `metrics.json` per preset.
- **CI (`build.yml`):** add a step after the build:

  ```bash
  pip install numpy pillow
  python3 tools/render_sky.py --check --scale 0.5 --out /tmp/stills
  ```

  This runs C1–C11, C13, C14, C15, and C16 on the CPU reference and fails the job on any violation. The step takes about 30 s at half scale. Keep the README stills generated by the same command with `--out docs`.

---

## 6. Code-level bugs found in the 1.11.0 diff

These are independent of the art changes. Severity is for users on the public build.

1. **New Fireflies and Snow tune generations are never stamped on new or reset configs (medium).** `Config::load` writes `kTune*` for every effect in the "no file" path and the "parse failed" path, but not `tuneFireflies` or `tuneSnow`, which stay at the struct default of 0. The first launch saves `"fireflies": 0, "snow": 0`. On the second launch, `migrateStaleTunes` resets both blocks to defaults, which throws away any Fireflies or Snow tweaks made in the first session, and does so on every launch until a migration save happens. The "empty file" path stamps no tunes at all, an older bug that now also hits the new effects. Fix: one `stampCurrentTunes()` helper called from all three paths.
2. **Selftest coverage uses two different pixel counts (low).** `lit` counts the scene target (`m_sceneW × m_sceneH` in `sampleSceneLuma`), but `cover` divides by `m_renderer.width() × height()`. They match today, but it breaks as soon as the scene target is scaled. Return the sampled pixel count from `sampleSceneLuma`. The 0.40 upper bound is also too loose to catch anything (see C7 and C8).
3. **The far tree card floats (art bug, high visibility).** In `PSFireflies` the far layer is `treeLine(..., fill = 0.0)`, and `treeLine` gates trees with `tt > 0.0`. The card has no ground and every base is cut on the same row (`ground = 0.66`). That is the reported 65% line. Snow's Pine forest far layer has the same problem at 0.64.
4. **Firefly glow is clipped at cell borders (medium visibility).** `addFlies` evaluates only the fly's own cell, but jitter (±0.275 cell) plus drift (±0.22 cell) can put the center 0.495 cell from the middle. The bloom gets cut flat whenever a fly is within about 3σ of a border, which shows as half-moon or flat-bottomed glows in the upper-left of `docs/fireflies.png`. Fix per 2.5.
5. **Shapes are not aspect-correct.**
   - `treeLine` takes `uv.x` without `× aspect`, so trees widen on 21:9 monitors and on spanned multi-monitor desktops.
   - `addSnow` and `addFlies` compute `dist = length(d) * (height / scale)` with cell fractions whose x and y pixel sizes differ (`width / scale` vs `height / (0.72 scale)`). Flakes and glows come out about 1.28× wider than tall at 16:9, and their real radius differs from the intended pixel radius.
   - Fix: do all of this in pixel space.
6. **Visible pop every hour (medium).** `Fireflies::render` and `Snow::render` pass `wrapMod(m_time, 3600)`, and the shaders use `time × speed × k` as positions. At the wrap every flake and fly jumps. Snow is the worst case: at the default speed the near layer's offset at the wrap is about 75 screen heights. Fix: integrate per-layer offsets on the CPU in double, wrap them modulo an integer cell period, and make the hash lattice periodic over that period. Or move snow to instanced quads (3.4).
7. **The private-effects hook works but has sharp edges (low).**
   - No check that the folder defines `registerPrivateEffects`. A folder without `register.cpp` fails at link time with an undefined reference instead of a clear CMake error. Add `if(NOT EXISTS "${STALGIA_PRIVATE_EFFECTS_DIR}/register.cpp") message(FATAL_ERROR ...)`.
   - No guard against pointing it inside the repo. The README says outside, but CMake accepts a path under `CMAKE_SOURCE_DIR`, which invites accidentally committing private sources. Reject paths under the source dir.
   - The option is sticky in the cache. Reconfiguring without the flag keeps it on. Print a `message(STATUS "Private effects: <dir>")` or `"Private effects: off"` line so CI logs show the public build is clean, and document `-USTALGIA_PRIVATE_EFFECTS_DIR`.
   - `EffectRegistry::addExternal` accepts duplicate or empty ids. A private effect registered as `matrix` would create a second gallery card while `find`/`create` always return the first. Log and skip duplicates.
8. **Ape/Myco fallback to Matrix is correct at startup (no functional bug).** `Application::initialize` maps an id that is not in the registry to `matrix` before `loadEffect`, and a running instance given `--wallpaper ape` logs and ignores it. Two leftovers:
   - The public source and exe still carry Ape/Myco-specific text and branches: `L"Code Ape settings"` / `L"Slow Myco settings"` in `SettingsPanel.cpp`, the `ape`/`myco` image-picker branch in `WallpaperGallery::pickImage`, `ApeSettings`/`MycoSettings` parse and serialize in `Config.cpp`, and comments in `RainReveal.*` and `Config.cpp`. They are kept so the private folder compiles against public headers. If the goal is no mention in the public repo, move the panel labels and image-path plumbing behind a small interface the private folder implements.
   - The load log line at `Config.cpp:421` still prints `tuneApe` and not the new tune values.
9. **Effect id is serialized unescaped (low, pre-existing).** `"effect": "<id>"` is written without escaping. A hand-edited id containing `"` produces invalid JSON on the next save, and the following launch moves the file to `.bad`.
