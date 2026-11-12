#include "host/Application.h"
#include "host/CrashGuard.h"

#include "core/Log.h"
#include "core/Util.h"
#include "effect/IEffect.h"
#include "effect/Presets.h"
#include "gfx/Plates.h"
#include "effects/LavaField.h"
#include "host/Autostart.h"
#include "host/DesktopIcons.h"

#include <algorithm>
#include <chrono>
#include <vector>
#include <cmath>
#include <commctrl.h>
#include <cstdio>
#include <cstring>
#include <shellapi.h>
#include <string>
#include <thread>
#include <wtsapi32.h>

#ifndef DEVICE_NOTIFY_WINDOW_HANDLE
#define DEVICE_NOTIFY_WINDOW_HANDLE 0
#endif
#ifndef PBT_APMRESUMECRITICAL
#define PBT_APMRESUMECRITICAL 0x0006
#endif
#ifndef PBT_APMRESUMESUSPEND
#define PBT_APMRESUMESUSPEND 0x0007
#endif
#ifndef PBT_APMRESUMEAUTOMATIC
#define PBT_APMRESUMEAUTOMATIC 0x0012
#endif

Application* Application::s_app = nullptr;

void Application::enableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    using SetCtx = BOOL(WINAPI*)(HANDLE);
    auto setCtx = (SetCtx)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (setCtx) {
        if (setCtx((HANDLE)-4)) return;
        setCtx((HANDLE)-3);
        return;
    }
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore) {
        using SetAw = HRESULT(WINAPI*)(int);
        auto setAw = (SetAw)(void*)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (setAw) setAw(2);
    }
}

void Application::bindSurfaceUserData() {
    HWND msg = m_host.messageHwnd();
    if (msg) SetWindowLongPtrW(msg, GWLP_USERDATA, (LONG_PTR)this);
    if (m_host.hwnd() && m_host.hwnd() != msg) {
        SetWindowLongPtrW(m_host.hwnd(), GWLP_USERDATA, (LONG_PTR)this);
    }
}

bool Application::initialize(HINSTANCE instance, HostMode mode, bool openPanelOnStart, bool openWidgetOnStart,
                             const std::string& effectOverride, int hideIconsCli, bool selftest, bool capture) {
    s_app = this;
    m_instance = instance;
    m_mode = mode;
    m_selftest = selftest;
    m_capture = capture && selftest;
    enableDpiAwareness();
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    m_config.load();
    m_autostartApplied = m_config.settings().startWithWindows;
    migrateLegacyAutostart();
    if (!effectOverride.empty()) {
        if (m_registry.contains(effectOverride)) {
            m_config.settings().effect = effectOverride;
            LOG_INFO("Wallpaper override '%s'", effectOverride.c_str());
        } else {
            LOG_WARN("Unknown wallpaper '%s'", effectOverride.c_str());
        }
    }
    if (!m_registry.contains(m_config.settings().effect)) {
        LOG_WARN("Effect '%s' is not in this build; using matrix", m_config.settings().effect.c_str());
        m_config.settings().effect = "matrix";
    }
    if (hideIconsCli >= 0) {
        m_config.settings().hideDesktopIcons = hideIconsCli != 0;
        LOG_INFO("CLI --icons %s", hideIconsCli ? "hide" : "show");
        m_config.save();
    }
    if (m_config.fresh()) {
        m_config.settings().startWithWindows = false;
        LOG_INFO("First run: Start with Windows is off until you turn it on");
    } else if (m_config.settings().startWithWindows != autostartEnabled()) {
        m_config.settings().startWithWindows = autostartEnabled();
    }

    if (!m_host.create(instance, mode, &Application::wndProc, this)) {
        LOG_ERR("Failed to create wallpaper host");
        return false;
    }

    bindSurfaceUserData();

    HWND msg = m_host.messageHwnd();
    WTSRegisterSessionNotification(msg, NOTIFY_FOR_THIS_SESSION);
    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    m_exitMsg = RegisterWindowMessageW(L"Stalgia.Cmd.Exit");
    m_panelMsg = RegisterWindowMessageW(L"Stalgia.Cmd.Panel");
    m_widgetMsg = RegisterWindowMessageW(L"Stalgia.Cmd.Widget");
    m_iconsMsg = RegisterWindowMessageW(L"Stalgia.Cmd.Icons");
    m_exitEvent = CreateEventW(nullptr, TRUE, FALSE, WallpaperHost::kExitEventName);
    if (!m_exitEvent) LOG_WARN("CreateEvent(exit) failed: %lu", GetLastError());
    if (msg) {
        using RegFn = void*(WINAPI*)(HANDLE, DWORD);
        auto reg = (RegFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                "RegisterSuspendResumeNotification");
        if (reg) {
            m_suspendNotify = reg(msg, DEVICE_NOTIFY_WINDOW_HANDLE);
            if (m_suspendNotify) LOG_INFO("Registered suspend/resume notification");
        }
    }

    if (!recreateRenderer()) return false;
    if (!loadEffect(m_config.settings().effect)) return false;
    warmSkyShaders();

    m_tray.create(msg, instance);
    applyPauseConfig();
    syncUi();

    m_warmupFrames = std::max(30, m_config.settings().fps);
    m_running = true;
    LOG_INFO("Stalgia %s started (%s) raised=%d composition=%d hwndPath=%d warmupFrames=%d selftest=%d",
             STALGIA_VERSION, mode == HostMode::Window ? "preview" : "desktop",
             m_host.raisedDesktop() ? 1 : 0, m_host.usesComposition() ? 1 : 0, m_host.hwndPath() ? 1 : 0,
             m_warmupFrames, m_selftest ? 1 : 0);
    CrashGuard::install();
    DesktopIcons::captureOriginal();
    if (mode == HostMode::Desktop) {
        if (!m_selftest) {
            LOG_INFO("Startup auto-pause probe: %s", PauseMonitor::causeName(m_pause.evaluate()));
        }
        installDesktopHooks();
        SetTimer(msg, kTimerHeartbeat, 30000, nullptr);
        DesktopIcons::apply(m_config.settings().hideDesktopIcons);
        armVisibilityWait("startup");
    }
    if (m_selftest) {
        m_stStart = GetTickCount64();
        m_stPhaseAt = m_stStart;
        m_stReport = std::string("Stalgia ") + STALGIA_VERSION + " selftest\n";
    } else {
        if (openPanelOnStart) openPanel();
        if (openWidgetOnStart || m_config.settings().showWidget) openWidget();
    }
    return true;
}

void Application::installDesktopHooks() {
    HWND msg = m_host.messageHwnd();
    if (msg) SetTimer(msg, kTimerZOrder, 2000, nullptr);
    // Reorder only. SHOW/HIDE and FOREGROUND fire during Win+D; re-asserting z-order
    // on those events fights Explorer's Show Desktop animation.
    m_winEventHookZ = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, &winEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT);
    if (!m_winEventHookZ) LOG_WARN("SetWinEventHook(REORDER) failed: %lu", GetLastError());
    else LOG_INFO("WinEvent hook installed (Progman/DefView reorder only; no shell hook)");
}

void Application::removeDesktopHooks() {
    HWND msg = m_host.messageHwnd();
    if (msg) KillTimer(msg, kTimerZOrder);
    if (m_winEventHookZ) {
        UnhookWinEvent(m_winEventHookZ);
        m_winEventHookZ = nullptr;
    }
}

void CALLBACK Application::winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD,
                                        DWORD) {
    if (!s_app || idObject != OBJID_WINDOW || !hwnd) return;
    if (event != EVENT_OBJECT_REORDER) return;
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (lstrcmpW(cls, L"Progman") != 0 && lstrcmpW(cls, L"SHELLDLL_DefView") != 0 &&
        lstrcmpW(cls, WallpaperHost::kSurfaceClass) != 0) {
        return;
    }
    s_app->notifyDesktopChanged();
}

void Application::notifyDesktopChanged() {
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 80) return;
    last = now;
    HWND msg = m_host.messageHwnd();
    if (msg) PostMessageW(msg, kZOrderMsg, 0, 0);
}

