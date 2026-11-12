#include "effects/MatrixRain.h"

#include "core/Log.h"
#include "core/Util.h"

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

}  // namespace

bool MatrixRain::initialize(ID3D11Device* device, ID3D11DeviceContext*) {
    m_device = device;
    m_atlasDirty = true;
    return true;
}

void MatrixRain::release() {
    m_atlas.release();
    m_cols.clear();
    m_instances.clear();
    m_device = nullptr;
}

void MatrixRain::applySettings(const AppSettings& settings) {
    const MatrixSettings next = settings.matrix;
    const bool structure = m_cols.empty() || next.glyphSize != m_cfg.glyphSize || next.glyphSet != m_cfg.glyphSet ||
                           next.customGlyphs != m_cfg.customGlyphs || std::fabs(next.density - m_cfg.density) > 0.001f ||
                           std::fabs(next.region - m_cfg.region) > 0.001f;
    if (next.glyphSize != m_cfg.glyphSize || next.glyphSet != m_cfg.glyphSet ||
        next.customGlyphs != m_cfg.customGlyphs) {
        m_atlasDirty = true;
    }
    m_cfg = next;
    if (structure && m_width > 0 && m_height > 0) rebuildColumns();
}

void MatrixRain::resize(int width, int height, float dpiScale) {
    m_width = width;
    m_height = height;
    m_dpi = dpiScale;
    rebuildColumns();
}

void MatrixRain::rebuildColumns() {
    if (m_width <= 0 || m_height <= 0) return;

    const float resScale = std::max(0.75f, m_primaryHeight / 1080.0f);
    m_cell = std::clamp(m_cfg.glyphSize * resScale, 12.0f, 56.0f);
    const int atlasCell = std::max(12, (int)std::lround(m_cell));
    if (atlasCell != m_atlasCell) {
        m_atlasCell = atlasCell;
        m_atlasDirty = true;
    }

    m_rows = std::max(1, (int)std::ceil(m_height / m_cell) + 2);
    const int n = std::max(1, (int)std::ceil(m_width / m_cell));
    m_cols.assign((size_t)n, Column{});
    const float dens = std::clamp(m_cfg.density, 0.1f, 2.0f);
    const float region = std::clamp(m_cfg.region, 0.0f, 1.0f);
    for (int i = 0; i < n; ++i) {
        Column& c = m_cols[(size_t)i];
        c.seed = hashu((uint32_t)(i + 1) * 0x9E3779B9u);
        const float x = n > 1 ? (float)i / (float)(n - 1) : 0.5f;
        const float center = 1.0f - std::fabs(x - 0.5f) * 2.0f;
        const float bias = (1.0f - region) + region * (0.35f + 0.9f * center);
        const float p = std::clamp(dens * bias, 0.05f, 1.0f);
        c.active = rand01(c.seed) < p;
        resetColumn(c, true);
    }
    m_builtGlyph = m_cfg.glyphSet;
    m_builtCustom = m_cfg.customGlyphs;
    m_builtDensity = m_cfg.density;
    m_builtRegion = m_cfg.region;
}

void MatrixRain::resetColumn(Column& c, bool spawnOnScreen) {
    uint32_t s = c.seed ^ (c.generation * 0x85EBCA6Bu);
        c.base = 8.0f + rand01(s) * 22.0f;
        c.layer = 0.35f + rand01(s) * 0.65f;
        c.trail = m_cfg.trailLength * (0.55f + rand01(s) * 0.9f);
    c.generation++;
    c.seed = hashu(s ^ 0xA5A5A5A5u);
    if (spawnOnScreen) {
        c.head = rand01(s) * (float)m_rows;
        c.delay = rand01(s) * 2.5f;
    } else if (m_cfg.direction < 0.0f) {
        c.head = (float)m_rows + rand01(s) * 8.0f;
        c.delay = rand01(s) * 1.8f;
    } else {
        c.head = -rand01(s) * 8.0f;
        c.delay = rand01(s) * 1.8f;
    }
}

void MatrixRain::update(float dt, double time) {
    m_time = time;
    if (m_cols.empty()) return;
    const float dir = std::clamp(m_cfg.direction, -1.0f, 1.0f);
    const float dirSafe = std::fabs(dir) < 0.08f ? (dir < 0.0f ? -0.08f : 0.08f) : dir;
    for (int i = 0; i < (int)m_cols.size(); ++i) {
        Column& c = m_cols[(size_t)i];
        if (!c.active) continue;
        if (c.glitch > 0) c.glitch -= dt;
        else if (m_cfg.glitch > 0.01f) {
            uint32_t g = hashu(c.seed ^ (uint32_t)(m_time * 8.0) ^ (uint32_t)i * 17u);
            if ((g & 0xFFFF) / 65535.0f < m_cfg.glitch * dt * 0.55f) c.glitch = 0.06f + ((g >> 16) & 255) / 255.0f * 0.12f;
        }
        if (c.delay > 0) {
            c.delay -= dt;
            continue;
        }
        const float layerMix = 1.0f - m_cfg.depth + m_cfg.depth * c.layer;
        c.head += c.base * m_cfg.speed * (0.45f + 0.55f * layerMix) * dirSafe * dt;
        const bool off = dirSafe >= 0 ? (c.head - c.trail > m_rows + 2) : (c.head + c.trail < -2);
        if (off) resetColumn(c, false);
    }
}

void MatrixRain::precompile(Renderer*) {
    if (!m_device || !m_atlasDirty) return;
    const int cell = m_atlasCell > 0 ? m_atlasCell : (m_cell > 1.0f ? (int)m_cell : 18);
    if (m_atlas.build(m_device, cell, m_cfg.glyphSet, widen(m_cfg.customGlyphs))) m_atlasDirty = false;
}

