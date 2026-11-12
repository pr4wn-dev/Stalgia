#pragma once

#include <d3d11.h>
#include <string>

struct AppSettings;
class Renderer;

// Per-frame data passed to an effect after the renderer has bound the backbuffer.
struct FrameContext {
    Renderer* renderer = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    int width = 0;
    int height = 0;
    float dpiScale = 1.0f;
    int primaryHeight = 1080;  // used to scale glyph size with resolution
    float dt = 0.0f;
    double time = 0.0;
    float lookX = 0.0f;  // subtle cursor parallax, already scaled, 0 when off
    float lookY = 0.0f;
};

// One animated wallpaper. To add a new effect:
//   1. Create src/effects/YourEffect.h/.cpp implementing IEffect.
//   2. Register it in EffectRegistry::EffectRegistry() with add("id", "Menu Name", factory).
//      Sources outside the repo use addExternal from STALGIA_PRIVATE_EFFECTS_DIR.
//   3. That's it — the tray Wallpaper menu and config "effect" key pick it up.
class IEffect {
public:
    virtual ~IEffect() = default;

    virtual const char* id() const = 0;
    virtual const char* displayName() const = 0;

    virtual bool initialize(ID3D11Device* device, ID3D11DeviceContext* context) = 0;
    virtual void release() = 0;

    virtual void resize(int width, int height, float dpiScale) = 0;
    virtual void applySettings(const AppSettings& settings) = 0;

    virtual void update(float dt, double time) = 0;
    virtual void render(const FrameContext& ctx) = 0;

    // Compile shaders / build atlases before the first present so a switch doesn't flash.
    virtual void precompile(Renderer*) {}

    // True only when this effect's real shader is bound. A fallback fill does not count.
    virtual bool ready() const { return true; }
};
