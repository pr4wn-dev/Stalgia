#include "effects/Aurora.h"

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

bool Aurora::initialize(ID3D11Device*, ID3D11DeviceContext*) {
    return true;
}

void Aurora::release() {
    m_ps.reset();
    m_ready = false;
    m_failed = false;
}

void Aurora::resize(int, int, float) {}

void Aurora::applySettings(const AppSettings& settings) {
    m_cfg = settings.aurora;
}

void Aurora::update(float, double time) {
    m_time = time;
}

int Aurora::ensure(Renderer* renderer, int blockMs) {
    if (m_ready && m_ps) return 1;
    if (m_failed || !renderer) return -1;
    ID3D11PixelShader* ps = nullptr;
    const int st = renderer->compilePixelShader(kFullscreenHLSL, sizeof(kFullscreenHLSL), "fullscreen.hlsl", "PSAurora",
                                                &ps, blockMs);
    if (st == 1 && ps) {
        m_ps = ComPtr<ID3D11PixelShader>(ps);
        m_ready = true;
        return 1;
    }
    if (st < 0) m_failed = true;
    return st;
}

void Aurora::precompile(Renderer* renderer) {
    ensure(renderer, 0);
}

void Aurora::render(const FrameContext& ctx) {
    if (!ctx.renderer || !ctx.device) return;
    ensure(ctx.renderer, 0);
    ID3D11PixelShader* ps = m_ready ? m_ps.get() : ctx.renderer->fallbackShader();
    if (!ps) return;

    Renderer::FullscreenCB cb{};
    cb.time = (float)wrapMod(m_time, 3600.0);
    cb.width = (float)ctx.width;
    cb.height = (float)ctx.height;
    cb.speed = m_cfg.speed;
    rgbFromHex(m_cfg.colorA, cb.colorA[0], cb.colorA[1], cb.colorA[2]);
    cb.colorA[3] = 1.0f;
    rgbFromHex(m_cfg.colorB, cb.colorB[0], cb.colorB[1], cb.colorB[2]);
    cb.colorB[3] = 1.0f;
    cb.p0 = m_cfg.height;
    cb.p1 = m_cfg.rays;
    cb.p2 = m_cfg.stars ? 1.0f : 0.0f;
    cb.p3 = m_cfg.horizon ? 1.0f : 0.0f;
    cb.lookX = ctx.lookX;
    cb.lookY = ctx.lookY;
    cb.scroll = (float)wrapMod(m_time * (double)m_cfg.speed * 0.085, 64.0);
    cb.quality = 1.0f;
    const wchar_t* plate = L"classic";
    if (m_cfg.horizon) plate = L"horizon";
    else if (m_cfg.colorA == 0xFF5FA2u) plate = L"magenta";
    else if (m_cfg.colorA == 0x86FFE0u) plate = L"polar";
    ctx.renderer->bindPlates(L"aurora", plate);
    cb.padA = ctx.renderer->plateMask();
    if (ctx.renderer->isWarp()) {
        cb.quality = 0.0f;
        cb.p3 = 0.0f;
    }
    ctx.renderer->drawFullscreen(ps, cb);
}