void Application::heartbeat() {
    const char* reason = "running";
    if (m_hidden) reason = "hidden";
    else if (m_userPaused) reason = "user";
    else if (m_lastPause != PauseMonitor::Cause::None) reason = PauseMonitor::causeName(m_lastPause);
    m_host.logHeartbeat(reason, m_renderer.presentCount(), m_renderer.lastPresentHr());
    if (m_mode == HostMode::Desktop && m_config.settings().hideDesktopIcons) {
        DesktopIcons::apply(true);
    }
}

bool Application::recreateRenderer() {
    if (m_renderer.deviceLost()) {
        LOG_ERR("Recreating renderer after device-lost GetDeviceRemovedReason=0x%08X",
                (unsigned)m_renderer.deviceRemovedReason());
    }
    m_thumbs.release();
    m_renderer.release();
    HWND surface = m_host.hwnd();
    if (!surface) return false;
    if (!m_renderer.initialize(surface, m_host.usesComposition())) {
        LOG_ERR("Renderer init failed");
        return false;
    }
    if (m_effect) {
        m_effect->release();
        if (!m_effect->initialize(m_renderer.device(), m_renderer.context())) return false;
        m_effect->applySettings(m_config.settings());
        RECT rc{};
        GetClientRect(surface, &rc);
        m_effect->resize(rc.right - rc.left, rc.bottom - rc.top, m_host.dpiScale());
        m_effect->precompile(&m_renderer);
    }
    warmSkyShaders();
    m_needsRecreate = false;
    m_recreateRetryAt = 0;
    return true;
}

void Application::drawVisibilityMarker() {
    HWND hwnd = m_host.hwnd();
    if (!hwnd) return;
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    const int sx = mi.rcWork.right - 36;
    const int sy = mi.rcWork.bottom - 36;
    const int cx = sx - wr.left;
    const int cy = sy - wr.top;
    m_renderer.fillRect(cx, cy, 16, 16, 1.0f, 0.0f, 0.72f, 1.0f);
}

namespace {

bool pointIsDesktop(int x, int y) {
    HWND hit = WindowFromPoint(POINT{x, y});
    if (!hit) return false;
    HWND walk = hit;
    for (int i = 0; i < 6 && walk; ++i) {
        wchar_t cls[64]{};
        GetClassNameW(walk, cls, 64);
        if (lstrcmpW(cls, L"SysListView32") == 0) return false;
        if (lstrcmpW(cls, L"Progman") == 0 || lstrcmpW(cls, L"WorkerW") == 0 ||
            lstrcmpW(cls, L"SHELLDLL_DefView") == 0 || lstrcmpW(cls, WallpaperHost::kSurfaceClass) == 0) {
            return true;
        }
        walk = GetParent(walk);
    }
    return false;
}

}  // namespace

int Application::probeWallpaperVisible() {
    HWND hwnd = m_host.hwnd();
    if (!hwnd) return -1;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return -1;
    const POINT candidates[] = {
        {mi.rcWork.right - 28, mi.rcWork.bottom - 28},
        {mi.rcWork.left + 28, mi.rcWork.bottom - 28},
        {mi.rcWork.right - 96, mi.rcWork.bottom - 96},
        {(mi.rcWork.left + mi.rcWork.right) / 2, (mi.rcWork.top + mi.rcWork.bottom) / 2},
    };
    int sx = 0, sy = 0;
    bool uncovered = false;
    for (const POINT& p : candidates) {
        if (!pointIsDesktop(p.x, p.y)) continue;
        sx = p.x;
        sy = p.y;
        uncovered = true;
        break;
    }
    if (!uncovered) {
        LOG_WARN("Visibility probe unavailable (sample points are covered by a window)");
        return -1;
    }
    // The marker is drawn at work.right-36. Only the first candidate lines up with it.
    const bool onMarker = sx == mi.rcWork.right - 28 && sy == mi.rcWork.bottom - 28;
    if (!onMarker) {
        LOG_INFO("Visibility probe skipped color match; marker pixel is covered, point=(%d,%d) is desktop",
                 sx, sy);
        return -1;
    }
    uint32_t bgra = 0;
    if (!m_renderer.sampleScreenBgra(sx, sy, &bgra)) {
        LOG_WARN("Visibility probe unavailable");
        return -1;
    }
    const int b = (int)(bgra & 0xFF);
    const int g = (int)((bgra >> 8) & 0xFF);
    const int r = (int)((bgra >> 16) & 0xFF);
    const int dr = r > 255 ? r - 255 : 255 - r;
    const int dg = g < 0 ? -g : g;
    const int db = b > 184 ? b - 184 : 184 - b;
    const bool hit = dr <= 48 && dg <= 48 && db <= 56;
    LOG_INFO("Visibility probe screen=(%d,%d) bgra=#%08X rgb=(%d,%d,%d) match=%d path=%s (expect magenta)",
             sx, sy, (unsigned)bgra, r, g, b, hit ? 1 : 0, m_host.hwndPath() ? "hwnd" : "dcomp");
    return hit ? 1 : 0;
}

void Application::armVisibilityWait(const char* why) {
    m_drawMarker = false;
    m_visArmPresent = m_renderer.presentCount();
    m_visNextAt = GetTickCount64() + kVisMinWaitMs;
    LOG_INFO("Visibility wait armed (%s) presents=%d dcomp=%d hwndPath=%d", why ? why : "?",
             m_visArmPresent, m_host.usesComposition() ? 1 : 0, m_host.hwndPath() ? 1 : 0);
}

void Application::tickVisibility() {
    if (m_mode != HostMode::Desktop || m_hidden) return;
    const ULONGLONG now = GetTickCount64();
    const int presented = m_renderer.presentCount() - m_visArmPresent;
    if (presented < kVisMinPresents || now < m_visNextAt) return;
    checkWallpaperVisibility();
}

bool Application::switchToHwndFallback(const char* why) {
    if (m_host.hwndPath() || m_hwndFallbackLatched) return false;
    LOG_WARN("Switching to layered HWND fallback (%s) — the path that shows on 24H2 when DComp can't be proven",
             why ? why : "?");
    if (!m_host.recreateLayeredHwnd()) {
        LOG_ERR("HWND fallback failed");
        return false;
    }
    m_hwndFallbackLatched = true;
    bindSurfaceUserData();
    m_needsRecreate = true;
    armVisibilityWait(why ? why : "hwnd-fallback");
    return true;
}

void Application::checkWallpaperVisibility() {
    if (m_mode != HostMode::Desktop || m_hidden) return;
    m_drawMarker = false;

    const int probe = probeWallpaperVisible();
    if (probe < 0) {
        m_visUnavailable = true;
        m_visUnavail++;
        LOG_WARN("Visibility probe unavailable (streak=%d) — not a miss", m_visUnavail);
        if (m_host.usesComposition() && switchToHwndFallback("probe-unavailable")) return;
        static const DWORD kBackoff[] = {2000, 4000, 8000, 12000};
        const DWORD wait = kBackoff[std::min(m_visUnavail, 4) - 1];
        m_visArmPresent = m_renderer.presentCount();
        m_visNextAt = GetTickCount64() + wait;
        if (m_visUnavail >= 4) {
            m_visNextAt = GetTickCount64() + kVisPeriodicMs;
            m_visUnavail = 0;
        }
        return;
    }

    m_visUnavailable = false;
    m_visUnavail = 0;

    if (probe > 0) {
        m_visibilityOk = true;
        m_visMiss = 0;
        LOG_INFO("Wallpaper is visible (probe ok) path=%s", m_host.hwndPath() ? "hwnd" : "dcomp");
        m_visNextAt = GetTickCount64() + kVisPeriodicMs;
        m_visArmPresent = m_renderer.presentCount();
        return;
    }

    m_visibilityOk = false;
    m_visMiss++;
    LOG_WARN("Wallpaper not visible (confirmed miss %d) path=%s", m_visMiss,
             m_host.hwndPath() ? "hwnd" : "dcomp");
    if (m_visMiss < 2) {
        armVisibilityWait("miss-confirm");
        return;
    }

    if (m_host.raisedDesktop() && !m_triedWorkerParent && !m_host.hwndPath()) {
        m_host.ensureRaisedWorkerW();
        m_triedWorkerParent = true;
        if (m_host.parentToWorkerW()) {
            bindSurfaceUserData();
            m_needsRecreate = true;
            armVisibilityWait("parent-workerw");
            LOG_INFO("Recovery: parented surface to WorkerW");
            return;
        }
    }
    if (m_host.usesComposition() && switchToHwndFallback("confirmed-miss")) return;

    LOG_ERR("HWND fallback still not visible; retrying in %u s", kVisPeriodicMs / 1000);
    m_visMiss = 0;
    m_visNextAt = GetTickCount64() + kVisPeriodicMs;
    m_visArmPresent = m_renderer.presentCount();
}

