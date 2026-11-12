#pragma once

#include "effect/EffectRegistry.h"

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

class Application;

class TrayIcon {
public:
    bool create(HWND hwnd, HINSTANCE instance);
    void destroy();
    void reloadIcon(HINSTANCE instance);
    void rebuildMenu(const EffectRegistry& registry, const std::string& currentEffect,
                     const std::vector<std::string>& favorites, int fps, bool userPaused, bool hidden,
                     bool widgetVisible, bool pauseFullscreen, bool pauseCovered, bool pauseBattery, bool autostart,
                     bool hideDesktopIcons);
    void showMenu();
    void setTip(const wchar_t* text);

    static constexpr UINT kCallbackMsg = WM_APP + 7;

    enum Cmd : UINT {
        Settings = 1000,
        Pause = 1001,
        OpenConfig = 1002,
        ShowWallpaper = 1003,
        HideWallpaper = 1004,
        ShowWidget = 1005,
        HideWidget = 1006,
        Exit = 1999,
        Fps15 = 1201,
        Fps30 = 1202,
        Fps60 = 1203,
        PauseFullscreen = 1301,
        PauseCovered = 1302,
        PauseBattery = 1303,
        Autostart = 1304,
        HideDesktopIcons = 1305,
        EffectBase = 1400,
        OpenGallery = 1600,
    };

private:
    HWND m_hwnd = nullptr;
    NOTIFYICONDATAW m_nid{};
    HMENU m_menu = nullptr;
    HICON m_icon = nullptr;
    bool m_added = false;
};
