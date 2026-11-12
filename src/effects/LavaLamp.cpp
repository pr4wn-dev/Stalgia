#include "effects/LavaLamp.h"

#include "core/Log.h"
#include "core/Util.h"
#include "gfx/Shaders.h"

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

float frac(float v) {
    return v - std::floor(v);
}

}  // namespace

bool LavaLamp::initialize(ID3D11Device*, ID3D11DeviceContext*) {
    if (!m_inited) {
        m_inited = true;
        syncCount();
    }
    return true;
}

void LavaLamp::release() {
    m_ps.reset();
    m_ready = false;
    m_failed = false;
}

void LavaLamp::resize(int width, int height, float) {
    m_width = std::max(1, width);
    m_height = std::max(1, height);
}

void LavaLamp::applySettings(const AppSettings& settings) {
    m_cfg = settings.lava;
    m_cfg.blobCount = std::clamp(m_cfg.blobCount, 8, kMaxBlobs);
    m_cfg.speed = clampf(m_cfg.speed, 0.1f, 2.5f);
    m_cfg.glow = clampf(m_cfg.glow, 0.2f, 3.0f);
    m_cfg.blobSize = clampf(m_cfg.blobSize, 0.2f, 2.0f);
    m_cfg.viscosity = clampf(m_cfg.viscosity, 0.35f, 2.2f);
    m_cfg.heat = clampf(m_cfg.heat, 0.0f, 1.0f);
    m_cfg.gradient = clampf(m_cfg.gradient, 0.0f, 1.0f);
    m_cfg.lamp = clampf(m_cfg.lamp, 0.0f, 1.0f);
    syncCount();
}

float LavaLamp::gooey() const {
    // Viscosity 1 matches the old speed=1 motion. Higher is thicker.
    const float v = clampf(m_cfg.viscosity, 0.35f, 2.2f);
    return clampf(1.35f - 0.45f * v, 0.35f, 1.25f);
}

void LavaLamp::spawn(Blob& b, int index) {
    uint32_t s = hashu((uint32_t)(index + 1) * 0x9E3779B9u ^ 0xC2B2AE35u);
    b.seed = s;
    b.phase = rand01(s) * 6.2831853f;
    b.vx = 0;
    b.vy = 0;
    b.timer = 0;
    const float size = m_cfg.blobSize;

    if (index == 0) {
        b.state = kPool;
        b.x = 0.50f;
        b.r = 0.165f * size;
        b.y = b.r * 0.40f;
        b.temp = 0.95f;
        return;
    }
    if (index == 1) {
        // Already stretching out of the pool so the first seconds show a neck, not a boil.
        b.state = kForming;
        b.x = 0.50f + 0.03f * std::sin(b.phase);
        b.r = 0.095f * size;
        b.y = 0.14f;
        b.temp = 0.90f;
        b.vy = 0.010f;
        return;
    }

    const float u = rand01(s);
    if (index >= 6 && u < 0.40f) {
        b.state = kQueued;
        b.x = 0.50f;
        b.y = 0.0f;
        b.r = 0.0f;
        b.temp = 0.70f;
        b.timer = 5.0f + rand01(s) * 14.0f;
        return;
    }

    b.state = kFree;
    b.x = 0.18f + rand01(s) * 0.64f;
    b.y = 0.28f + rand01(s) * 0.58f;
    b.r = (0.058f + rand01(s) * 0.034f) * size;
    b.temp = 0.30f + rand01(s) * 0.42f;
    b.vy = (b.temp - 0.42f) * 0.08f;
}

void LavaLamp::syncCount() {
    const int n = std::clamp(m_cfg.blobCount, 8, kMaxBlobs);
    if ((int)m_blobs.size() < n) {
        const int old = (int)m_blobs.size();
        m_blobs.resize((size_t)n);
        for (int i = old; i < n; ++i) {
            if (old == 0) {
                spawn(m_blobs[(size_t)i], i);
            } else {
                spawn(m_blobs[(size_t)i], i);
                if (i > 1) {
                    m_blobs[(size_t)i].state = kQueued;
                    m_blobs[(size_t)i].r = 0;
                    m_blobs[(size_t)i].timer = 4.0f + 3.0f * (float)(i - old);
                }
            }
        }
    } else if ((int)m_blobs.size() > n) {
        m_blobs.resize((size_t)n);
        if (!m_blobs.empty()) m_blobs[0].state = kPool;
    }
    if (m_blobs.empty()) {
        m_blobs.resize((size_t)n);
        for (int i = 0; i < n; ++i) spawn(m_blobs[(size_t)i], i);
    }
}

