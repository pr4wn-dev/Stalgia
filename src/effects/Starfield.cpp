#include "effects/Starfield.h"

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

bool Starfield::initialize(ID3D11Device*, ID3D11DeviceContext*) {
    return true;
}

void Starfield::release() {
    m_ps.reset();
    m_ready = false;
    m_failed = false;
}

void Starfield::resize(int, int, float) {}

void Starfield::applySettings(const AppSettings& settings) {
    m_cfg = settings.starfield;
}

void Starfield::update(float, double time) {
    m_time = time;
}

int Starfield::ensure(Renderer* renderer, int blockMs) {
    if (m_ready && m_ps) return 1;
    if (m_failed || !renderer) return -1;
    ID3D11PixelShader* ps = nullptr;
    const int st = renderer->compilePixelShader(kFullscreenHLSL, sizeof(kFullscreenHLSL), "fullscreen.hlsl",
                                                "PSStarfield", &ps, blockMs);
    if (st == 1 && ps) {
        m_ps = ComPtr<ID3D11PixelShader>(ps);
        m_ready = true;
        return 1;
    }
    if (st < 0) m_failed = true;
    return st;
}

void Starfield::precompile(Renderer* renderer) {
    ensure(renderer, 0);
}

void Starfield::render(const FrameContext& ctx) {
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
    cb.colorB[0] = cb.colorA[0];
    cb.colorB[1] = cb.colorA[1];
    cb.colorB[2] = cb.colorA[2];
    cb.colorB[3] = 1.0f;
    cb.density = m_cfg.density;
    cb.p0 = m_cfg.warp;
    cb.p1 = m_cfg.nebula;
    cb.p2 = m_cfg.twinkle;
    cb.direction = m_cfg.direction;
    cb.lookX = ctx.lookX;
    cb.lookY = ctx.lookY;
    cb.scroll = (float)wrapMod(m_time * (double)m_cfg.speed * 0.085, 64.0);
    cb.quality = 1.0f;
    const wchar_t* plate = L"still-night";
    if (m_cfg.warp > 0.4f) plate = L"warp";
    else if (m_cfg.nebula > 0.4f) plate = L"nebula";
    else if (m_cfg.density > 1.35f) plate = L"milky-way";
    ctx.renderer->bindPlates(L"stars", plate);
    cb.padA = ctx.renderer->plateMask();
    if (ctx.renderer->isWarp()) {
        cb.quality = 0.0f;
        if (cb.p0 > 0.2f) cb.p0 = 0.2f;
    }
    ctx.renderer->drawFullscreen(ps, cb);
}
