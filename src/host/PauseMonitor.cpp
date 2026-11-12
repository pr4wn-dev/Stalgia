#include "host/PauseMonitor.h"

#include "core/Util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dwmapi.h>
#include <shellapi.h>
#include <string>

namespace {

bool classIs(HWND hwnd, const wchar_t* want) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    return lstrcmpW(cls, want) == 0;
}

bool isShellWindow(HWND hwnd) {
    wchar_t cls[80]{};
    GetClassNameW(hwnd, cls, 80);
    return lstrcmpW(cls, L"Progman") == 0 || lstrcmpW(cls, L"WorkerW") == 0 ||
           lstrcmpW(cls, L"SHELLDLL_DefView") == 0 || lstrcmpW(cls, L"SysListView32") == 0 ||
           lstrcmpW(cls, L"Shell_TrayWnd") == 0 || lstrcmpW(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           lstrcmpW(cls, L"NotifyIconOverflowWindow") == 0 || lstrcmpW(cls, L"ForegroundStaging") == 0 ||
           lstrcmpW(cls, L"DummyDWMListenerWindow") == 0 ||
           lstrcmpW(cls, L"ApplicationManager_DesktopShellWindow") == 0 ||
           lstrcmpW(cls, L"XamlExplorerHostIslandWindow") == 0 ||
           lstrcmpW(cls, L"Windows.Internal.Shell.TabProxyWindow") == 0 ||
           lstrcmpW(cls, L"StalgiaSurface") == 0 || lstrcmpW(cls, L"StalgiaHost") == 0 ||
           lstrcmpW(cls, L"StalgiaPanel") == 0 || lstrcmpW(cls, L"StalgiaWidget") == 0;
}

bool isCloaked(HWND hwnd) {
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return cloaked != FALSE;
    }
    return false;
}

bool coversRect(HWND hwnd, const RECT& area, double minFrac) {
    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
    const long aw = area.right - area.left;
    const long ah = area.bottom - area.top;
    const long overlapW = std::max(0L, std::min(wr.right, area.right) - std::max(wr.left, area.left));
    const long overlapH = std::max(0L, std::min(wr.bottom, area.bottom) - std::max(wr.top, area.top));
    const double areaPx = (double)aw * (double)ah;
    if (areaPx <= 0) return false;
    return ((double)overlapW * (double)overlapH) / areaPx >= minFrac;
}

bool skipWindow(HWND hwnd) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || isShellWindow(hwnd) || isCloaked(hwnd)) {
        return true;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return true;
    const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return true;
    return false;
}

}  // namespace

const char* PauseMonitor::causeName(Cause c) {
    switch (c) {
        case Cause::User: return "user";
        case Cause::Locked: return "session-locked";
        case Cause::Battery: return "battery-saver";
        case Cause::Fullscreen: return "fullscreen";
        case Cause::Maximized: return "maximized";
        case Cause::Covered: return "desktop-covered";
        default: return "none";
    }
}

void PauseMonitor::configure(bool pauseOnFullscreen, bool pauseOnCovered, bool pauseOnBatterySaver) {
    m_pauseFullscreen = pauseOnFullscreen;
    m_pauseCovered = pauseOnCovered;
    m_pauseBattery = pauseOnBatterySaver;
}

HWND PauseMonitor::foregroundRoot() const {
    HWND raw = GetForegroundWindow();
    if (!raw) return nullptr;
    HWND fg = GetAncestor(raw, GA_ROOT);
    return fg ? fg : raw;
}

void PauseMonitor::describe(HWND fg, HWND raw, const char* why) {
    wchar_t cls[64]{};
    wchar_t title[96]{};
    if (fg) {
        GetClassNameW(fg, cls, 64);
        GetWindowTextW(fg, title, 96);
    }
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    RECT wr{};
    if (fg) GetWindowRect(fg, &wr);
    const LONG style = fg ? GetWindowLongW(fg, GWL_STYLE) : 0;
    const LONG ex = fg ? GetWindowLongW(fg, GWL_EXSTYLE) : 0;
    std::snprintf(m_detail, sizeof(m_detail),
                  "%s hwnd=%p raw=%p class='%s' title='%s' pid=%lu style=0x%08lX ex=0x%08lX "
                  "rect=[%ld,%ld,%ld,%ld]",
                  why ? why : "?", (void*)fg, (void*)raw, narrow(std::wstring(cls)).c_str(),
                  narrow(std::wstring(title)).c_str(), pid, (unsigned long)style, (unsigned long)ex, wr.left,
                  wr.top, wr.right, wr.bottom);
}

bool PauseMonitor::exclusiveFullscreen() {
    QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&quns))) return false;
    if (quns == QUNS_RUNNING_D3D_FULL_SCREEN) {
        std::snprintf(m_detail, sizeof(m_detail), "quns=%d (D3D exclusive)", (int)quns);
        return true;
    }
    if (quns == QUNS_PRESENTATION_MODE) {
        std::snprintf(m_detail, sizeof(m_detail), "quns=%d (presentation)", (int)quns);
        return true;
    }
    return false;
}