void LavaLamp::step(float dt) {
    const float g = gooey();
    const float size = m_cfg.blobSize;
    const int n = (int)m_blobs.size();
    if (n <= 0) return;
    const float twoPi = 6.2831853f;
    const float minR = 0.050f * size;
    const float maxR = 0.135f * size;
    const float poolTarget = 0.168f * size;

    Blob& pool = m_blobs[0];
    pool.state = kPool;
    pool.temp += (0.96f - pool.temp) * 0.07f * g * dt;
    pool.temp = clampf(pool.temp, 0.78f, 1.0f);
    if (pool.r < poolTarget) pool.r += (poolTarget - pool.r) * 0.035f * g * dt;
    pool.r = clampf(pool.r, 0.110f * size, 0.210f * size);
    // Mound sits on the floor: the circle is half off-screen, so the iso is a cap.
    pool.y = pool.r * 0.40f;
    pool.vy = 0;
    // Very slow lateral wander (~0.06 Hz), small amplitude — not a jiggle.
    pool.vx = std::sin((float)m_simTime * twoPi * 0.06f + pool.phase) * 0.006f * g;
    pool.x += pool.vx * dt;
    pool.x = clampf(pool.x, 0.28f, 0.72f);

    int forming = 0;
    int merging = 0;
    for (int i = 1; i < n; ++i) {
        if (m_blobs[(size_t)i].state == kForming) forming++;
        if (m_blobs[(size_t)i].state == kFree && m_blobs[(size_t)i].y < pool.y + pool.r * 0.85f &&
            m_blobs[(size_t)i].vy <= 0.004f)
            merging++;
    }
    int mergeLeft = merging > 0 ? 1 : 0;

    for (int i = 1; i < n; ++i) {
        Blob& b = m_blobs[(size_t)i];
        if (b.state == kQueued) {
            b.r = 0;
            b.vx = 0;
            b.vy = 0;
            b.x = pool.x;
            b.y = pool.y;
            b.timer -= dt;
            // One pinch-off at a time. Wait until the pool is a real mound.
            if (forming == 0 && b.timer <= 0.0f && pool.r > 0.125f * size) {
                b.state = kForming;
                b.x = pool.x + 0.035f * std::sin(b.phase);
                b.r = pool.r * 0.52f;
                b.y = pool.y + pool.r * 0.22f;
                b.temp = 0.88f;
                b.vx = 0;
                b.vy = 0.006f * g;
                pool.r = std::max(0.115f * size, pool.r - b.r * 0.38f);
                forming++;
            }
            continue;
        }

        if (b.state == kForming) {
            const float targetR = (0.072f + 0.028f * b.temp) * size;
            b.r += (targetR - b.r) * 0.055f * g * dt;
            b.r = clampf(b.r, minR, maxR);
            b.temp += (1.0f - b.temp) * 0.08f * g * dt;
            // Stretch upward over ~8–14 s into a neck, then pinch.
            b.vy += (0.016f * g - b.vy) * 0.9f * dt;
            b.y += b.vy * dt;
            const float lean = pool.x + 0.045f * std::sin(b.phase);
            b.x += (lean - b.x) * 0.35f * dt;
            b.vx *= 0.90f;
            const float sep = b.y - pool.y;
            const float pinchAt = (pool.r + b.r) * 0.90f;
            if (sep > pinchAt && b.r > minR) {
                b.state = kFree;
                b.vy = 0.014f * g;
            }
            continue;
        }

        // Free-floating glob. No radius pulsing, no tiny leftovers.
        b.temp += (0.26f - b.temp) * (0.045f + 0.070f * b.y) * g * dt;
        b.temp = clampf(b.temp, 0.22f, 1.0f);
        const bool joining = (b.y < pool.y + pool.r * 0.70f && b.vy <= 0.004f);
        if (!joining) b.r = clampf(b.r, minR, maxR);
        else b.r = std::min(b.r, maxR);

        float targetVy = (b.temp - 0.43f) * 0.095f * g;
        if (b.y > 0.80f) {
            b.temp += (0.22f - b.temp) * 0.09f * g * dt;
            if (b.temp < 0.38f) targetVy = std::min(targetVy, -0.025f * g);
            if (b.y > 0.93f) {
                b.y = 0.93f;
                if (targetVy > 0) targetVy = 0;
            }
        }
        b.vy += (targetVy - b.vy) * 1.05f * dt;

        // ~0.08 Hz, a few pixels of wander. Visibly smooth at 30 fps.
        const float hz = 0.07f + 0.03f * frac(b.phase * 0.159f);
        const float wob = std::sin((float)m_simTime * twoPi * hz + b.phase) * 0.0030f * g;
        b.vx += (wob - b.vx) * 0.65f * dt;

        b.x += b.vx * dt;
        b.y += b.vy * dt;
        if (m_cfg.lamp > 0.35f) b.x += (0.50f - b.x) * (0.35f + 0.8f * m_cfg.lamp) * dt;

        if (b.x < 0.10f) {
            b.x = 0.10f;
            b.vx = std::abs(b.vx) * 0.2f;
        }
        if (b.x > 0.90f) {
            b.x = 0.90f;
            b.vx = -std::abs(b.vx) * 0.2f;
        }

        // Rejoin the pool as a merge, never as a new bubble. One at a time.
        if (mergeLeft > 0 && joining) {
            mergeLeft--;
            const float xfer = std::min(b.r, 0.045f * size * dt);
            b.r -= xfer;
            pool.r = std::min(0.210f * size, pool.r + xfer * 0.88f);
            b.x += (pool.x - b.x) * 1.4f * dt;
            b.y += (pool.y - b.y) * 1.2f * dt;
            if (b.r < minR * 0.55f) {
                b.state = kQueued;
                b.r = 0;
                b.timer = 6.0f + 10.0f * frac(b.phase * 0.37f);
                b.x = pool.x;
                b.y = pool.y;
                b.vx = 0;
                b.vy = 0;
            }
        }
    }

}

