#pragma once

#include "core/ComPtr.h"
#include "core/Config.h"
#include "effect/IEffect.h"
#include "gfx/Renderer.h"

#include <vector>

struct LavaCB {
    float time = 0;
    float width = 1;
    float height = 1;
    float speed = 1;
    float lava[4] = {1, 0.4f, 0.08f, 1};
    float liquid[4] = {0.06f, 0.03f, 0.08f, 1};
    float glow = 1.4f;
    float blobScale = 1;
    float count = 0;
    float threshold = 0.28f;
    float heat = 0.65f;
    float gradient = 0.6f;
    float lamp = 0;
    float specGain = 1;
    float lookX = 0;
    float lookY = 0;
    float pad0 = 0;
    float pad1 = 0;
    float blobs[16][4]{};
};

class LavaLamp final : public IEffect {
public:
    static constexpr int kMaxBlobs = 16;

    const char* id() const override { return "lava"; }
    const char* displayName() const override { return "Lava lamp"; }

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
    enum State : int {
        kPool = 0,     // wide mound sitting on the heater
        kForming = 1,  // overlapping the pool, stretching into a neck
        kFree = 2,     // detached glob drifting / hanging / sinking
        kQueued = 3    // invisible; waits to become the next pinch-off
    };

    struct Blob {
        float x = 0.5f;
        float y = 0.08f;  // 0 = bottom, 1 = top
        float vx = 0;
        float vy = 0;
        float r = 0.06f;
        float temp = 0.4f;
        float phase = 0;
        float timer = 0;
        int state = kQueued;
        uint32_t seed = 1;
    };

    void syncCount();
    void spawn(Blob& b, int index);
    void step(float dt);
    float gooey() const;

    ComPtr<ID3D11PixelShader> m_ps;
    LavaSettings m_cfg;
    std::vector<Blob> m_blobs;
    int m_width = 1;
    int m_height = 1;
    double m_simTime = 0;
    float m_accum = 0;
    bool m_inited = false;
    bool m_ready = false;
    bool m_failed = false;
};