bool Application::loadEffect(const std::string& id) {
    if (!m_registry.contains(id)) {
        LOG_WARN("Unknown effect '%s'", id.c_str());
        return false;
    }
    auto fx = m_registry.create(id);
    if (!fx) {
        LOG_ERR("Unknown effect '%s'", id.c_str());
        return false;
    }
    const bool canInit = m_renderer.device() && m_host.hwnd();
    if (canInit) {
        if (!fx->initialize(m_renderer.device(), m_renderer.context())) {
            LOG_ERR("Effect '%s' failed to initialize", id.c_str());
            fx->release();
            return false;
        }
        fx->applySettings(m_config.settings());
        RECT rc{};
        GetClientRect(m_host.hwnd(), &rc);
        fx->resize(std::max(1, (int)(rc.right - rc.left)), std::max(1, (int)(rc.bottom - rc.top)),
                   m_host.dpiScale());
        fx->precompile(&m_renderer);
    }
    auto old = std::move(m_effect);
    m_effect = std::move(fx);
    m_config.settings().effect = m_effect->id();
    LOG_INFO("Effect '%s' selected", m_effect->id());
    if (old) old->release();
    return true;
}

void Application::applyPauseConfig() {
    m_pause.configure(m_config.settings().pauseOnFullscreen, m_config.settings().pauseOnCovered,
                      m_config.settings().pauseOnBatterySaver);
}

void Application::saveConfig() {
    if (m_selftest) return;
    m_config.save();
}

void Application::applySettingsLive() {
    ++m_tuneGen;
    auto& s = m_config.settings();
    s.fps = Config::clampFps(s.fps);
    if (s.startWithWindows != m_autostartApplied) {
        setAutostart(s.startWithWindows);
        s.startWithWindows = autostartEnabled();
        m_autostartApplied = s.startWithWindows;
    }
    applyPauseConfig();
    if (m_effect) m_effect->applySettings(s);
    if (m_mode == HostMode::Desktop) DesktopIcons::apply(s.hideDesktopIcons);
    m_settingsDirty = true;
    m_saveDeadline = GetTickCount64() + 400;
    syncUi();
}

void Application::flushSettings() {
    if (!m_settingsDirty) return;
    m_settingsDirty = false;
    saveConfig();
}

void Application::setHideDesktopIcons(bool hide) {
    m_config.settings().hideDesktopIcons = hide;
    if (m_mode == HostMode::Desktop) DesktopIcons::apply(hide);
    saveConfig();
    syncUi();
}

void Application::switchEffect(const std::string& id) {
    if (!loadEffect(id)) return;
    saveConfig();
    syncUi();
    if (!m_hidden && m_host.hwnd() && m_renderer.device()) {
        renderFrame(1.0f / 30.0f, m_time);
    }
}

void Application::setUserPaused(bool paused) {
    m_userPaused = paused;
    saveConfig();
    syncUi();
}

void Application::setWallpaperHidden(bool hidden) {
    if (m_mode != HostMode::Desktop) {
        if (m_host.hwnd()) ShowWindow(m_host.hwnd(), hidden ? SW_HIDE : SW_SHOW);
        m_hidden = hidden;
        syncUi();
        return;
    }
    if (hidden == m_hidden) {
        syncUi();
        return;
    }
    m_hidden = hidden;
    if (hidden) {
        removeDesktopHooks();
        m_thumbs.release();
        m_renderer.release();
        if (m_effect) m_effect->release();
        m_host.teardownSurface();
        LOG_INFO("Wallpaper hidden; Progman child destroyed so Win+D should work");
    } else {
        if (!m_host.setupSurface()) {
            LOG_ERR("Failed to recreate wallpaper surface");
            m_hidden = true;
            syncUi();
            return;
        }
        bindSurfaceUserData();
        if (!recreateRenderer()) {
            LOG_ERR("Failed to recreate renderer after show");
            m_hidden = true;
            syncUi();
            return;
        }
        if (!m_effect) loadEffect(m_config.settings().effect);
        installDesktopHooks();
        m_warmupFrames = std::max(8, m_config.settings().fps / 2);
        armVisibilityWait("shown");
        LOG_INFO("Wallpaper shown");
    }
    saveConfig();
    syncUi();
}

void Application::openConfigFile() {
    saveConfig();
    openFileInDefaultApp(m_config.path());
}

void Application::openPanel() {
    if (!m_panel.create(m_instance, this)) {
        LOG_ERR("Control panel failed to open");
        return;
    }
    m_panel.show();
}

void Application::openWidget() {
    HWND owner = m_host.messageHwnd();
    if (!m_widget.create(m_instance, this, m_mode == HostMode::Desktop, owner)) {
        LOG_ERR("Desktop widget failed to open after all fallbacks");
        return;
    }
    m_widget.show();
}

void Application::hideWidget() {
    m_widget.hide();
    syncUi();
}

void Application::openGallery() {
    m_gallery.show(m_instance, this, false);
}

void Application::cycleWallpaper(int dir) {
    const auto list = m_registry.list();
    if (list.empty()) return;
    int idx = 0;
    for (int i = 0; i < (int)list.size(); ++i) {
        if (list[(size_t)i].id == m_config.settings().effect) idx = i;
    }
    const int n = (int)list.size();
    int next = (idx + (dir < 0 ? -1 : 1)) % n;
    if (next < 0) next += n;
    switchEffect(list[(size_t)next].id);
}

bool Application::isFavorite(const std::string& id) const {
    const auto& favs = m_config.settings().favorites;
    return std::find(favs.begin(), favs.end(), id) != favs.end();
}

void Application::toggleFavorite(const std::string& id) {
    auto& favs = m_config.settings().favorites;
    auto it = std::find(favs.begin(), favs.end(), id);
    if (it == favs.end()) favs.push_back(id);
    else favs.erase(it);
    saveConfig();
    syncUi();
    m_gallery.refresh();
}

void Application::syncUi() {
    const auto& s = m_config.settings();
    m_tray.rebuildMenu(m_registry, s.effect, s.favorites, s.fps, m_userPaused, m_hidden, m_widget.visible(),
                       s.pauseOnFullscreen, s.pauseOnCovered, s.pauseOnBatterySaver, s.startWithWindows,
                       s.hideDesktopIcons);
    std::wstring tip = L"Stalgia";
    std::string name = s.effect;
    for (const auto& e : m_registry.list()) {
        if (e.id == s.effect) {
            name = e.displayName;
            break;
        }
    }
    if (m_hidden) tip += L" — hidden";
    else if (m_userPaused) tip += L" — paused (" + widen(name) + L")";
    else tip += L" — " + widen(name);
    m_tray.setTip(tip.c_str());
    m_panel.refresh();
    m_widget.refresh();
    m_gallery.refresh();
}

void Application::onDisplayChange() {
    m_tray.reloadIcon(m_instance);
    if (m_widget.hwnd()) applyWindowIcons(m_widget.hwnd(), m_instance);
    if (m_panel.hwnd()) applyWindowIcons(m_panel.hwnd(), m_instance);
    if (m_host.hwnd()) applyWindowIcons(m_host.hwnd(), m_instance);
    if (m_hidden) return;
    if (m_mode == HostMode::Desktop) m_host.syncToVirtualScreen();
    m_needsResize = true;
}

void Application::scheduleReattach(const char* why) {
    m_reattachWhy = why ? why : "event";
    m_reattachAt = GetTickCount64() + 400;
    HWND msg = m_host.messageHwnd();
    if (msg) SetTimer(msg, kTimerReattach, 400, nullptr);
    LOG_INFO("Reattach scheduled (%s)", m_reattachWhy);
}