void MatrixRain::render(const FrameContext& ctx) {
    if (!m_device) m_device = ctx.device;
    m_primaryHeight = ctx.primaryHeight > 0 ? ctx.primaryHeight : ctx.height;

    if (ctx.width != m_width || ctx.height != m_height) {
        resize(ctx.width, ctx.height, ctx.dpiScale);
    }

    if (m_atlasDirty && m_device) {
        if (!m_atlas.build(m_device, m_atlasCell > 0 ? m_atlasCell : (int)m_cell, m_cfg.glyphSet,
                           widen(m_cfg.customGlyphs))) {
            LOG_ERR("Matrix rain: glyph atlas failed");
            return;
        }
        m_atlasDirty = false;
    }
    if (!m_atlas.srv()) return;

    float br, bg, bb;
    float hr, hg, hb;
    rgbFromHex(m_cfg.color, br, bg, bb);
    rgbFromHex(m_cfg.headColor, hr, hg, hb);

    const int nGlyphs = std::max(1, m_atlas.glyphCount());
    m_instances.clear();
    m_instances.reserve((size_t)m_cols.size() * (size_t)std::min(m_rows, (int)m_cfg.trailLength + 8));

    const float fadePow = std::max(0.4f, m_cfg.fadePower);
    const uint32_t timeSalt = (uint32_t)std::llround(m_time * 8.0);
    const float dirSign = m_cfg.direction >= 0.0f ? 1.0f : -1.0f;

    for (int i = 0; i < (int)m_cols.size(); ++i) {
        const Column& c = m_cols[(size_t)i];
        if (!c.active || c.delay > 0) continue;

        const float layerMix = 1.0f - std::clamp(m_cfg.depth, 0.0f, 1.0f) + std::clamp(m_cfg.depth, 0.0f, 1.0f) * c.layer;
        const float draw = m_cell * (0.62f + 0.38f * layerMix);
        const float dim = 0.38f + 0.62f * layerMix;
        const float wind = std::sin((float)m_time * 0.55f + (float)i * 0.37f) * m_cfg.wind * m_cell * 0.9f;
        const float parallax = ctx.lookX * (1.15f - layerMix) * m_cell * 10.0f;
        float x = (float)i * m_cell + (m_cell - draw) * 0.5f + wind + parallax;
        if (c.glitch > 0) x += (float)((int)(hashu(c.seed ^ timeSalt) & 7) - 3);
        const int trailCells = std::max(4, (int)std::ceil(c.trail));
        const int headCell = (int)std::floor(c.head);

        for (int k = 0; k <= trailCells; ++k) {
            const int row = headCell - (int)(dirSign * (float)k);
            if (row < -1 || row > m_rows) continue;
            const float y = (float)row * m_cell + (m_cell - draw) * 0.5f;
            if (y + draw < 0 || y > m_height) continue;

            const float dist = std::fabs(c.head - (float)row);
            const float t = std::clamp(dist / std::max(1.0f, c.trail), 0.0f, 1.0f);

            uint32_t h = hashu(c.seed ^ (uint32_t)row * 0x9E3779B9u ^ (c.generation * 7u));
            const float mutate = (k == 0) ? std::max(m_cfg.mutate, 0.25f) : m_cfg.mutate;
            if (mutate > 0) {
                const uint32_t flicker = hashu(h ^ timeSalt ^ (uint32_t)k);
                if ((flicker & 0xFFFF) / 65535.0f < mutate) {
                    h ^= flicker;
                }
            }
            const int gi = (int)(h % (uint32_t)nGlyphs);

            GlyphInstance g{};
            g.rect[0] = x;
            g.rect[1] = y;
            g.rect[2] = draw;
            g.rect[3] = draw;
            m_atlas.uvFor(gi, g.uv[0], g.uv[1], g.uv[2], g.uv[3]);

            if (k == 0) {
                if (m_cfg.bloom > 0.02f) {
                    const float o = draw * (0.10f + 0.16f * m_cfg.bloom);
                    const float offs[4][2] = {{o, 0}, {-o, 0}, {0, o}, {0, -o}};
                    for (int b = 0; b < 4; ++b) {
                        GlyphInstance glow = g;
                        glow.rect[0] = x + offs[b][0];
                        glow.rect[1] = y + offs[b][1];
                        glow.color[0] = hr;
                        glow.color[1] = hg;
                        glow.color[2] = hb;
                        glow.color[3] = std::clamp(0.10f * m_cfg.bloom * dim, 0.0f, 0.28f);
                        m_instances.push_back(glow);
                    }
                }
                g.color[0] = hr * dim;
                g.color[1] = hg * dim;
                g.color[2] = hb * dim;
                g.color[3] = 1.0f;
            } else {
                const float fade = std::pow(1.0f - t, fadePow);
                const float nearHead = (k < 3) ? 0.55f + 0.45f * fade : fade;
                const float flick = (c.glitch > 0 && (hashu(h ^ timeSalt) & 3) == 0) ? 0.35f : 1.0f;
                g.color[0] = br * nearHead * dim * flick;
                g.color[1] = bg * nearHead * dim * flick;
                g.color[2] = bb * nearHead * dim * flick;
                g.color[3] = std::clamp(nearHead * 1.15f * dim, 0.0f, 1.0f);
            }
            m_instances.push_back(g);
        }
    }

    if (ctx.renderer) {
        ctx.renderer->clear(0.0f, 0.0f, 0.0f, 1.0f);
        ctx.renderer->drawGlyphs(m_atlas.srv(), m_instances.data(), (int)m_instances.size());
    }
}
