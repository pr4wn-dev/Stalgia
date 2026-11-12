#pragma once

#include "host/SettingsPanel.h"
#include "host/SysMonitor.h"

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

struct ID2D1RenderTarget;
struct IWICImagingFactory;
struct IWICBitmap;

// Frameless Direct2D desktop gadget. Sits on the Progman desktop layer
// (above the wallpaper, below normal windows), never in the taskbar / Alt-Tab.
class DesktopWidget {
public:
    static constexpr wchar_t kClass[] = L"StalgiaWidget";

    bool create(HINSTANCE instance, SettingsPanel::Host* host, bool desktopLayer, HWND owner = nullptr);
    void destroy();
    void show();
    void hide();
    void refresh();
    bool visible() const;
    // Expanded, on screen, and not minimized. Thumbnail rendering keys off this.
    bool wantsLiveThumb() const;
    bool created() const { return m_hwnd != nullptr; }
    HWND hwnd() const { return m_hwnd; }
    SysSample sysSnapshot() const { return m_sys.snapshot(); }
    // Simulated clicks and drags through the widget window procedure.
    bool runSettingsSelfTest(std::string& report);
    // Full GPU sampling for the selftest. Does not write widgetMini.
    void forceFullSampling();
    bool samplingMini() const { return m_miniTarget > 0.5f; }

private:
    enum class HitKind : int {
        None = 0,
        Drag,
        MiniToggle,
        Play,
        Gear,
        Fps,
        Icons,
        Tile0,
        Tile1,
        Tile2,
        Gallery,
        Drawer,
        Preset,
        Slider,
        Toggle,
        Swatch,
        Scroll,
    };

    struct Hit {
        HitKind kind = HitKind::None;
        int index = -1;
        bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
        bool operator!=(const Hit& o) const { return !(*this == o); }
    };

    struct DipRect {
        float x = 0, y = 0, w = 0, h = 0;
        bool contains(float px, float py) const {
            return px >= x && py >= y && px < x + w && py < y + h;
        }
    };

    struct Layout {
        DipRect card, header, logo, title, miniBtn, preview;
        DipRect wallpaper, wallThumb, wallPrev, wallNext;
        DipRect play, fps, icons, gear, drawer, sys, gauges[3], spark, sysFoot;
        DipRect galleryBtn;
        DipRect miniCpu;
        struct Chip {
            DipRect box;
            int index = 0;
            std::wstring label;
        };
        struct Head {
            DipRect box;
            const wchar_t* label = L"";
        };
        struct Row {
            DipRect box;
            DipRect track;
            DipRect labelBox;
            DipRect valueBox;
            int index = 0;
            int kind = 0;  // KnobKind
            const wchar_t* label = L"";
            std::wstring value;
            float t = 0;
            bool on = false;
            uint32_t rgb = 0;
        };
        std::vector<Chip> chips;
        std::vector<Head> heads;
        std::vector<Row> rows;
        float contentH = 0;
        DipRect scrollBar;
        float cardW = 0, cardH = 0;
        float shadow = 0;
    };

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool initFactories();
    void releaseFactories();
    void updateDpi();
    void computeSize();
    Layout buildLayout() const;
    void positionWindow(bool keepScreenPos);
    void pinDesktopZ();
    bool createTopLevelWindow(HWND owner);
    HWND sanitizeOwner(HWND owner) const;
    bool applyDwmAcrylic();
    bool enableLayeredFallback();
    bool ensureHwndTarget();
    void releaseHwndTarget();
    void paint();
    bool presentLayer(const unsigned char* bgra, int w, int h);
    void startAnim();
    void stopAnimIfIdle();
    void tickAnim();
    Hit hitTest(float x, float y) const;
    void onClick(Hit hit, float x, float y);
    void onDragSlider(float x);
    int hoverSlot(Hit hit) const;
    uint32_t accentRgb() const;
    void setMini(bool mini);
    void saveChrome();
    void syncSysSampling();
    void onSysSample();
    uint32_t healthRgb(float pct, uint32_t accent) const;
    void releaseLogoCache();
    IWICBitmap* ensureLogoScaled(IWICImagingFactory* wic, UINT destPx);
    bool drawAppBadge(ID2D1RenderTarget* rt, IWICImagingFactory* wic, const DipRect& box);

    HINSTANCE m_instance = nullptr;
    SettingsPanel::Host* m_host = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_owner = nullptr;
    bool m_desktopLayer = false;
    bool m_classReg = false;
    bool m_visible = false;
    bool m_useDwm = false;
    bool m_pinningZ = false;

    UINT m_dpi = 96;
    float m_scale = 1.0f;
    int m_pixelW = 1;
    int m_pixelH = 1;

    float m_drawerT = 0;
    float m_drawerTarget = 0;
    float m_drawerScroll = 0;
    float m_miniT = 0;
    float m_miniTarget = 0;
    float m_hover[12]{};
    Hit m_hot{};
    Hit m_pressed{};
    int m_dragSlider = -1;
    bool m_draggingWin = false;
    bool m_dragScroll = false;
    float m_scrollGrab = 0;
    POINT m_dragScreen{};
    int m_dragOrigX = 0, m_dragOrigY = 0;
    bool m_trackingLeave = false;
    bool m_timerOn = false;
    ULONGLONG m_lastTick = 0;
    float m_previewTime = 0;

    static constexpr UINT kSampleMsg = WM_APP + 31;
    SysMonitor m_sys;
    SysSample m_sysSnap;
    float m_dCpu = 0, m_dRam = 0, m_dGpu = 0;

    void* m_d2d = nullptr;     // ID2D1Factory*
    void* m_dwrite = nullptr;  // IDWriteFactory*
    void* m_wic = nullptr;     // IWICImagingFactory*
    void* m_hwndRt = nullptr;  // ID2D1HwndRenderTarget*
    void* m_logoSrc = nullptr;     // IWICBitmap* 256 px PBGRA
    void* m_logoScaled = nullptr;  // IWICBitmap* destPx PBGRA
    int m_logoPx = 0;
};