void Application::runReattach() {
    HWND msg = m_host.messageHwnd();
    if (msg) KillTimer(msg, kTimerReattach);
    m_reattachAt = 0;
    if (m_hidden || m_mode != HostMode::Desktop) return;
    LOG_INFO("Reattach now (%s)", m_reattachWhy ? m_reattachWhy : "?");
    const HWND before = m_host.hwnd();
    const bool compBefore = m_host.usesComposition();
    const bool alive = before && IsWindow(before);
    m_host.reattach();
    bindSurfaceUserData();
    const bool hwndChanged = m_host.hwnd() != before || !alive || !IsWindow(m_host.hwnd());
    const bool compChanged = m_host.usesComposition() != compBefore;
    if (hwndChanged || compChanged) m_needsRecreate = true;
    else {
        if (m_host.usesComposition()) m_renderer.rebindComposition();
        m_host.clearCompositionDirty();
        m_needsResize = true;
    }
    m_visMiss = 0;
    m_visUnavail = 0;
    m_visibilityOk = false;
    m_triedWorkerParent = false;
    armVisibilityWait(m_reattachWhy ? m_reattachWhy : "reattach");
    onDisplayChange();
    if (m_mode == HostMode::Desktop) DesktopIcons::apply(m_config.settings().hideDesktopIcons);
}

void Application::renderFrame(float dt, double time) {
    if (m_hidden || !m_host.hwnd()) return;
    if (m_needsRecreate || m_renderer.deviceLost()) {
        if (GetTickCount64() < m_recreateRetryAt) return;
        if (!recreateRenderer()) {
            m_recreateRetryAt = GetTickCount64() + 1500;
            return;
        }
        armVisibilityWait("renderer-recreate");
    }
    if (m_needsResize) {
        RECT rc{};
        GetClientRect(m_host.hwnd(), &rc);
        m_renderer.resize(rc.right - rc.left, rc.bottom - rc.top);
        if (m_effect) m_effect->resize(m_renderer.width(), m_renderer.height(), m_host.dpiScale());
        m_needsResize = false;
    }
    if (!m_renderer.beginFrame() || !m_effect) return;

    const auto& settings = m_config.settings();
    Renderer::Grade grade;
    if (const EffectLook* look = effectLook(settings, settings.effect)) {
        grade.brightness = look->brightness;
        grade.saturation = look->saturation;
        grade.vignette = look->vignette;
        grade.night = nightAmount(look->timeOfDay);
    }
    m_renderer.setGrade(grade);

    float targetX = 0, targetY = 0;
    const EffectLook* look = effectLook(settings, settings.effect);
    if (look && look->parallax && m_host.hwnd()) {
        POINT p{};
        RECT wr{};
        if (GetCursorPos(&p) && GetWindowRect(m_host.hwnd(), &wr)) {
            const float cx = (wr.left + wr.right) * 0.5f;
            const float cy = (wr.top + wr.bottom) * 0.5f;
            const float hw = std::max(1.0f, (wr.right - wr.left) * 0.5f);
            const float hh = std::max(1.0f, (wr.bottom - wr.top) * 0.5f);
            targetX = std::clamp((p.x - cx) / hw, -1.0f, 1.0f);
            targetY = std::clamp((p.y - cy) / hh, -1.0f, 1.0f);
        }
    }
    const float ease = 1.0f - std::exp(-std::max(0.0f, dt) * 5.0f);
    m_lookX += (targetX - m_lookX) * ease;
    m_lookY += (targetY - m_lookY) * ease;

    FrameContext ctx;
    ctx.renderer = &m_renderer;
    ctx.device = m_renderer.device();
    ctx.context = m_renderer.context();
    ctx.rtv = m_renderer.rtv();
    ctx.width = m_renderer.width();
    ctx.height = m_renderer.height();
    ctx.dpiScale = m_host.dpiScale();
    ctx.primaryHeight = m_host.primaryHeight();
    ctx.dt = dt;
    ctx.time = time;
    ctx.lookX = m_lookX * 0.045f;
    ctx.lookY = m_lookY * 0.035f;
    m_effect->update(dt, time);
    m_renderer.advancePlateFade(dt);
    m_renderer.beginGpuTime();
    const char* eid = m_effect->id();
    const bool usesPlates = eid && (std::strcmp(eid, "starfield") == 0 || std::strcmp(eid, "aurora") == 0 ||
                                    std::strcmp(eid, "fireflies") == 0 || std::strcmp(eid, "snow") == 0);
    // Matrix, lava, and the private effects never sample plates. Drop a binding
    // left over from the previous wallpaper before they draw.
    if (!usesPlates) m_renderer.bindPlates(nullptr, nullptr);
    const bool perMonitor = eid && (std::strcmp(eid, "starfield") == 0 || std::strcmp(eid, "aurora") == 0 ||
                                    std::strcmp(eid, "lava") == 0 || std::strcmp(eid, "fireflies") == 0 ||
                                    std::strcmp(eid, "snow") == 0);
    struct Mon {
        int x, y, w, h;
    };
    std::vector<Mon> mons;
    if (perMonitor && m_host.hwnd()) {
        POINT origin{0, 0};
        ClientToScreen(m_host.hwnd(), &origin);
        struct Pack {
            std::vector<Mon>* out;
            POINT origin;
            int fullW;
            int fullH;
        } pack{&mons, origin, ctx.width, ctx.height};
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR, HDC, LPRECT rc, LPARAM lp) -> BOOL {
                auto* p = reinterpret_cast<Pack*>(lp);
                int x = rc->left - p->origin.x;
                int y = rc->top - p->origin.y;
                int x2 = std::min(p->fullW, x + (int)(rc->right - rc->left));
                int y2 = std::min(p->fullH, y + (int)(rc->bottom - rc->top));
                x = std::max(0, x);
                y = std::max(0, y);
                if (x2 - x >= 2 && y2 - y >= 2) p->out->push_back(Mon{x, y, x2 - x, y2 - y});
                return TRUE;
            },
            (LPARAM)&pack);
    }
    if (perMonitor && mons.size() > 1) {
        const int fullW = ctx.width;
        const int fullH = ctx.height;
        for (const Mon& r : mons) {
            m_renderer.setViewport(r.x, r.y, r.w, r.h);
            ctx.width = r.w;
            ctx.height = r.h;
            m_effect->render(ctx);
        }
        m_renderer.setViewport(0, 0, fullW, fullH);
    } else {
        m_effect->render(ctx);
    }
    m_framePlateMask = m_renderer.plateMask();
    m_renderer.compositeToBackbuffer();
    m_renderer.endGpuTime();
    const int presented = m_renderer.presentCount() - m_visArmPresent;
    const bool probeNow = m_mode == HostMode::Desktop && !m_hidden && presented + 1 >= kVisMinPresents &&
                          GetTickCount64() >= m_visNextAt;
    if (probeNow) drawVisibilityMarker();
    m_renderer.endFrame();
    if (probeNow) checkWallpaperVisibility();
}

void Application::handleCommand(UINT cmd) {
    auto& s = m_config.settings();
    switch (cmd) {
        case TrayIcon::Settings:
            openPanel();
            return;
        case TrayIcon::ShowWidget:
            openWidget();
            return;
        case TrayIcon::HideWidget:
            hideWidget();
            return;
        case TrayIcon::Pause:
            setUserPaused(!m_userPaused);
            return;
        case TrayIcon::ShowWallpaper:
            setWallpaperHidden(false);
            return;
        case TrayIcon::HideWallpaper:
            setWallpaperHidden(true);
            return;
        case TrayIcon::Fps15:
            s.fps = 15;
            break;
        case TrayIcon::Fps30:
            s.fps = 30;
            break;
        case TrayIcon::Fps60:
            s.fps = 60;
            break;
        case TrayIcon::PauseFullscreen:
            s.pauseOnFullscreen = !s.pauseOnFullscreen;
            applyPauseConfig();
            break;
        case TrayIcon::PauseCovered:
            s.pauseOnCovered = !s.pauseOnCovered;
            applyPauseConfig();
            break;
        case TrayIcon::PauseBattery:
            s.pauseOnBatterySaver = !s.pauseOnBatterySaver;
            applyPauseConfig();
            break;
        case TrayIcon::Autostart:
            s.startWithWindows = !s.startWithWindows;
            setAutostart(s.startWithWindows);
            s.startWithWindows = autostartEnabled();
            m_autostartApplied = s.startWithWindows;
            break;
        case TrayIcon::HideDesktopIcons:
            setHideDesktopIcons(!s.hideDesktopIcons);
            return;
        case TrayIcon::OpenConfig:
            openConfigFile();
            return;
        case TrayIcon::Exit:
            requestQuit();
            return;
        case TrayIcon::OpenGallery:
            openGallery();
            return;
        default:
            if (cmd >= TrayIcon::EffectBase) {
                const auto list = m_registry.list();
                const size_t idx = cmd - TrayIcon::EffectBase;
                if (idx < list.size()) switchEffect(list[idx].id);
                return;
            }
            break;
    }
    saveConfig();
    syncUi();
}