bool PauseMonitor::borderlessMonitor(HWND fg) {
    if (!fg || skipWindow(fg)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const LONG style = GetWindowLongW(fg, GWL_STYLE);
    const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
    const bool noCaption = (style & WS_CAPTION) == 0;
    const bool topmost = (ex & WS_EX_TOPMOST) != 0;
    if (!(noCaption || topmost)) return false;
    if (!coversRect(fg, mi.rcMonitor, 0.97)) return false;
    describe(fg, GetForegroundWindow(), "borderless-monitor");
    return true;
}

bool PauseMonitor::maximizedWork(HWND fg) {
    if (!fg || skipWindow(fg)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    if (IsZoomed(fg) && coversRect(fg, mi.rcWork, 0.90)) {
        describe(fg, GetForegroundWindow(), "zoomed-work");
        return true;
    }
    const LONG style = GetWindowLongW(fg, GWL_STYLE);
    const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
    const bool noCaption = (style & WS_CAPTION) == 0;
    const bool topmost = (ex & WS_EX_TOPMOST) != 0;
    if ((noCaption || topmost) && coversRect(fg, mi.rcWork, 0.92) && !coversRect(fg, mi.rcMonitor, 0.97)) {
        describe(fg, GetForegroundWindow(), "borderless-work");
        return true;
    }
    return false;
}

PauseMonitor::Cause PauseMonitor::rawCause() {
    m_detail[0] = 0;
    if (m_locked) {
        std::snprintf(m_detail, sizeof(m_detail), "session locked");
        return Cause::Locked;
    }
    if (m_pauseBattery && batterySaverOn()) {
        std::snprintf(m_detail, sizeof(m_detail), "battery saver");
        return Cause::Battery;
    }
    HWND fg = foregroundRoot();
    HWND raw = GetForegroundWindow();
    if (fg && (skipWindow(fg) || skipWindow(raw) || classIs(fg, L"Progman") || classIs(fg, L"WorkerW"))) {
        fg = nullptr;
    }
    if (m_pauseFullscreen) {
        if (exclusiveFullscreen()) return Cause::Fullscreen;
        if (fg && borderlessMonitor(fg)) return Cause::Fullscreen;
    }
    if (m_pauseCovered) {
        int monitors = 0;
        EnumDisplayMonitors(
            nullptr, nullptr, [](HMONITOR, HDC, LPRECT, LPARAM lp) -> BOOL {
                *reinterpret_cast<int*>(lp) += 1;
                return TRUE;
            },
            (LPARAM)&monitors);
        if (fg && monitors <= 1 && maximizedWork(fg)) return Cause::Maximized;
        if (allMonitorsCovered()) {
            std::snprintf(m_detail, sizeof(m_detail), "all monitors covered");
            return Cause::Covered;
        }
    }
    return Cause::None;
}

PauseMonitor::Cause PauseMonitor::evaluate() {
    const ULONGLONG now = GetTickCount64();
    if (now - m_lastCheck < 250) return m_latched;
    m_lastCheck = now;

    const Cause raw = rawCause();
    const bool exclusive = raw == Cause::Fullscreen && (strncmp(m_detail, "quns=", 5) == 0);

    if (raw == m_latched) {
        m_enterStreak = 0;
        m_leaveStreak = 0;
        m_pending = raw;
        return m_latched;
    }

    if (raw != Cause::None) {
        m_leaveStreak = 0;
        if (raw != m_pending) {
            m_pending = raw;
            m_enterStreak = 1;
        } else {
            m_enterStreak++;
        }
        // Exclusive fullscreen latches immediately; everything else needs two hits (~500 ms).
        if (exclusive || m_enterStreak >= 2) {
            m_latched = raw;
            m_enterStreak = 0;
        }
        return m_latched;
    }

    m_enterStreak = 0;
    m_pending = Cause::None;
    m_leaveStreak++;
    // Stay paused until the condition has been gone for ~750 ms so Chrome focus flaps don't twitch.
    if (m_leaveStreak >= 3) {
        m_latched = Cause::None;
        m_leaveStreak = 0;
        m_detail[0] = 0;
    }
    return m_latched;
}

bool PauseMonitor::allMonitorsCovered() const {
    struct Ctx {
        bool anyVisible = false;
    } ctx;

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(mon, &mi)) return TRUE;

            struct Hit {
                RECT work{};
                bool covered = false;
            } hit;
            hit.work = mi.rcWork;

            EnumWindows(
                [](HWND hwnd, LPARAM inner) -> BOOL {
                    auto* h = reinterpret_cast<Hit*>(inner);
                    if (skipWindow(hwnd)) return TRUE;
                    if (coversRect(hwnd, h->work, 0.90) || (IsZoomed(hwnd) && coversRect(hwnd, h->work, 0.85))) {
                        h->covered = true;
                        return FALSE;
                    }
                    return TRUE;
                },
                (LPARAM)&hit);

            if (!hit.covered) c->anyVisible = true;
            return TRUE;
        },
        (LPARAM)&ctx);

    return !ctx.anyVisible;
}

bool PauseMonitor::batterySaverOn() const {
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) return false;
    return (sps.SystemStatusFlag & 1) != 0;
}
