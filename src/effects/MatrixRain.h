#pragma once

#include "core/Config.h"
#include "effect/IEffect.h"
#include "gfx/GlyphAtlas.h"
#include "gfx/Renderer.h"

#include <cstdint>
#include <vector>

class MatrixRain final : public IEffect {
public:
    const char* id() const override { return "matrix"; }
    const char* displayName() const override { return "Matrix rain"; }

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void release() override;

    void resize(int width, int height, float dpiScale) override;
    void applySettings(const AppSettings& settings) override;

    void update(float dt, double time) override;
    void render(const FrameContext& ctx) override;
    void precompile(Renderer* renderer) override;

private:
    struct Column {
        float head = 0;
        float base = 12;
        float trail = 18;
        float delay = 0;
        float layer = 1;
        float glitch = 0;
        uint32_t seed = 1;
        uint32_t generation = 1;
        bool active = true;
    };

    void rebuildColumns();
    void resetColumn(Column& c, bool spawnOnScreen);

    ID3D11Device* m_device = nullptr;
    GlyphAtlas m_atlas;
    MatrixSettings m_cfg;
    std::vector<Column> m_cols;
    std::vector<GlyphInstance> m_instances;

    int m_width = 0;
    int m_height = 0;
    float m_dpi = 1.0f;
    int m_primaryHeight = 1080;
    float m_cell = 18.0f;
    int m_rows = 0;
    double m_time = 0;
    bool m_atlasDirty = true;
    int m_atlasCell = 0;
    int m_builtGlyph = -1;
    std::string m_builtCustom;
    float m_builtDensity = -1;
    float m_builtRegion = -1;
};
