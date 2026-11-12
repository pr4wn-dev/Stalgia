#pragma once

#include <string>
#include <windows.h>

#include "host/UiMetrics.h"

struct AppSettings;
class EffectRegistry;
class ThumbnailStage;

class SettingsPanel {
public:
    class Host {
    public:
        virtual ~Host() = default;
        virtual AppSettings& settings() = 0;
        virtual const EffectRegistry& registry() const = 0;
        virtual void applySettingsLive() = 0;
        // Write a debounced settings change now. Default is a no-op.
        virtual void flushSettings() {}
        virtual void switchEffect(const std::string& id) = 0;
        virtual void setWallpaperHidden(bool hidden) = 0;
        virtual void setUserPaused(bool paused) = 0;
        virtual bool wallpaperHidden() const = 0;
        virtual bool userPaused() const = 0;
        virtual void openConfigFile() = 0;
        virtual void setHideDesktopIcons(bool hide) = 0;
        virtual void exitApp() = 0;
        virtual void openGallery() = 0;
        virtual void cycleWallpaper(int dir) = 0;
        virtual bool isFavorite(const std::string& id) const = 0;
        virtual void toggleFavorite(const std::string& id) = 0;
        virtual ThumbnailStage* thumbs() = 0;
    };

    static constexpr wchar_t kClass[] = L"StalgiaPanel";

    bool create(HINSTANCE instance, Host* host);
    void destroy();
    void show();
    void refresh();
    bool visible() const;
    HWND hwnd() const { return m_hwnd; }
    bool runScrollSelfTest(std::string& report);

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT msg, WPARAM wParam, LPARAM lParam);

    void createControls();
    void layout();
    void syncToUi();
    void applyFromUi();
    void updateValueLabels();
    void updateColorButtons();
    void pickColor(int which);
    int dpiPx(int px) const;
    int contentHeightPx() const;
    bool controlShown(HWND w) const;
    void updateScrollBar();
    HWND addStatic(const wchar_t* text, int id = 0, DWORD extra = SS_CENTERIMAGE | SS_NOPREFIX);
    HWND addBtn(const wchar_t* text, int id);
    HWND addCheck(const wchar_t* text, int id);
    HWND addTrack(int id, int minV, int maxV);

    HINSTANCE m_instance = nullptr;
    Host* m_host = nullptr;
    HWND m_hwnd = nullptr;
    HFONT m_font = nullptr;
    int m_dpi = 96;
    int m_scroll = 0;
    int m_contentPx = 1;
    bool m_syncing = false;
    bool m_inLayout = false;

    HWND m_lblTitle = nullptr;
    HWND m_status = nullptr;
    HWND m_lblWallpaper = nullptr;
    HWND m_list = nullptr;
    HWND m_show = nullptr;
    HWND m_hide = nullptr;
    HWND m_pause = nullptr;
    HWND m_grpFx = nullptr;
    HWND m_lblColorA = nullptr;
    HWND m_btnColorA = nullptr;
    HWND m_swatchA = nullptr;
    HWND m_lblColorB = nullptr;
    HWND m_btnColorB = nullptr;
    HWND m_swatchB = nullptr;
    HWND m_lblSpeed = nullptr;
    HWND m_slSpeed = nullptr;
    HWND m_valSpeed = nullptr;
    HWND m_lblDensity = nullptr;
    HWND m_slDensity = nullptr;
    HWND m_valDensity = nullptr;
    HWND m_lblGlyph = nullptr;
    HWND m_slGlyph = nullptr;
    HWND m_valGlyph = nullptr;
    HWND m_lblTrail = nullptr;
    HWND m_slTrail = nullptr;
    HWND m_valTrail = nullptr;
    HWND m_lblExtra1 = nullptr;
    HWND m_slExtra1 = nullptr;
    HWND m_valExtra1 = nullptr;
    HWND m_lblExtra2 = nullptr;
    HWND m_slExtra2 = nullptr;
    HWND m_valExtra2 = nullptr;
    HWND m_chkEye = nullptr;
    HWND m_chkBreathe = nullptr;
    HWND m_chkColor = nullptr;
    HWND m_lblPlayback = nullptr;
    HWND m_lblFps = nullptr;
    HWND m_slFps = nullptr;
    HWND m_valFps = nullptr;
    HWND m_chkFs = nullptr;
    HWND m_chkCovered = nullptr;
    HWND m_chkBattery = nullptr;
    HWND m_chkAuto = nullptr;
    HWND m_chkIcons = nullptr;
    HWND m_btnGallery = nullptr;
    HWND m_btnOpenCfg = nullptr;
    HWND m_btnExit = nullptr;
    HWND m_note = nullptr;
};