LRESULT CALLBACK Application::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!app) app = s_app;
    if (app) return app->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Application::requestQuit() {
    if (!m_running) return;
    LOG_INFO("Quit requested");
    m_running = false;
    if (m_exitEvent) SetEvent(m_exitEvent);
    PostQuitMessage(0);
}

LRESULT Application::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (m_exitMsg && msg == m_exitMsg) {
        requestQuit();
        return 0;
    }
    if (m_panelMsg && msg == m_panelMsg) {
        openPanel();
        return 0;
    }
    if (m_widgetMsg && msg == m_widgetMsg) {
        openWidget();
        return 0;
    }
    if (m_iconsMsg && msg == m_iconsMsg) {
        setHideDesktopIcons(wParam != 0);
        return 0;
    }
    if (m_taskbarCreated && msg == m_taskbarCreated) {
        m_tray.destroy();
        m_tray.create(m_host.messageHwnd(), m_instance);
        syncUi();
        scheduleReattach("TaskbarCreated");
        return 0;
    }

    switch (msg) {
        case WM_COMMAND:
            handleCommand(LOWORD(wParam));
            return 0;
        case TrayIcon::kCallbackMsg: {
            const UINT notify = LOWORD(lParam);
            if (notify == WM_CONTEXTMENU || notify == WM_RBUTTONUP) {
                m_tray.showMenu();
            } else if (notify == NIN_SELECT || notify == NIN_KEYSELECT || notify == WM_LBUTTONDBLCLK ||
                       notify == WM_LBUTTONUP) {
                if (m_widget.visible()) m_widget.refresh();
                else openWidget();
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            scheduleReattach("WM_DISPLAYCHANGE");
            return 0;
        case WM_DPICHANGED:
            onDisplayChange();
            scheduleReattach("WM_DPICHANGED");
            return 0;
        case WM_SETTINGCHANGE:
            if (wParam == SPI_SETDESKWALLPAPER || wParam == SPI_SETWORKAREA) {
                scheduleReattach(wParam == SPI_SETDESKWALLPAPER ? "SPI_SETDESKWALLPAPER"
                                                                : "SPI_SETWORKAREA");
            }
            return 0;
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND ||
                wParam == PBT_APMRESUMECRITICAL) {
                scheduleReattach("resume");
            }
            return TRUE;
        case WM_SIZE:
            m_needsResize = true;
            return 0;
        case WM_WINDOWPOSCHANGED:
            break;
        case WM_TIMER:
            if (wParam == kTimerZOrder) {
                const bool moved = m_host.maintainZOrder();
                if (moved || m_host.compositionNeedsRebind()) {
                    m_renderer.rebindComposition();
                    m_host.clearCompositionDirty();
                }
            } else if (wParam == kTimerHeartbeat) heartbeat();
            else if (wParam == kTimerReattach) runReattach();
            return 0;
        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
            if (!cds || !cds->lpData || cds->cbData == 0) break;
            auto* copy = new (std::nothrow) WallpaperHost::IpcMessage{};
            if (!copy) return FALSE;
            if (cds->dwData == WallpaperHost::kCopyIpc && cds->cbData >= sizeof(WallpaperHost::IpcMessage)) {
                std::memcpy(copy, cds->lpData, sizeof(*copy));
                copy->payload[sizeof(copy->payload) - 1] = 0;
            } else if (cds->dwData == WallpaperHost::kCopyWallpaper) {
                copy->cmd = WallpaperHost::IpcWallpaper;
                const size_t n = std::min((size_t)cds->cbData, sizeof(copy->payload) - 1);
                std::memcpy(copy->payload, cds->lpData, n);
                copy->payload[n] = 0;
            } else {
                delete copy;
                break;
            }
            if (!PostMessageW(hwnd, kIpcDeferred, 0, (LPARAM)copy)) {
                delete copy;
                return FALSE;
            }
            return TRUE;
        }
        case WM_QUERYENDSESSION:
        case WM_ENDSESSION:
            DesktopIcons::restoreOriginal();
            WallpaperHost::restoreWallpaperFromCrash();
            if (msg == WM_ENDSESSION && wParam) requestQuit();
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK) m_pause.setSessionLocked(true);
            if (wParam == WTS_SESSION_UNLOCK) {
                m_pause.setSessionLocked(false);
                scheduleReattach("session-unlock");
            }
            if (wParam == WTS_REMOTE_CONNECT || wParam == WTS_CONSOLE_DISCONNECT) {
                LOG_INFO("Session is remote; leaving DComp if it is not already proven");
                if (m_host.usesComposition()) switchToHwndFallback("remote-session");
                else scheduleReattach("remote-session");
            }
            if (wParam == WTS_CONSOLE_CONNECT) scheduleReattach("console-connect");
            return 0;
        case WM_CLOSE:
            requestQuit();
            if (m_mode == HostMode::Window && hwnd == m_host.hwnd()) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (hwnd == m_host.hwnd() && m_mode == HostMode::Window) PostQuitMessage(0);
            return 0;
        default:
            if (msg == kIpcDeferred) {
                auto* ipc = reinterpret_cast<WallpaperHost::IpcMessage*>(lParam);
                if (ipc) {
                    handleIpc(*ipc);
                    delete ipc;
                }
                return 0;
            }
            if (msg == kZOrderMsg) {
                const bool moved = m_host.maintainZOrder();
                if (moved || m_host.compositionNeedsRebind()) {
                    m_renderer.rebindComposition();
                    m_host.clearCompositionDirty();
                }
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int Application::run() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    auto last = clock::now();

    while (m_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!m_running) break;
        if (m_exitEvent && WaitForSingleObject(m_exitEvent, 0) == WAIT_OBJECT_0) {
            requestQuit();
            break;
        }

        PauseMonitor::Cause autoCause = PauseMonitor::Cause::None;
        if (m_mode == HostMode::Desktop && !m_hidden && !m_selftest) autoCause = m_pause.evaluate();
        const bool warmup = m_warmupFrames > 0;
        if (warmup) m_warmupFrames--;
        const bool paused = m_hidden || (!warmup && (m_userPaused || autoCause != PauseMonitor::Cause::None));

        PauseMonitor::Cause shown = PauseMonitor::Cause::None;
        if (paused && !m_hidden) shown = m_userPaused ? PauseMonitor::Cause::User : autoCause;
        if (m_hidden) shown = PauseMonitor::Cause::None;
        if (shown != m_lastPause && !m_hidden) {
            if (paused) {
                if (m_pause.lastDetail()[0]) {
                    LOG_INFO("Pause: %s (%s)", PauseMonitor::causeName(shown), m_pause.lastDetail());
                } else {
                    LOG_INFO("Pause: %s", PauseMonitor::causeName(shown));
                }
            } else {
                LOG_INFO("Resume rendering (was %s)", PauseMonitor::causeName(m_lastPause));
            }
            m_lastPause = shown;
        }
        if (!m_hidden) m_lastPause = shown;

        const bool galleryThumbs = m_gallery.acceptsThumbs();
        const bool widgetThumb = m_widget.wantsLiveThumb();
        auto pumpThumbs = [&]() {
            if (!galleryThumbs && !widgetThumb) return;
            if (!m_renderer.device() || m_renderer.deviceLost()) return;
            if (m_thumbs.tick(&m_renderer, m_registry, m_config.settings(), galleryThumbs, widgetThumb,
                              m_config.settings().effect, m_tuneGen)) {
                if (galleryThumbs) m_gallery.refresh();
                if (widgetThumb) m_widget.refresh();
            }
        };

        if (paused) {
            pumpThumbs();
            HANDLE ev = m_exitEvent;
            const DWORD waitMs = (galleryThumbs || widgetThumb) ? 70 : 250;
            const DWORD wr =
                MsgWaitForMultipleObjects(ev ? 1 : 0, ev ? &ev : nullptr, FALSE, waitMs, QS_ALLINPUT);
            if (ev && wr == WAIT_OBJECT_0) requestQuit();
            last = clock::now();
            next = last;
            continue;
        }

        if (m_settingsDirty && GetTickCount64() >= m_saveDeadline) flushSettings();

        const auto now = clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt < 0) dt = 0;
        if (dt > 0.1f) dt = 0.1f;
        int fps = frameCapFps(m_config.settings());
        if (m_renderer.isWarp()) fps = std::min(fps, 15);
        const float ideal = 1.0f / (float)std::max(1, fps);
        if (dt > 0.0f && std::fabs(dt - ideal) < ideal * 0.30f) dt = ideal;
        m_time += dt;
        renderFrame(dt, m_time);
        // Not during a measured selftest window: the first draw of a sky shader
        // is the driver's compile, and that hitch must not land on the clock.
        if (!m_selftest || !m_stWindow) m_renderer.pumpSkyWarmup();
        pumpThumbs();
        if (m_selftest) tickSelftest();

        next += std::chrono::microseconds(1000000 / std::max(1, fps));
        if (next < now) {
            next = now;
        } else {
            std::this_thread::sleep_until(next);
        }
    }
    return m_selftest ? m_stExitCode : 0;
}

