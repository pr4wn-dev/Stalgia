#pragma once

#include "core/Config.h"
#include "effect/IEffect.h"
#include "gfx/GlyphAtlas.h"
#include "gfx/Renderer.h"

#include <cstdint>
#include <vector>

// Shared 1.4.5-style rain-reveal: dense wrapping katakana rain, character
// visible only because trails are brighter / slower-fading inside a luma mask.
// Optional RGB palette tints glyphs inside the silhouette.
struct CharacterMask {
    const unsigned char* luma = nullptr;
    const unsigned char* rgb = nullptr;  // RGB888, same w*h as luma, or null
    int w = 0;
    int h = 0;
};

class RainReveal : public IEffect {
public:
    RainReveal(const char* id, const char* displayName, CharacterMask mask);

    const char* id() const override { return m_id; }
    const char* displayName() const override { return m_name; }

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void release() override;

    void resize(int width, int height, float dpiScale) override;
    void applySettings(const AppSettings& settings) override;

    void update(float dt, double time) override;
    void render(const FrameContext& ctx) override;
    void precompile(Renderer* renderer) override;

protected:
    struct RevealKnobs {
        uint32_t color = 0xF2F6FF;
        float bgDim = 1.00f;
        float brightness = 1.70f;
        float size = 0.85f;
        float posX = 0.50f;
        float speed = 1.0f;
        int glyphSize = 9;
        bool eyeGlow = false;
        bool breathing = true;
        bool colorize = false;
        float reveal = 1.0f;
        float detail = 0.5f;
        float breatheAmt = 1.0f;
        std::string imagePath;
    };

    virtual RevealKnobs knobsFrom(const AppSettings& settings) const = 0;

private:
    struct Stream {
        float head = 0;
        float base = 12;
        float trail = 18;
        float delay = 0;
        uint32_t seed = 1;
        uint32_t generation = 1;
    };

    struct Column {
        Stream s[2];
        uint8_t n = 1;
        uint32_t seed = 1;
    };

    void rebuildColumns();
    void resetStream(Stream& s, uint32_t colSeed, uint32_t which, bool spawnOnScreen);
    void bindMask();
    void loadCustom(const std::string& path);
    float sampleLumaAt(float x, float y) const;
    float sampleLuma(float x, float y) const;
    void samplePalette(float x, float y, float& r, float& g, float& b) const;
    float wrapDist(float head, float row) const;
    static float trailBri(float dist, float trail, bool inside);
    static float shadeLuma(float luma);

    const char* m_id = "";
    const char* m_name = "";
    CharacterMask m_mask;
    CharacterMask m_active;
    std::string m_imagePath;
    std::vector<uint8_t> m_customLuma;
    std::vector<uint8_t> m_customRgb;
    int m_customW = 0;
    int m_customH = 0;
    float m_builtDim = -1;
    int m_builtGlyph = -1;

    ID3D11Device* m_device = nullptr;
    GlyphAtlas m_atlas;
    RevealKnobs m_cfg;
    std::vector<Column> m_cols;
    std::vector<GlyphInstance> m_instances;

    int m_width = 0;
    int m_height = 0;
    float m_dpi = 1.0f;
    int m_primaryHeight = 1080;
    float m_cell = 16.0f;
    int m_rows = 0;
    double m_time = 0;
    bool m_atlasDirty = true;
    int m_atlasCell = 0;
};
