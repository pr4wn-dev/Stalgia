#include "effects/Fireflies.h"

#include "core/Util.h"
#include "gfx/Shaders.h"

#include <cmath>

namespace {
double wrapMod(double t, double m) {
    double w = std::fmod(t, m);
    if (w < 0) w += m;
    return w;
}
}

bool Fireflies::initialize(ID3D11Device*, ID3D11DeviceContext*) {
    return true;
}

void Fireflies::release() {
    m_ps.reset();
    m_ready = false;
    m_failed = false;
}

void Fireflies::resize(int, int, float) {}

void Fireflies::applySettings(const AppSettings& settings) {
    m_cfg = settings.fireflies;
}

void Fireflies::update(float, double time) {
    m_time = time;
}

int Fireflies::ensure(Renderer* renderer, int blockMs) {
    if (m_ready && m_ps) return 1;
    if (m_failed || !renderer) return -1;
    ID3D11PixelShader* ps = nullptr;
    const int st = renderer->compilePixelShader(kFullscreenHLSL, sizeof(kFullscreenHLSL), "fullscreen.hlsl",
                                                "PSFireflies", &ps, blockMs);
    if (st == 1 && ps) {
        m_ps = ComPtr<ID3D11PixelShader>(ps);
        m_ready = true;
        return 1;
    }
    if (st < 0) m_failed = true;
    return st;
}

void Fireflies::precompile(Renderer* renderer) {
    ensure(renderer, 0);
}

void Fireflies::render(const FrameContext& ctx) {
    if (!ctx.renderer || !ctx.device) return;
    ensure(ctx.renderer, 0);
    ID3D11PixelShader* ps = m_ready ? m_ps.get() : ctx.renderer->fallbackShader();
    if (!ps) return;

    Renderer::FullscreenCB cb{};
    cb.time = (float)wrapMod(m_time, 3600.0);
    cb.width = (float)ctx.width;
    cb.height = (float)ctx.height;
    cb.speed = m_cfg.speed;
    rgbFromHex(m_cfg.color, cb.colorA[0], cb.colorA[1], cb.colorA[2]);
    cb.colorA[3] = 1.0f;
    cb.density = m_cfg.density;
    cb.p0 = m_cfg.mist;
    cb.p1 = m_cfg.stars;
    cb.p2 = m_cfg.woods;
    cb.direction = (float)m_cfg.style;
    cb.p3 = m_mask ? -1.0f : 0.0f;
    cb.lookX = ctx.lookX;
    cb.lookY = ctx.lookY;
    cb.quality = ctx.renderer->isWarp() ? 0.0f : 1.0f;
    ctx.renderer->drawFullscreen(ps, cb);
}