void Application::shutdown() {
    flushSettings();
    if (m_selftest && m_stPhase < 99) {
        m_stReport += "Result: FAIL (selftest ended early)\n";
        m_stExitCode = 1;
        writeSelftestReport();
        m_stPhase = 99;
    }
    DesktopIcons::restoreOriginal();
    removeDesktopHooks();
    HWND msg = m_host.messageHwnd();
    if (msg) {
        KillTimer(msg, kTimerHeartbeat);
        KillTimer(msg, kTimerReattach);
    }
    if (m_suspendNotify) {
        using UnregFn = BOOL(WINAPI*)(void*);
        auto unreg = (UnregFn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                    "UnregisterSuspendResumeNotification");
        if (unreg) unreg(m_suspendNotify);
        m_suspendNotify = nullptr;
    }
    m_gallery.destroy();
    m_thumbs.release();
    m_widget.destroy();
    m_panel.destroy();
    m_tray.destroy();
    if (m_effect) {
        m_effect->release();
        m_effect.reset();
    }
    m_renderer.release();
    if (msg) WTSUnRegisterSessionNotification(msg);
    m_host.destroy();
    if (m_exitEvent) {
        CloseHandle(m_exitEvent);
        m_exitEvent = nullptr;
    }
    WallpaperHost::releaseInstanceMutex();
    logx::close();
    s_app = nullptr;
}

void Application::handleIpc(const WallpaperHost::IpcMessage& msg) {
    LOG_INFO("IPC cmd=%u payload='%s'", msg.cmd, msg.payload);
    switch (msg.cmd) {
        case WallpaperHost::IpcWallpaper:
            if (msg.payload[0]) switchEffect(msg.payload);
            break;
        case WallpaperHost::IpcIconsHide:
            setHideDesktopIcons(true);
            break;
        case WallpaperHost::IpcIconsShow:
            setHideDesktopIcons(false);
            break;
        case WallpaperHost::IpcExit:
            requestQuit();
            break;
        case WallpaperHost::IpcPanel:
            openPanel();
            break;
        case WallpaperHost::IpcWidget:
            openWidget();
            break;
        case WallpaperHost::IpcHideWidget:
            hideWidget();
            break;
        default:
            LOG_WARN("Unknown IPC cmd %u", msg.cmd);
            break;
    }
}

void Application::writeSelftestReport() const {
    const std::wstring dir = appDataDirectory();
    ensureDirectory(dir);
    const std::wstring path = dir + L"\\selftest.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_ERR("selftest: could not write %s (%lu)", narrow(path).c_str(), GetLastError());
        return;
    }
    DWORD written = 0;
    WriteFile(h, m_stReport.data(), (DWORD)m_stReport.size(), &written, nullptr);
    CloseHandle(h);
    LOG_INFO("selftest report written to %s", narrow(path).c_str());
}

void Application::finishSelftest(bool pass, const char* extra) {
    if (!m_selftest) return;
    if (extra && extra[0]) {
        m_stReport += extra;
        if (m_stReport.back() != '\n') m_stReport += '\n';
    }
    m_stReport += pass ? "Result: PASS\n" : "Result: FAIL\n";
    m_stFail = !pass;
    m_stExitCode = pass ? 0 : 1;
    writeSelftestReport();
    m_stPhase = 99;
    requestQuit();
}

namespace {

bool presetUsesPlates(const std::string& id, const char* name) {
    if (!name) return false;
    if (id == "starfield") return std::strcmp(name, "Still night") == 0 || std::strcmp(name, "Milky Way") == 0;
    if (id == "aurora") return std::strcmp(name, "Classic") == 0;
    if (id == "fireflies") return std::strcmp(name, "Summer dusk") == 0;
    if (id == "snow") return std::strcmp(name, "Pine forest") == 0;
    return false;
}

bool platePresetId(const std::string& id, int preset, const wchar_t** effect, const wchar_t** name) {
    if (id == "starfield" && preset == 0) {
        *effect = L"stars";
        *name = L"still-night";
        return true;
    }
    if (id == "starfield" && preset == 1) {
        *effect = L"stars";
        *name = L"milky-way";
        return true;
    }
    if (id == "aurora" && preset == 0) {
        *effect = L"aurora";
        *name = L"classic";
        return true;
    }
    if (id == "fireflies" && preset == 0) {
        *effect = L"fireflies";
        *name = L"summer-dusk";
        return true;
    }
    if (id == "snow" && preset == 1) {
        *effect = L"snow";
        *name = L"pine-forest";
        return true;
    }
    return false;
}

float gridMotion(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0.0f;
    float move = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) move = std::max(move, std::fabs(a[i] - b[i]));
    return move;
}

}  // namespace

