#pragma once

#include "core/ComPtr.h"
#include "core/Config.h"
#include "effect/IEffect.h"
#include "gfx/Renderer.h"

class Snow final : public IEffect {
public:
    const char* id() const override { return "snow"; }
    const char* displayName() const override { return "Snow"; }

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void release() override;

    void resize(int width, int height, float dpiScale) override;
    void applySettings(const AppSettings& settings) override;

    void update(float dt, double time) override;
    void render(const FrameContext& ctx) override;
    void precompile(Renderer* renderer) override;
    bool ready() const override { return m_ready; }

private:
    int ensure(Renderer* renderer, int blockMs);

    ComPtr<ID3D11PixelShader> m_ps;
    SnowSettings m_cfg;
    double m_time = 0;
    bool m_ready = false;
    bool m_failed = false;
};
