#pragma once

#include "host/SettingsPanel.h"

#include <string>
#include <windows.h>

struct GalleryUi;

// Dark, resizable wallpaper browser. Thumbnails are drawn only while this
// window is open and not minimized. Layout is in DIPs; the HWND render target
// is already DPI-scaled, so painting must not scale a second time.
class WallpaperGallery {
    friend struct GalleryUi;

public:
    static constexpr wchar_t kClass[] = L"StalgiaGallery";

    bool show(HINSTANCE instance, SettingsPanel::Host* host, bool offscreen);
    void hide();
    void refresh();
    void destroy();
    bool visible() const { return m_visible; }
    bool acceptsThumbs() const;
    HWND hwnd() const { return m_hwnd; }
    // Hit-tests groups and cards at several sizes, including 125% of a 1024-wide window.
    bool runUiSelfTest(std::string& report);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool initFactories();
    void releaseFactories();
    void paint();
    void syncSelectionToEffect();
    void setSlider(int index, float t);
    void toggleAt(int index);
    void pickColor(int which);
    void pickImage();
    void editGlyphs();
    float pxToDip(float px) const;
    void clientDipSize(float& w, float& h) const;

    HINSTANCE m_instance = nullptr;
    SettingsPanel::Host* m_host = nullptr;
    HWND m_hwnd = nullptr;
    bool m_classReg = false;
    bool m_visible = false;
    bool m_offscreen = false;
    UINT m_dpi = 96;
    int m_group = 0;
    std::string m_panelId;
    float m_gridScroll = 0;
    float m_sideScroll = 0;
    int m_hotKind = 0;
    int m_hotIndex = -1;
    std::string m_hotId;
    int m_downKind = 0;
    int m_downIndex = -1;
    std::string m_downId;
    int m_dragSlider = -1;
    int m_dragScroll = 0;
    float m_scrollGrab = 0;
    int m_preset = -1;
    bool m_trackLeave = false;
    void* m_d2d = nullptr;
    void* m_dwrite = nullptr;
    void* m_hwndRt = nullptr;
};