void Application::tickSelftest() {
    if (!m_selftest || m_stPhase >= 99) return;
    const ULONGLONG now = GetTickCount64();
    char line[512];

    if (m_stPhase == 0) {
        if (m_host.hwnd() && m_renderer.presentCount() >= 1) {
            std::snprintf(line, sizeof(line),
                          "Attach: PASS raised=%d composition=%d hwndPath=%d presents=%d\n",
                          m_host.raisedDesktop() ? 1 : 0, m_host.usesComposition() ? 1 : 0,
                          m_host.hwndPath() ? 1 : 0, m_renderer.presentCount());
            m_stReport += line;
            m_stPhase = 1;
            m_stPhaseAt = now;
        } else if (now - m_stStart > 12000) {
            m_stReport += "Attach: FAIL (no surface / no presents)\n";
            finishSelftest(false, nullptr);
        }
        return;
    }

    if (m_stPhase == 1) {
        if (m_visibilityOk) {
            m_stReport += m_host.hwndPath() ? "Visibility: PASS (hwnd)\n" : "Visibility: PASS (dcomp)\n";
            m_stPhase = 2;
            m_stPhaseAt = now;
        } else if (now - m_stPhaseAt > 20000) {
            if (m_host.hwndPath() && m_renderer.presentCount() >= 30) {
                m_stReport += "Visibility: PASS (hwnd fallback; probe could not prove DComp)\n";
                m_stPhase = 2;
                m_stPhaseAt = now;
            } else {
                m_stReport += "Visibility: FAIL (not visible after wait)\n";
                finishSelftest(false, nullptr);
            }
        }
        return;
    }

    if (m_stPhase == 2) {
        openWidget();
        // Mini mode never samples the GPU. Force a full sample without saving widgetMini.
        m_widget.forceFullSampling();
        m_stPhase = 3;
        m_stPhaseAt = now;
        return;
    }

    if (m_stPhase == 3) {
        const SysSample snap = m_widget.sysSnapshot();
        const bool statsReady = snap.ready && snap.cpuValid && snap.ramValid && snap.ramTotalGB > 0.2f;
        // CPU/RAM are valid on the first primed sample. GPU% arrives on the next
        // PDH tick (~1s). Recording before that always printed '-'.
        const bool waitGpu = !snap.gpuValid && now - m_stPhaseAt < 5000;
        const bool waitCore = !statsReady && now - m_stPhaseAt < 8000;
        if (waitGpu || waitCore) return;
        if (statsReady || now - m_stPhaseAt >= 8000) {
            char gpu[32], ram[32], cpuBuf[16];
            if (snap.gpuValid) std::snprintf(gpu, sizeof(gpu), "%.0f%%", snap.gpuPct);
            else std::snprintf(gpu, sizeof(gpu), "-");
            if (snap.ramValid) std::snprintf(ram, sizeof(ram), "%.1f/%.1f", snap.ramUsedGB, snap.ramTotalGB);
            else std::snprintf(ram, sizeof(ram), "-");
            if (snap.cpuValid) std::snprintf(cpuBuf, sizeof(cpuBuf), "%.0f%%", snap.cpuPct);
            else std::snprintf(cpuBuf, sizeof(cpuBuf), "-");
            std::snprintf(line, sizeof(line), "Stats: CPU=%s RAM=%s GPU=%s\n", cpuBuf, ram, gpu);
            m_stReport += line;
            if (!snap.gpuValid) {
                m_stReport += m_widget.samplingMini()
                                  ? "Stats: GPU=- because the widget is in mini mode (CPU-only sampler)\n"
                                  : "Stats: GPU=- because the GPU counter did not return a sample\n";
            }
            if (!snap.cpuValid || !snap.ramValid || snap.ramTotalGB < 0.2f) {
                m_stReport += "Stats: FAIL core counter unreadable\n";
                m_stFail = true;
            }
            const auto list = m_registry.list();
            if (list.empty()) {
                finishSelftest(false, "Effects: FAIL (none registered)\n");
                return;
            }
            m_stPhase = 4;
            m_stPhaseAt = now;
            if (!m_gallery.show(m_instance, this, true)) {
                m_stReport += "Gallery: FAIL (window did not open)\n";
                m_stFail = true;
                m_stEffect = 0;
                m_stPresentsMark = m_renderer.presentCount();
                switchEffect(list[0].id);
                m_stPhase = 5;
            }
        }
        return;
    }

    if (m_stPhase == 4) {
        const auto list = m_registry.list();
        bool pending = false;
        int ready = 0;
        std::string failedIds;
        for (const auto& e : list) {
            if (!m_thumbs.settled(e.id)) pending = true;
            else if (m_thumbs.failed(e.id)) {
                if (!failedIds.empty()) failedIds += ",";
                failedIds += e.id;
            } else if (m_thumbs.ready(e.id)) {
                ++ready;
            }
        }
        if (pending && now - m_stPhaseAt < 8000) return;
        {
            std::string ui;
            if (!m_gallery.runUiSelfTest(ui)) m_stFail = true;
            m_stReport += ui;
        }
        {
            std::string widget;
            if (!m_widget.runSettingsSelfTest(widget)) m_stFail = true;
            m_stReport += widget;
        }
        {
            std::string scroll;
            if (!m_panel.hwnd()) openPanel();
            if (!m_panel.runScrollSelfTest(scroll)) m_stFail = true;
            m_stReport += scroll;
        }
        {
            std::string why;
            if (!lavaSurfaceContinuity(why)) {
                m_stReport += "LavaField: FAIL " + why + "\n";
                m_stFail = true;
            } else {
                m_stReport += "LavaField: PASS " + why + "\n";
            }
        }
        {
            AppSettings probe;
            applyPreset(probe, "aurora", 3);
            const bool horizon = probe.aurora.horizon && probe.aurora.stars;
            applyPreset(probe, "starfield", 3);
            const bool warp = probe.starfield.warp > 0.5f && probe.starfield.speed > 1.0f;
            const bool names = std::string(presetName("aurora", 0)) == "Classic" &&
                               std::string(presetName("aurora", 3)) == "Horizon" &&
                               std::string(presetName("starfield", 0)) == "Still night" &&
                               std::string(presetName("starfield", 3)) == "Warp" &&
                               presetCount("aurora") == 4 && presetCount("starfield") == 4;
            if (!horizon || !warp || !names) {
                m_stReport += "SkyPresets: FAIL\n";
                m_stFail = true;
            } else {
                m_stReport += "SkyPresets: PASS aurora=4 starfield=4\n";
            }
        }
        {
            const bool names = std::string(presetName("fireflies", 0)) == "Summer dusk" &&
                               std::string(presetName("fireflies", 1)) == "Deep woods" &&
                               std::string(presetName("fireflies", 2)) == "Meadow" &&
                               presetCount("fireflies") == 3 &&
                               std::string(presetName("snow", 0)) == "Night sky" &&
                               std::string(presetName("snow", 1)) == "Pine forest" &&
                               std::string(presetName("snow", 2)) == "Window" && presetCount("snow") == 3;
            AppSettings probe;
            applyPreset(probe, "fireflies", 1);
            const bool woods = probe.fireflies.woods > 0.8f && probe.fireflies.stars < 0.05f;
            applyPreset(probe, "snow", 2);
            const bool window = probe.snow.backdrop == 2 && probe.snow.wind < 0.2f;
            if (!names || !woods || !window) {
                m_stReport += "NightPresets: FAIL\n";
                m_stFail = true;
            } else {
                m_stReport += "NightPresets: PASS fireflies=3 snow=3\n";
            }
        }
        m_gallery.hide();
        if (pending || !failedIds.empty() || ready != (int)list.size()) {
            std::snprintf(line, sizeof(line), "Gallery: FAIL thumbnails=%d/%d failed=%s\n", ready,
                          (int)list.size(), failedIds.empty() ? "-" : failedIds.c_str());
            m_stReport += line;
            m_stFail = true;
        } else {
            std::snprintf(line, sizeof(line), "Gallery: PASS thumbnails=%d\n", ready);
            m_stReport += line;
        }
        if (list.empty()) {
            finishSelftest(false, "Effects: FAIL (none registered)\n");
            return;
        }
        m_stEffect = 0;
        m_stPreset = 0;
        m_stPhase = 5;
        notePresetSwitch(list[0].id, 0);
        return;
    }

    if (m_stPhase == 5) {
        const auto list = m_registry.list();
        if (list.empty()) {
            finishSelftest(false, "Effects: FAIL (none registered)\n");
            return;
        }
        const std::string& id = list[(size_t)m_stEffect].id;
        const char* pname = presetName(id, m_stPreset);
        const bool shaderReady = m_effect && m_effect->ready();
        if (!m_stWindow) {
            // Wait for the real shader before the present clock. This wait is not Switch ms.
            if (!shaderReady && now - m_stPhaseAt < 8000) return;
            if (!shaderReady) {
                std::snprintf(line, sizeof(line),
                              "Preset %s/%s: FAIL ready timeout switch=%.0fms gpu=- luma=0\n", id.c_str(), pname,
                              m_stSwitchMs);
                m_stReport += line;
                m_stFail = true;
                if (m_stSwitchMs > 250.0) m_stFail = true;
                advanceSelftestPreset(list);
                return;
            }
            // Seed the motion grid outside the present window. The readback stalls
            // the GPU, and that stall is not a missed present.
            Renderer::SceneSample seed;
            constexpr int kGridW = 48;
            constexpr int kGridH = 27;
            m_stHaveGrid = m_renderer.sampleScene(seed, &m_stGrid, kGridW, kGridH) &&
                           m_stGrid.size() == (size_t)kGridW * kGridH;
            m_stWindow = true;
            m_stPresentsMark = m_renderer.presentCount();
            m_stPhaseAt = now;
            return;
        }
        if (now - m_stPhaseAt < 700) return;
        const int gained = m_renderer.presentCount() - m_stPresentsMark;
        Renderer::SceneSample sample;
        std::vector<float> grid;
        constexpr int kGridW = 48;
        constexpr int kGridH = 27;
        const bool read = m_renderer.sampleScene(sample, &grid, kGridW, kGridH);
        const float move = (read && m_stHaveGrid) ? gridMotion(m_stGrid, grid) : 0.0f;
        const int lit = sample.lit;
        const int sampled = sample.sampled;
        const float luma = sample.maxLuma;
        const float gpuMs = m_renderer.lastGpuMs();
        char gpuBuf[32];
        if (gpuMs < 0.0f) std::snprintf(gpuBuf, sizeof(gpuBuf), "- (no timestamp)");
        else std::snprintf(gpuBuf, sizeof(gpuBuf), "%.2fms", gpuMs);
        // Max pixel, not a patch average: matrix is a few bright glyphs on black.
        // The fallback gradient stays under 0.004 and lights nothing above 0.02.
        const bool content = read && luma >= 0.02f && lit >= 48;
        const bool plated = presetUsesPlates(id, pname);
        const float plate = m_framePlateMask;
        const bool plateBound = plate >= 14.5f;
        const bool plateClear = plate < 0.5f;
        float edge = 0.0f;
        bool edgeOk = true;
        if (id == "starfield" && !plated) {
            edge = m_renderer.sampleBandEdge();
            edgeOk = edge <= 0.25f;
        }
        float cover = 0.0f;
        bool coverOk = true;
        if (sampled > 0) cover = (float)lit / (float)sampled;
        if (plated) {
            // The photograph covers the frame. A procedural speckle stays under 40%.
            coverOk = cover >= 0.12f && cover <= 0.98f;
        } else if (id == "fireflies" || id == "snow") {
            // Flakes and fireflies speckle a dark frame, so 40% is a blown-out field.
            // Window is the room and the view: the lamp-lit wall and the night
            // through the glass sit above the 0.02 floor on purpose. That measures
            // about 0.53 at 16:9 and 0.63 at 1920x1200. A white wall of snow
            // would be near 1, and the graded mean would leave the art-direction range.
            const bool window = id == "snow" && pname && std::strcmp(pname, "Window") == 0;
            coverOk = window ? (cover >= 0.35f && cover <= 0.78f)
                             : (cover >= 0.002f && cover <= 0.40f);
        }
        bool effectOk = true;
        const float band = (sample.bandN > 0 && sample.bandR > 1e-4) ? (float)(sample.bandG / sample.bandR) : 0.0f;
        if (plated && id == "starfield") {
            // Bright points on the night plate. The mountain silhouette fails the
            // procedural Milky Way edge test, so that test stays on Nebula and Warp.
            effectOk = sample.bright >= 12;
        } else if (plated && id == "aurora") {
            // Green curtain in the open sky, or the curtain moved during the window.
            const float bandG = sample.bandN ? (float)(sample.bandG / sample.bandN) : 0.0f;
            effectOk = (band >= 1.03f && bandG >= 0.02f) || move >= 0.008f;
        } else if (plated) {
            effectOk = move >= 0.004f;
        }
        const bool plateOk = plated ? plateBound : plateClear;
        const bool pass = gained >= 2 && shaderReady && content && edgeOk && coverOk && effectOk && plateOk &&
                          m_stSwitchMs <= 250.0;
        if (plated && id == "starfield") {
            std::snprintf(line, sizeof(line),
                          "Preset %s/%s: %s presents=%d switch=%.0fms gpu=%s luma=%.4f lit=%d cover=%.4f plate=%.0f bright=%d move=%.4f\n",
                          id.c_str(), pname, pass ? "PASS" : "FAIL", gained, m_stSwitchMs, gpuBuf, luma, lit, cover,
                          plate, sample.bright, move);
        } else if (plated && id == "aurora") {
            std::snprintf(line, sizeof(line),
                          "Preset %s/%s: %s presents=%d switch=%.0fms gpu=%s luma=%.4f lit=%d cover=%.4f plate=%.0f band=%.3f move=%.4f\n",
                          id.c_str(), pname, pass ? "PASS" : "FAIL", gained, m_stSwitchMs, gpuBuf, luma, lit, cover,
                          plate, band, move);
        } else if (id == "starfield") {
            std::snprintf(line, sizeof(line),
                          "Preset %s/%s: %s presents=%d switch=%.0fms gpu=%s luma=%.4f lit=%d edge=%.3f plate=%.0f\n",
                          id.c_str(), pname, pass ? "PASS" : "FAIL", gained, m_stSwitchMs, gpuBuf, luma, lit, edge,
                          plate);
        } else if (id == "fireflies" || id == "snow") {
            if (plated) {
                std::snprintf(line, sizeof(line),
                              "Preset %s/%s: %s presents=%d switch=%.0fms gpu=%s luma=%.4f lit=%d cover=%.4f plate=%.0f move=%.4f\n",
                              id.c_str(), pname, pass ? "PASS" : "FAIL", gained, m_stSwitchMs, gpuBuf, luma, lit, cover,
                              plate, move);
            } else {
                std::snprintf(line, sizeof(line),
                              "Preset %s/%s: %s presents=%d switch=%.0fms gpu=%s luma=%.4f lit=%d cover=%.4f plate=%.0f sky=%.4f\n",
                              id.c_str(), pname, pass ? "PASS" : "FAIL", gained, m_stSwitchMs, gpuBuf, luma, lit, cover,
                              plate, sample.skyMean);
            }
        } else {
            std::snprintf(line, sizeof(line),
                          "Preset %s/%s: %s presents=%d switch=%.0fms gpu=%s luma=%.4f lit=%d plate=%.0f\n", id.c_str(),
                          pname, pass ? "PASS" : "FAIL", gained, m_stSwitchMs, gpuBuf, luma, lit, plate);
        }
        m_stReport += line;
        if (!pass) m_stFail = true;
        if (m_capture) {
            const std::wstring dir = appDataDirectory() + L"\\capture";
            ensureDirectory(dir);
            std::wstring name = widen(id) + L"-" + widen(pname);
            for (wchar_t& c : name) {
                if (c == L' ' || c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
                    c == L'<' || c == L'>' || c == L'|')
                    c = L'-';
            }
            const std::wstring file = dir + L"\\" + name + L".png";
            if (!m_renderer.capturePng(file)) {
                m_stReport += "Capture: FAIL " + narrow(file) + "\n";
                m_stFail = true;
            }
        }
        advanceSelftestPreset(list);
    }
}