void LavaLamp::update(float dt, double) {
    if (dt < 0) dt = 0;
    if (dt > 0.05f) dt = 0.05f;
    syncCount();
    m_accum += dt * clampf(m_cfg.speed, 0.1f, 2.5f);
    if (m_accum > 0.10f) m_accum = 0.10f;
    const float ht = 1.0f / 60.0f;
    while (m_accum >= ht) {
        step(ht);
        m_simTime += ht;
        m_accum -= ht;
    }
}

int LavaLamp::ensure(Renderer* renderer, int blockMs) {
    if (m_ready && m_ps) return 1;
    if (m_failed || !renderer) return -1;
    ID3D11PixelShader* ps = nullptr;
    const int st = renderer->compilePixelShader(kLavaHLSL, sizeof(kLavaHLSL), "lava.hlsl", "PSLava", &ps, blockMs);
    if (st == 1 && ps) {
        m_ps = ComPtr<ID3D11PixelShader>(ps);
        m_ready = true;
        return 1;
    }
    if (st < 0) {
        m_failed = true;
        LOG_ERR("Lava lamp pixel shader failed");
    }
    return st;
}

void LavaLamp::precompile(Renderer* renderer) {
    ensure(renderer, 0);
}

void LavaLamp::render(const FrameContext& ctx) {
    if (!ctx.renderer || !ctx.device) return;
    ensure(ctx.renderer, 0);
    ID3D11PixelShader* ps = m_ready ? m_ps.get() : ctx.renderer->fallbackShader();
    if (!ps) return;

    LavaCB cb{};
    double wrapped = std::fmod(m_simTime, 3600.0);
    if (wrapped < 0) wrapped += 3600.0;
    cb.time = (float)wrapped;
    cb.width = (float)ctx.width;
    cb.height = (float)ctx.height;
    cb.speed = m_cfg.speed;
    rgbFromHex(m_cfg.lavaColor, cb.lava[0], cb.lava[1], cb.lava[2]);
    cb.lava[3] = 1.0f;
    rgbFromHex(m_cfg.liquidColor, cb.liquid[0], cb.liquid[1], cb.liquid[2]);
    cb.liquid[3] = 1.0f;
    cb.glow = m_cfg.glow;
    cb.blobScale = 1.0f;
    cb.count = (float)m_blobs.size();
    cb.threshold = 0.28f;
    cb.heat = m_cfg.heat;
    cb.gradient = m_cfg.gradient;
    cb.lamp = m_cfg.lamp;
    cb.specGain = 1.0f;
    cb.lookX = ctx.lookX;
    cb.lookY = ctx.lookY;
    for (int i = 0; i < kMaxBlobs; ++i) {
        if (i < (int)m_blobs.size()) {
            const Blob& b = m_blobs[(size_t)i];
            const bool vis = b.state != kQueued && b.r > 0.008f;
            cb.blobs[i][0] = vis ? b.x : -1.0f;
            cb.blobs[i][1] = vis ? (1.0f - b.y) : -1.0f;
            cb.blobs[i][2] = vis ? b.r : 0.0f;
            cb.blobs[i][3] = vis ? b.temp : 0.0f;
        }
    }
    ctx.renderer->drawFullscreen(ps, &cb, sizeof(cb));
}
