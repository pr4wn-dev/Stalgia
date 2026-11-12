#include "effects/RainReveal.h"

#include "core/Log.h"
#include "core/Util.h"
#include "effects/ImageMask.h"

#include <algorithm>
#include <cmath>

namespace {

uint32_t hashu(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float rand01(uint32_t& s) {
    s = hashu(s);
    return (s >> 8) * (1.0f / 16777215.0f);
}

float clampf(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

}  // namespace

RainReveal::RainReveal(const char* id, const char* displayName, CharacterMask mask)
    : m_id(id), m_name(displayName), m_mask(mask), m_active(mask) {}

bool RainReveal::initialize(ID3D11Device* device, ID3D11DeviceContext*) {
    m_device = device;
    m_atlasDirty = true;
    return true;
}

void RainReveal::release() {
    m_atlas.release();
    m_cols.clear();
    m_instances.clear();
    m_device = nullptr;
}

void RainReveal::bindMask() {
    if (m_customW > 1 && !m_customLuma.empty()) {
        m_active.luma = m_customLuma.data();
        m_active.rgb = m_customRgb.empty() ? nullptr : m_customRgb.data();
        m_active.w = m_customW;
        m_active.h = m_customH;
    } else {
        m_active = m_mask;
    }
}

void RainReveal::loadCustom(const std::string& path) {
    m_imagePath = path;
    m_customLuma.clear();
    m_customRgb.clear();
    m_customW = m_customH = 0;
    if (!path.empty()) {
        int w = 0, h = 0;
        if (!loadImageMask(widen(path), m_customLuma, m_customRgb, w, h)) {
            LOG_WARN("%s: custom image failed, using the built-in mask", m_name);
            m_customLuma.clear();
            m_customRgb.clear();
        } else {
            m_customW = w;
            m_customH = h;
        }
    }
    bindMask();
}

void RainReveal::applySettings(const AppSettings& settings) {
    RevealKnobs next = knobsFrom(settings);
    next.speed = clampf(next.speed, 0.1f, 3.0f);
    next.bgDim = clampf(next.bgDim, 0.35f, 1.20f);
    next.brightness = clampf(next.brightness, 0.4f, 2.5f);
    next.size = clampf(next.size, 0.50f, 0.95f);
    next.posX = clampf(next.posX, 0.25f, 0.75f);
    next.glyphSize = std::clamp(next.glyphSize, 7, 40);
    next.reveal = clampf(next.reveal, 0.2f, 2.2f);
    next.detail = clampf(next.detail, 0.0f, 1.0f);
    next.breatheAmt = clampf(next.breatheAmt, 0.0f, 2.0f);
    if (next.glyphSize != m_cfg.glyphSize) m_atlasDirty = true;
    if (next.imagePath != m_imagePath) loadCustom(next.imagePath);
    const bool structure = m_cols.empty() || next.glyphSize != m_cfg.glyphSize ||
                           std::fabs(next.bgDim - m_cfg.bgDim) > 0.03f;
    m_cfg = next;
    if (structure && m_width > 0 && m_height > 0) rebuildColumns();
}

void RainReveal::resize(int width, int height, float dpiScale) {
    m_width = width;
    m_height = height;
    m_dpi = dpiScale;
    rebuildColumns();
}

void RainReveal::rebuildColumns() {
    if (m_width <= 0 || m_height <= 0) return;
    const float resScale = std::max(0.90f, m_primaryHeight / 1200.0f);
    m_cell = clampf(m_cfg.glyphSize * resScale, 7.0f, 40.0f);
    const int atlasCell = std::max(8, (int)std::lround(m_cell));
    if (atlasCell != m_atlasCell) {
        m_atlasCell = atlasCell;
        m_atlasDirty = true;
    }
    m_rows = std::max(1, (int)std::ceil(m_height / m_cell) + 2);
    const int n = std::max(1, (int)std::ceil(m_width / m_cell));
    m_cols.assign((size_t)n, Column{});
    const float dualProb = clampf(0.18f + 0.32f * m_cfg.bgDim, 0.20f, 0.55f);
    for (int i = 0; i < n; ++i) {
        Column& c = m_cols[(size_t)i];
        c.seed = hashu((uint32_t)(i + 1) * 0x9E3779B9u ^ 0xC0DEA9E0u);
        uint32_t ds = c.seed ^ 0xA53C9E4Bu;
        c.n = (rand01(ds) < dualProb) ? 2 : 1;
        resetStream(c.s[0], c.seed, 0, true);
        if (c.n > 1) resetStream(c.s[1], c.seed, 1, true);
    }
}

float RainReveal::wrapDist(float head, float row) const {
    const float period = (float)std::max(1, m_rows);
    float dist = head - row;
    dist -= period * std::floor(dist / period);
    return dist;
}

void RainReveal::resetStream(Stream& s, uint32_t colSeed, uint32_t which, bool spawnOnScreen) {
    uint32_t st = colSeed ^ (s.generation * 0x85EBCA6Bu) ^ (which * 0x27D4EB2Du);
    s.base = 10.0f + rand01(st) * 16.0f;
    s.trail = (float)m_rows * (0.84f + rand01(st) * 0.13f);
    s.generation++;
    s.seed = hashu(st ^ 0xA5A5A5A5u);
    if (spawnOnScreen) {
        s.head = rand01(st) * (float)m_rows;
        if (which == 1) s.head = std::fmod(s.head + (float)m_rows * 0.47f, (float)m_rows);
        s.delay = 0.0f;
    } else {
        s.head = -0.5f;
        s.delay = 0.0f;
    }
}

float RainReveal::sampleLumaAt(float x, float y) const {
    if (!m_active.luma || m_active.w < 2 || m_active.h < 2) return 0.0f;
    const float apeH = m_cfg.size * (float)m_height;
    const float apeW = apeH * ((float)m_active.w / (float)m_active.h);
    const float cx = m_cfg.posX * (float)m_width;
    const float cy = (float)m_height * 0.50f;
    const float u = (x - (cx - apeW * 0.5f)) / apeW;
    const float v = (y - (cy - apeH * 0.5f)) / apeH;
    if (u <= 0.0f || v <= 0.0f || u >= 1.0f || v >= 1.0f) return 0.0f;

    const float fx = u * (float)(m_active.w - 1);
    const float fy = v * (float)(m_active.h - 1);
    const int x0 = (int)fx;
    const int y0 = (int)fy;
    const int x1 = std::min(x0 + 1, m_active.w - 1);
    const int y1 = std::min(y0 + 1, m_active.h - 1);
    const float tx = fx - (float)x0;
    const float ty = fy - (float)y0;
    auto pix = [&](int ix, int iy) {
        return m_active.luma[iy * m_active.w + ix] * (1.0f / 255.0f);
    };
    return lerp(lerp(pix(x0, y0), pix(x1, y0), tx), lerp(pix(x0, y1), pix(x1, y1), tx), ty);
}

float RainReveal::sampleLuma(float x, float y) const {
    const float c = sampleLumaAt(x, y);
    // Palette characters (Slow Myco): let thin dark seams win the glyph
    // cell so plates/face lines survive 9px rain, otherwise keep rims.
    if (!m_active.rgb || m_cell < 1.0f) return c;
    const float h = m_cell * 0.32f;
    float mn = c, mx = c;
    const float taps[4][2] = {{x - h, y}, {x + h, y}, {x, y - h}, {x, y + h}};
    for (const auto& t : taps) {
        const float s = sampleLumaAt(t[0], t[1]);
        mn = std::min(mn, s);
        mx = std::max(mx, s);
    }
    // Only punched-out seams (empty gaps) take the whole cell. Dim
    // features like the pupil stay local so the eye highlight survives.
    return (mn < 0.05f) ? mn : mx;
}

void RainReveal::samplePalette(float x, float y, float& r, float& g, float& b) const {
    r = g = b = 1.0f;
    if (!m_active.rgb || m_active.w < 2 || m_active.h < 2) return;
    const float apeH = m_cfg.size * (float)m_height;
    const float apeW = apeH * ((float)m_active.w / (float)m_active.h);
    const float cx = m_cfg.posX * (float)m_width;
    const float cy = (float)m_height * 0.50f;
    const float u = (x - (cx - apeW * 0.5f)) / apeW;
    const float v = (y - (cy - apeH * 0.5f)) / apeH;
    if (u <= 0.0f || v <= 0.0f || u >= 1.0f || v >= 1.0f) return;

    const float fx = u * (float)(m_active.w - 1);
    const float fy = v * (float)(m_active.h - 1);
    const int x0 = (int)fx;
    const int y0 = (int)fy;
    const int x1 = std::min(x0 + 1, m_active.w - 1);
    const int y1 = std::min(y0 + 1, m_active.h - 1);
    const float tx = fx - (float)x0;
    const float ty = fy - (float)y0;
    auto chan = [&](int ix, int iy, int c) {
        return m_active.rgb[(iy * m_active.w + ix) * 3 + c] * (1.0f / 255.0f);
    };
    auto bilerp = [&](int c) {
        return lerp(lerp(chan(x0, y0, c), chan(x1, y0, c), tx), lerp(chan(x0, y1, c), chan(x1, y1, c), tx), ty);
    };
    r = bilerp(0);
    g = bilerp(1);
    b = bilerp(2);
}

float RainReveal::trailBri(float dist, float trail, bool inside) {
    if (inside) {
        if (dist < 0.35f) return 1.0f;
        if (dist < 8.0f) return lerp(0.82f, 0.68f, clampf((dist - 0.35f) / 7.65f, 0.0f, 1.0f));
        const float tail = clampf((dist - 8.0f) / std::max(1.0f, trail - 8.0f), 0.0f, 1.0f);
        return 0.68f * std::pow(1.0f - tail, 0.72f);
    }
    if (dist < 0.35f) return 1.0f;
    if (dist < 5.5f) return lerp(0.70f, 0.60f, clampf((dist - 0.35f) / 5.15f, 0.0f, 1.0f));
    const float tail = clampf((dist - 5.5f) / std::max(1.0f, trail - 5.5f), 0.0f, 1.0f);
    return 0.60f * std::pow(1.0f - tail, 1.05f);
}

float RainReveal::shadeLuma(float luma) {
    float x = clampf(luma, 0.0f, 1.0f);
    x = std::pow(x, 0.62f);
    x = clampf((x - 0.06f) / 0.86f, 0.0f, 1.0f);
    x = clampf((x - 0.38f) * 1.50f + 0.38f, 0.0f, 1.0f);
    return x;
}

void RainReveal::update(float dt, double time) {
    if (dt < 0) dt = 0;
    if (dt > 0.05f) dt = 0.05f;
    m_time = time;
    if (m_cols.empty()) return;

    const float period = (float)std::max(1, m_rows);
    for (int i = 0; i < (int)m_cols.size(); ++i) {
        Column& c = m_cols[(size_t)i];
        for (int si = 0; si < c.n; ++si) {
            Stream& s = c.s[si];
            if (s.delay > 0) {
                s.delay -= dt;
                continue;
            }
            s.head += s.base * m_cfg.speed * dt;
            if (s.head >= period) s.head -= period * std::floor(s.head / period);
        }
    }
}

void RainReveal::precompile(Renderer*) {
    if (!m_device || !m_atlasDirty) return;
    const int cell = m_atlasCell > 0 ? m_atlasCell : (m_cell > 1.0f ? (int)m_cell : 14);
    if (m_atlas.build(m_device, cell)) m_atlasDirty = false;
}

void RainReveal::render(const FrameContext& ctx) {
    if (!m_device) m_device = ctx.device;
    m_primaryHeight = ctx.primaryHeight > 0 ? ctx.primaryHeight : ctx.height;
    if (ctx.width != m_width || ctx.height != m_height) {
        resize(ctx.width, ctx.height, ctx.dpiScale);
    }
    if (m_atlasDirty && m_device) {
        if (!m_atlas.build(m_device, m_atlasCell > 0 ? m_atlasCell : (int)m_cell)) {
            LOG_ERR("%s: glyph atlas failed", m_name);
            return;
        }
        m_atlasDirty = false;
    }
    if (!m_atlas.srv()) return;

    float cr, cg, cb;
    rgbFromHex(m_cfg.color, cr, cg, cb);
    const float hr = clampf(cr * 0.15f + 0.85f, 0.0f, 1.0f);
    const float hg = clampf(cg * 0.15f + 0.85f, 0.0f, 1.0f);
    const float hb = clampf(cb * 0.15f + 0.85f, 0.0f, 1.0f);

    const int nGlyphs = std::max(1, m_atlas.glyphCount());
    m_instances.clear();
    m_instances.reserve((size_t)m_cols.size() * (size_t)(m_rows + 3));

    const uint32_t bodySalt = (uint32_t)std::llround(m_time * 0.22);
    const uint32_t headSalt = (uint32_t)std::llround(m_time * 0.70);
    const float twoPi = 6.2831853f;
    const float breathe = m_cfg.breathing
                              ? (1.0f + 0.045f * m_cfg.breatheAmt * std::sin((float)(m_time * twoPi / 5.0) * m_cfg.reveal))
                              : 1.0f;
    const float rainScale = clampf(m_cfg.bgDim, 0.35f, 1.20f);
    const float charScale = 0.72f + 0.18f * m_cfg.brightness;
    const float outsideDim = 0.70f;
    const bool colorize = m_cfg.colorize && m_active.rgb;

    const int rowBegin = -1;
    const int rowEnd = m_rows;
    for (int i = 0; i < (int)m_cols.size(); ++i) {
        const Column& c = m_cols[(size_t)i];
        const float x = (float)i * m_cell + ctx.lookX * m_cell * 5.0f;
        const float cx = x + m_cell * 0.5f;

        for (int row = rowBegin; row <= rowEnd; ++row) {
            const float y = (float)row * m_cell;
            if (y + m_cell < 0.0f || y > (float)m_height) continue;
            const float cy = y + m_cell * 0.5f;
            float luma = sampleLuma(cx, cy);
            if (std::fabs(m_cfg.detail - 0.5f) > 0.02f) {
                const float contrast = lerp(0.65f, 2.2f, m_cfg.detail);
                luma = clampf((luma - 0.45f) * contrast + 0.45f, 0.0f, 1.0f);
            }
            const bool inside = luma > 0.02f;
            const float revealScale = inside ? lerp(1.28f, 0.72f, clampf(m_cfg.reveal / 2.2f, 0.0f, 1.0f)) : 1.0f;

            float rainFade = 0.0f;
            int kHead = 999;
            uint32_t streamSeed = c.s[0].seed;
            uint32_t streamGen = c.s[0].generation;
            bool anyTrail = false;

            for (int si = 0; si < c.n; ++si) {
                const Stream& s = c.s[si];
                if (s.delay > 0) continue;
                const float dist = wrapDist(s.head, (float)row) * revealScale;
                const int trailCells = std::max(8, (int)std::ceil(s.trail));
                if (dist > (float)trailCells + 0.5f) continue;
                anyTrail = true;
                const float bri = trailBri(dist, s.trail, inside);
                if (bri >= rainFade) {
                    rainFade = bri;
                    kHead = (int)std::lround(std::max(0.0f, dist));
                    streamSeed = s.seed;
                    streamGen = s.generation;
                }
            }
            if (!anyTrail) continue;

            float bri = 0.0f;
            bool glowingHead = kHead == 0;
            const float shade = inside ? shadeLuma(luma) : 0.0f;
            if (inside) {
                const float mod = lerp(0.32f, 1.28f, shade) * charScale * breathe;
                if (glowingHead) {
                    bri = lerp(0.78f, 1.0f, shade);
                    if (m_cfg.eyeGlow && luma > 0.72f) bri = 1.0f;
                } else {
                    bri = rainFade * mod;
                }
            } else {
                bri = glowingHead ? 1.0f : rainFade * rainScale * outsideDim;
            }
            if (bri < 0.018f) continue;

            uint32_t h = hashu(streamSeed ^ (uint32_t)row * 0x9E3779B9u ^ (streamGen * 7u));
            const uint32_t salt = glowingHead ? headSalt : bodySalt;
            if ((hashu(h ^ salt) & 0xFFFF) < 2200) h ^= salt * 0x45D9F3Bu;
            const int gi = (int)(h % (uint32_t)nGlyphs);

            GlyphInstance g{};
            g.rect[0] = x;
            g.rect[1] = y;
            g.rect[2] = m_cell;
            g.rect[3] = m_cell;
            m_atlas.uvFor(gi, g.uv[0], g.uv[1], g.uv[2], g.uv[3]);

            float pr = cr, pg = cg, pb = cb;
            float phr = hr, phg = hg, phb = hb;
            if (colorize && inside) {
                samplePalette(cx, cy, pr, pg, pb);
                // Keep the palette's relative value (plate centers lighter
                // than seams). Only lift the floor so tinted rain still
                // reads against silver outside. No per-glyph hue jitter.
                const float mx = std::max(pr, std::max(pg, pb));
                if (mx > 0.02f) {
                    const float vTarget = lerp(0.46f, 1.0f, shade);
                    const float scale = vTarget / mx;
                    pr = clampf(pr * scale, 0.0f, 1.0f);
                    pg = clampf(pg * scale, 0.0f, 1.0f);
                    pb = clampf(pb * scale, 0.0f, 1.0f);
                } else {
                    pr = pg = pb = 1.0f;
                }
                const float wash = 0.08f;
                pr = pr * (1.0f - wash) + wash;
                pg = pg * (1.0f - wash) + wash;
                pb = pb * (1.0f - wash) + wash;
                phr = clampf(pr * 0.12f + 0.88f, 0.0f, 1.0f);
                phg = clampf(pg * 0.12f + 0.88f, 0.0f, 1.0f);
                phb = clampf(pb * 0.12f + 0.88f, 0.0f, 1.0f);
            }

            if (glowingHead && (!inside || shade > 0.35f)) {
                g.color[0] = phr;
                g.color[1] = phg;
                g.color[2] = phb;
                g.color[3] = 1.0f;
            } else {
                const float k = clampf(bri, 0.0f, 1.0f);
                g.color[0] = pr * k;
                g.color[1] = pg * k;
                g.color[2] = pb * k;
                g.color[3] = clampf(bri * 1.08f, 0.0f, 1.0f);
            }
            m_instances.push_back(g);
        }
    }

    if (ctx.renderer) {
        ctx.renderer->clear(0.0f, 0.0f, 0.0f, 1.0f);
        ctx.renderer->drawGlyphs(m_atlas.srv(), m_instances.data(), (int)m_instances.size());
    }
}
