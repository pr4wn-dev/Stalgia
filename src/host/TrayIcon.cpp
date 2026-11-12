#include "host/TrayIcon.h"

#include "core/Log.h"
#include "core/Util.h"

#include <shellapi.h>

bool TrayIcon::create(HWND hwnd, HINSTANCE instance) {
    m_hwnd = hwnd;
    ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_nid.uCallbackMessage = kCallbackMsg;
    m_icon = loadAppIcon(instance, smallIconPx(hwnd));
    m_nid.hIcon = m_icon;
    if (!m_nid.hIcon) m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(m_nid.szTip, L"Stalgia", ARRAYSIZE(m_nid.szTip));

    if (!Shell_NotifyIconW(NIM_ADD, &m_nid)) {
        LOG_ERR("Shell_NotifyIcon ADD failed");
        return false;
    }
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
    m_added = true;
    return true;
}

void TrayIcon::reloadIcon(HINSTANCE instance) {
    if (!m_hwnd) return;
    HICON icon = loadAppIcon(instance, smallIconPx(m_hwnd));
    if (!icon) return;
    HICON old = m_icon;
    m_icon = icon;
    m_nid.hIcon = m_icon;
    m_nid.uFlags = NIF_ICON;
    if (m_added) Shell_NotifyIconW(NIM_MODIFY, &m_nid);
    m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    if (old && old != icon) DestroyIcon(old);
}

void TrayIcon::destroy() {
    if (m_added) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_added = false;
    }
    if (m_menu) {
        DestroyMenu(m_menu);
        m_menu = nullptr;
    }
    if (m_icon) {
        DestroyIcon(m_icon);
        m_icon = nullptr;
    }
}

void TrayIcon::setTip(const wchar_t* text) {
    lstrcpynW(m_nid.szTip, text, ARRAYSIZE(m_nid.szTip));
    m_nid.uFlags = NIF_TIP;
    if (m_added) Shell_NotifyIconW(NIM_MODIFY, &m_nid);
    m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
}

void TrayIcon::rebuildMenu(const EffectRegistry& registry, const std::string& currentEffect,
                           const std::vector<std::string>& favorites, int fps, bool userPaused, bool hidden,
                           bool widgetVisible, bool pauseFullscreen, bool pauseCovered, bool pauseBattery,
                           bool autostart, bool hideDesktopIcons) {
    if (m_menu) DestroyMenu(m_menu);
    m_menu = CreatePopupMenu();
    AppendMenuW(m_menu, MF_STRING | MF_DISABLED, 0, L"Stalgia");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING, Settings, L"Control panel…");
    AppendMenuW(m_menu, MF_STRING, widgetVisible ? HideWidget : ShowWidget,
                widgetVisible ? L"Hide widget" : L"Show widget");
    AppendMenuW(m_menu, MF_STRING, hidden ? ShowWallpaper : HideWallpaper,
                hidden ? L"Show wallpaper" : L"Hide wallpaper");
    AppendMenuW(m_menu, MF_STRING | (hidden ? MF_GRAYED : 0), Pause, userPaused ? L"Resume" : L"Pause");

    HMENU wall = CreatePopupMenu();
    const auto list = registry.list();
    auto appendItem = [&](const EffectInfo& e) {
        UINT index = 0;
        for (UINT i = 0; i < (UINT)list.size(); ++i) {
            if (list[i].id == e.id) {
                index = i;
                break;
            }
        }
        UINT flags = MF_STRING;
        if (e.id == currentEffect) flags |= MF_CHECKED;
        AppendMenuW(wall, flags, EffectBase + index, widen(e.displayName).c_str());
    };
    auto appendGroup = [&](const wchar_t* title, const std::vector<EffectInfo>& items, bool allowEmpty) {
        if (items.empty() && !allowEmpty) return;
        AppendMenuW(wall, MF_STRING | MF_GRAYED, 0, title);
        if (items.empty()) {
            AppendMenuW(wall, MF_STRING | MF_GRAYED, 0, L"(none)");
            return;
        }
        for (const auto& e : items) appendItem(e);
    };
    std::vector<EffectInfo> favs;
    for (const auto& id : favorites) {
        if (const EffectInfo* info = registry.find(id)) favs.push_back(*info);
    }
    appendGroup(L"Favorites", favs, true);
    for (const auto& group : registry.groups()) {
        std::vector<EffectInfo> items;
        for (const auto& e : list) {
            if (e.group == group) items.push_back(e);
        }
        appendGroup(widen(group).c_str(), items, false);
    }
    AppendMenuW(wall, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(wall, MF_STRING, OpenGallery, L"Open Gallery…");
    AppendMenuW(m_menu, MF_POPUP, (UINT_PTR)wall, L"Wallpaper");

    HMENU fpsMenu = CreatePopupMenu();
    AppendMenuW(fpsMenu, MF_STRING | (fps == 15 ? MF_CHECKED : 0), Fps15, L"15 FPS");
    AppendMenuW(fpsMenu, MF_STRING | (fps == 30 ? MF_CHECKED : 0), Fps30, L"30 FPS");
    AppendMenuW(fpsMenu, MF_STRING | (fps == 60 ? MF_CHECKED : 0), Fps60, L"60 FPS");
    AppendMenuW(m_menu, MF_POPUP, (UINT_PTR)fpsMenu, L"Frame rate");

    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING | (pauseFullscreen ? MF_CHECKED : 0), PauseFullscreen,
                L"Pause on fullscreen");
    AppendMenuW(m_menu, MF_STRING | (pauseCovered ? MF_CHECKED : 0), PauseCovered,
                L"Pause when desktop is covered");
    AppendMenuW(m_menu, MF_STRING | (pauseBattery ? MF_CHECKED : 0), PauseBattery, L"Pause on battery saver");
    AppendMenuW(m_menu, MF_STRING | (autostart ? MF_CHECKED : 0), Autostart, L"Start with Windows");
    AppendMenuW(m_menu, MF_STRING | (hideDesktopIcons ? MF_CHECKED : 0), HideDesktopIcons,
                L"Hide desktop icons");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING, OpenConfig, L"Open config file");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING, Exit, L"Exit");
}

void TrayIcon::showMenu() {
    if (!m_menu || !m_hwnd) return;
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);
    TrackPopupMenuEx(m_menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
}