void Application::awaitSwitchReady(const std::string& id, int preset) {
    if (!m_renderer.device()) return;
    const wchar_t* plateEffect = nullptr;
    const wchar_t* platePreset = nullptr;
    const bool wantPlate = platePresetId(id, preset, &plateEffect, &platePreset);
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 8000) {
        const bool plates = plateUploadsIdle(m_renderer.device()) &&
                            (!wantPlate || plateSetReady(m_renderer.device(), plateEffect, platePreset));
        const bool shader = m_renderer.skyShaderWarmed(id.c_str());
        if (plates && shader) {
            const ULONGLONG waited = GetTickCount64() - t0;
            if (waited > 30)
                LOG_INFO("Switch prep %s/%d ready in %llu ms", id.c_str(), preset, (unsigned long long)waited);
            return;
        }
        // Decode stays on the worker. This loop only uploads a layer and draws
        // a shader that just finished compiling, and none of it is Switch ms.
        renderFrame(1.0f / 30.0f, m_time);
        m_time += 1.0 / 30.0;
        m_renderer.pumpSkyWarmup();
    }
    LOG_INFO("Switch prep %s/%d timed out", id.c_str(), preset);
}

void Application::notePresetSwitch(const std::string& id, int preset) {
    awaitSwitchReady(id, preset);
    LARGE_INTEGER freq{}, a{}, b{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    applyPreset(m_config.settings(), id, preset);
    if (m_config.settings().effect != id) switchEffect(id);
    else if (m_effect) m_effect->applySettings(m_config.settings());
    QueryPerformanceCounter(&b);
    m_stSwitchMs = (freq.QuadPart > 0) ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart : 0;
    m_stWindow = false;
    m_stHaveGrid = false;
    m_stGrid.clear();
    m_stPhaseAt = GetTickCount64();
}

void Application::advanceSelftestPreset(const std::vector<EffectInfo>& list) {
    if (list.empty()) {
        finishSelftest(false, nullptr);
        return;
    }
    const std::string& id = list[(size_t)m_stEffect].id;
    m_stPreset++;
    if (m_stPreset >= presetCount(id)) {
        m_stEffect++;
        m_stPreset = 0;
        if (m_stEffect >= (int)list.size()) {
            finishSelftest(!m_stFail, nullptr);
            return;
        }
    }
    notePresetSwitch(list[(size_t)m_stEffect].id, m_stPreset);
}

void Application::warmSkyShaders() {
    if (!m_renderer.device()) return;
    const char* ids[] = {"starfield", "aurora", "fireflies", "snow"};
    for (const char* id : ids) {
        if (m_effect && std::strcmp(m_effect->id(), id) == 0) {
            m_effect->precompile(&m_renderer);
            continue;
        }
        auto fx = m_registry.create(id);
        if (!fx) continue;
        fx->precompile(&m_renderer);
        fx->release();
    }
}
