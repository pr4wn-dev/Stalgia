#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Shared grade and behavior. Stored on each effect so a preset can set them
// without changing the others. fpsCap 0 follows the global cap.
struct EffectLook {
    float brightness = 1.0f;
    float saturation = 1.0f;
    float vignette = 0.18f;
    int fpsCap = 0;
    bool timeOfDay = false;
    bool parallax = false;
};

struct MatrixSettings {
    uint32_t color = 0x00FF41;
    uint32_t headColor = 0xDEFFDE;
    float speed = 1.0f;
    float density = 1.0f;
    int glyphSize = 18;  // designed around 1080p; scaled with resolution
    float trailLength = 22.0f;
    float fadePower = 1.35f;
    float mutate = 0.08f;
    int glyphSet = 0;  // 0 katakana, 1 latin, 2 binary, 3 custom
    std::string customGlyphs;
    float depth = 0.55f;
    float bloom = 0.6f;
    float glitch = 0.18f;
    float direction = 1.0f;  // +1 down, -1 up
    float wind = 0.12f;
    float region = 0.0f;  // 0 uniform, 1 denser in the middle
    EffectLook look;
};

struct StarfieldSettings {
    uint32_t color = 0xF3F6FF;
    float speed = 0.28f;
    float density = 1.15f;
    float twinkle = 0.40f;
    float nebula = 0.16f;
    float warp = 0.0f;
    float direction = 0.0f;
    EffectLook look;
    StarfieldSettings() { look.fpsCap = 30; }
};

struct AuroraSettings {
    uint32_t colorA = 0x3CFF78;
    uint32_t colorB = 0xFF4B7A;
    float speed = 0.32f;
    float height = 0.72f;
    float rays = 0.82f;
    bool stars = true;
    bool horizon = false;
    EffectLook look;
    AuroraSettings() { look.fpsCap = 30; }
};

struct LavaSettings {
    uint32_t lavaColor = 0xFF6A14;
    uint32_t liquidColor = 0x100814;
    int blobCount = 11;
    float speed = 1.0f;
    float glow = 1.4f;
    float blobSize = 1.15f;
    float viscosity = 1.0f;
    float heat = 0.65f;
    float gradient = 0.60f;
    float lamp = 0.0f;  // 0 full screen, 1 glass capsule
    EffectLook look;
};

struct ApeSettings {
    uint32_t color = 0xF2F6FF;
    float bgDim = 1.00f;
    float apeBrightness = 1.70f;
    float size = 0.85f;
    float posX = 0.50f;
    float speed = 1.0f;
    int glyphSize = 9;
    bool eyeGlow = false;
    bool breathing = true;
    float reveal = 1.0f;
    float detail = 0.50f;
    float breatheAmt = 1.0f;
    bool fromImage = false;
    std::string imagePath;
    EffectLook look;
};

struct MycoSettings {
    uint32_t color = 0xF2F6FF;
    float bgDim = 1.00f;
    float brightness = 1.70f;
    float size = 0.85f;
    float posX = 0.50f;
    float speed = 1.0f;
    int glyphSize = 7;
    bool eyeGlow = false;
    bool breathing = true;
    bool colorize = true;
    float reveal = 0.85f;
    float detail = 0.55f;
    float breatheAmt = 1.0f;
    std::string imagePath;
    EffectLook look;
};

struct AppSettings {
    int version = 2;
    int fps = 30;
    std::string effect = "matrix";
    std::vector<std::string> favorites;
    bool pauseOnFullscreen = true;
    bool pauseOnCovered = true;
    bool pauseOnBatterySaver = true;
    bool startWithWindows = false;
    bool showWidget = true;
    bool hideDesktopIcons = false;
    bool widgetMini = false;
    int widgetX = -1;
    int widgetY = -1;
    int tuneMatrix = 0;
    int tuneStarfield = 0;
    int tuneAurora = 0;
    int tuneLava = 0;
    int tuneApe = 0;
    int tuneMyco = 0;
    MatrixSettings matrix;
    StarfieldSettings starfield;
    AuroraSettings aurora;
    LavaSettings lava;
    ApeSettings ape;
    MycoSettings myco;
};

class Config {
public:
    // On-disk JSON format. Bump when top-level keys change.
    static constexpr int kFormatVersion = 2;

    // Per-effect knob generations. Bump when THAT wallpaper's defaults change
    // so stale saved values are replaced instead of silently overriding them.
    // Missing tune.* keys load as 0, which is older than every current generation.
    static constexpr int kTuneMatrix = 2;      // 1.9.0 depth, bloom, glyph sets
    static constexpr int kTuneStarfield = 4;   // 1.10.2 single-cell stars, 30 fps cap
    static constexpr int kTuneAurora = 4;      // 1.10.2 ribbon aurora, horizon 0.85, 30 fps cap
    static constexpr int kTuneLava = 2;        // 1.9.0 lamp, viscosity, heat
    static constexpr int kTuneApe = 3;         // 1.9.0 reveal, detail, custom image
    static constexpr int kTuneMyco = 3;

    bool load();
    bool save() const;

    AppSettings& settings() { return m_s; }
    const AppSettings& settings() const { return m_s; }

    const std::wstring& path() const { return m_path; }
    bool migrated() const { return m_migrated; }
    bool fresh() const { return m_fresh; }

    static int clampFps(int fps);

private:
    AppSettings m_s;
    std::wstring m_path;
    bool m_migrated = false;
    bool m_fresh = false;

    void resolvePath();
    bool parse(const std::string& text);
    std::string serialize() const;
    bool migrateStaleTunes();
    void clampSettings();
};
