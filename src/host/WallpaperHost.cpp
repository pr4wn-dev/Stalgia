#include "host/WallpaperHost.h"

#include "core/Log.h"
#include "core/Util.h"
#include "host/DesktopDiag.h"

#include <cstring>

namespace {

BOOL CALLBACK enumClassicWorker(HWND hwnd, LPARAM lParam) {
    HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!defView) return TRUE;
    HWND* out = reinterpret_cast<HWND*>(lParam);
    // The wallpaper WorkerW is the next top-level sibling after the window that owns DefView.
    HWND w = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
    if (!w) return TRUE;
    *out = w;
    return FALSE;
}

void writeWallpaperRestoreFile(bool raised, const std::wstring& wall) {
    const std::wstring path = appDataDirectory() + L"\\wallpaper.restore";
    ensureDirectory(appDataDirectory());
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string body = std::string(raised ? "raised\n" : "classic\n") + narrow(wall) + "\n";
    DWORD wrote = 0;
    WriteFile(h, body.data(), (DWORD)body.size(), &wrote, nullptr);
    CloseHandle(h);
}

}  // namespace

HANDLE WallpaperHost::s_mutex = nullptr;

bool WallpaperHost::alreadyRunning() {
    s_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!s_mutex) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(s_mutex);
        s_mutex = nullptr;
        return true;
    }
    return false;
}

void WallpaperHost::releaseInstanceMutex() {
    if (s_mutex) {
        ReleaseMutex(s_mutex);
        CloseHandle(s_mutex);
        s_mutex = nullptr;
    }
}

HWND WallpaperHost::findRunningHost() {
    HWND host = FindWindowW(kHostClass, kHostTitle);
    if (!host) host = FindWindowW(kHostClass, nullptr);
    if (!host) host = findWindowByClass(kHostClass);
    return host;
}

bool WallpaperHost::sendIpc(uint32_t cmd, const char* payload) {
    HWND host = findRunningHost();
    if (!host) {
        LOG_WARN("StalgiaHost window not found (ipc cmd=%u)", cmd);
        return false;
    }
    IpcMessage msg{};
    msg.cmd = cmd;
    if (payload && payload[0]) {
        std::strncpy(msg.payload, payload, sizeof(msg.payload) - 1);
    }
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyIpc;
    cds.cbData = sizeof(msg);
    cds.lpData = &msg;
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(host, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 3000, &result)) {
        LOG_WARN("WM_COPYDATA cmd=%u failed: %lu", cmd, GetLastError());
        return false;
    }
    LOG_INFO("IPC cmd=%u payload='%s' handled=%d", cmd, payload ? payload : "", result ? 1 : 0);
    return result != 0;
}

bool WallpaperHost::requestRunningInstancePanel() {
    return sendIpc(IpcPanel);
}

bool WallpaperHost::requestRunningInstanceWidget() {
    return sendIpc(IpcWidget);
}

bool WallpaperHost::requestRunningInstanceHideWidget() {
    return sendIpc(IpcHideWidget);
}

bool WallpaperHost::requestRunningInstanceIcons(bool hide) {
    return sendIpc(hide ? IpcIconsHide : IpcIconsShow);
}

bool WallpaperHost::requestRunningInstanceWallpaper(const std::string& id) {
    return sendIpc(IpcWallpaper, id.c_str());
}

bool WallpaperHost::requestRunningInstanceExit() {
    bool signaled = sendIpc(IpcExit);
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kExitEventName);
    if (ev) {
        if (SetEvent(ev)) {
            LOG_INFO("Signaled exit event");
            signaled = true;
        }
        CloseHandle(ev);
    } else {
        LOG_WARN("OpenEvent(%s) failed: %lu", narrow(std::wstring(kExitEventName)).c_str(), GetLastError());
    }
    HWND host = findRunningHost();
    if (host) {
        PostMessageW(host, WM_CLOSE, 0, 0);
        signaled = true;
    }
    return signaled;
}

bool WallpaperHost::create(HINSTANCE instance, HostMode mode, WNDPROC proc, void* user) {
    m_mode = mode;
    m_user = user;
    m_instance = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hIcon = loadAppIcon(instance, largeIconPx(nullptr));
    wc.hIconSm = loadAppIcon(instance, smallIconPx(nullptr));
    wc.lpszClassName = kSurfaceClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW mc = wc;
    mc.lpszClassName = kHostClass;
    mc.hbrBackground = nullptr;
    RegisterClassExW(&mc);

    // Real top-level HWND (not HWND_MESSAGE) so FindWindow / --exit can see it.
    m_messageHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kHostClass, kHostTitle, WS_POPUP, 0,
                                    0, 1, 1, nullptr, nullptr, instance, user);
    if (m_messageHwnd) {
        // Keep it hidden; 1x1 is enough for FindWindow.
        ShowWindow(m_messageHwnd, SW_HIDE);
        LOG_INFO("Host IPC window created");
        char desc[768];
        describeWindow(m_messageHwnd, desc, sizeof(desc));
        LOG_INFO("  %s", desc);
    } else {
        LOG_ERR("CreateWindow StalgiaHost failed: %lu", GetLastError());
    }

    if (mode == HostMode::Window) return createPreviewWindow(instance, proc);
    return createDesktopWindow(instance, proc);
}

void WallpaperHost::raiseDesktopClassic(HWND progman) {
    auto send = [&](WPARAM w, LPARAM l, const char* tag) {
        DWORD_PTR result = 0;
        const LRESULT sent = SendMessageTimeoutW(progman, 0x052C, w, l, SMTO_NORMAL, 1000, &result);
        LOG_INFO("Sent 0x052C (%s) to Progman, sent=%ld result=%p", tag, (long)sent, (void*)result);
    };
    // Win10 and Win11 22H2/23H2: (0,0) spawns the WorkerW behind the icons.
    send(0, 0, "w=0,l=0");
    if (findClassicWorkerW() || findChildWorkerW(progman)) return;
    // Some 22H2/23H2 builds ignore (0,0) and only spawn on (0xD, 0) or (0xD, 1).
    send(0xD, 0, "w=0xD,l=0");
    if (findClassicWorkerW() || findChildWorkerW(progman)) return;
    send(0xD, 1, "w=0xD,l=1");
}

HWND WallpaperHost::findDefView(HWND progman) const {
    HWND def = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def) return def;
    // Sometimes DefView lives under a WorkerW child of Progman (classic nested).
    HWND w = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    while (w) {
        def = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr);
        if (def) return def;
        w = FindWindowExW(progman, w, L"WorkerW", nullptr);
    }
    struct Ctx {
        HWND def = nullptr;
    } ctx;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            HWND d = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
            if (d) {
                reinterpret_cast<Ctx*>(lp)->def = d;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&ctx);
    return ctx.def;
}

HWND WallpaperHost::findChildWorkerW(HWND progman) const {
    HWND w = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    HWND lastWithoutDef = nullptr;
    HWND any = nullptr;
    while (w) {
        any = w;
        if (!FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) lastWithoutDef = w;
        w = FindWindowExW(progman, w, L"WorkerW", nullptr);
    }
    // Prefer the wallpaper WorkerW (no DefView inside). If the only WorkerW hosts
    // DefView — common on 24H2 — still return it so we don't log worker=0.
    return lastWithoutDef ? lastWithoutDef : any;
}

HWND WallpaperHost::findClassicWorkerW() const {
    HWND worker = nullptr;
    EnumWindows(enumClassicWorker, (LPARAM)&worker);
    return worker;
}

ZOrderSnapshot WallpaperHost::snapshotZOrder() const {
    ZOrderSnapshot s;
    s.surface = m_hwnd;
    if (!m_hwnd) return s;
    s.parent = GetParent(m_hwnd);
    s.progman = IsWindow(m_progman) ? m_progman : FindWindowW(L"Progman", nullptr);
    s.defView = findDefView(s.progman);
    s.worker = findChildWorkerW(s.progman);
    s.visible = IsWindowVisible(m_hwnd) != FALSE;
    s.parentIsProgman = s.parent && s.progman && s.parent == s.progman;
    if (s.progman) {
        int rank = 0;
        for (HWND c = GetWindow(s.progman, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT), ++rank) {
            s.childCount++;
            if (c == m_hwnd) s.surfaceRank = rank;
        }
    }
    if (s.defView && GetParent(s.defView) == s.progman) {
        s.belowDefView = GetWindow(s.defView, GW_HWNDNEXT);
        s.belowDefViewIsUs = s.belowDefView == m_hwnd;
    }
    if (s.progman) {
        bool seenUs = false;
        for (HWND c = GetWindow(s.progman, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            if (c == m_hwnd) {
                seenUs = true;
                continue;
            }
            wchar_t cls[32]{};
            GetClassNameW(c, cls, 32);
            if (!seenUs && lstrcmpW(cls, L"WorkerW") == 0) s.workerAboveUs = true;
        }
    }
    return s;
}

void WallpaperHost::quietZOrder(DWORD milliseconds) {
    const ULONGLONG until = GetTickCount64() + milliseconds;
    if (until > m_zQuietUntil) m_zQuietUntil = until;
}

bool WallpaperHost::maintainZOrder() {
    if (m_mode != HostMode::Desktop || !m_raised || !m_hwnd || m_fixingZ) return false;
    if (!IsWindow(m_hwnd)) return false;
    if (GetTickCount64() < m_zQuietUntil) return false;

    m_progman = IsWindow(m_progman) ? m_progman : FindWindowW(L"Progman", nullptr);
    if (!m_progman) return false;

    const ZOrderSnapshot before = snapshotZOrder();
    // Show Desktop raises Explorer's wallpaper WorkerW. Fighting that z-order is a
    // leading suspect for Win+D never completing — leave WorkerW where Explorer put it.
    if (before.workerAboveUs) {
        quietZOrder(2000);
        return false;
    }
    const bool parentOk = m_anchorWorker ? (m_worker && before.parent == m_worker) : before.parentIsProgman;
    bool dirty = false;
    if (!parentOk) dirty = true;
    if (!before.visible) dirty = true;
    if (!m_anchorWorker && before.defView && GetParent(before.defView) == m_progman && !before.belowDefViewIsUs)
        dirty = true;

    if (!dirty) return false;

    m_fixingZ = true;
    LOG_INFO("Z-order drift: parentOk=%d anchorWorker=%d vis=%d belowDefViewIsUs=%d workerAboveUs=%d rank=%d/%d def=%p worker=%p below=%p",
             parentOk ? 1 : 0, m_anchorWorker ? 1 : 0, before.visible ? 1 : 0, before.belowDefViewIsUs ? 1 : 0,
             before.workerAboveUs ? 1 : 0, before.surfaceRank, before.childCount, (void*)before.defView,
             (void*)before.worker, (void*)before.belowDefView);

    if (!parentOk) {
        HWND want = m_anchorWorker && m_worker ? m_worker : m_progman;
        SetParent(m_hwnd, want);
        m_compositionDirty = true;
    }
    if (!IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }
    applyZOrder(m_progman);
    m_compositionDirty = true;
    m_fixingZ = false;

    const ZOrderSnapshot after = snapshotZOrder();
    LOG_INFO("Z-order fixed: vis=%d belowDefViewIsUs=%d workerAboveUs=%d rank=%d/%d",
             after.visible ? 1 : 0, after.belowDefViewIsUs ? 1 : 0, after.workerAboveUs ? 1 : 0,
             after.surfaceRank, after.childCount);
    return true;
}

void WallpaperHost::logHeartbeat(const char* pausedReason, int frames, HRESULT lastPresent) const {
    const ZOrderSnapshot s = snapshotZOrder();
    LOG_INFO("heartbeat frames=%d paused=%s vis=%d parentProgman=%d zRank=%d/%d belowDefViewIsUs=%d "
             "workerAboveUs=%d def=%p worker=%p lastPresent=0x%08X dcomp=%d hwndPath=%d",
             frames, pausedReason ? pausedReason : "none", s.visible ? 1 : 0, s.parentIsProgman ? 1 : 0,
             s.surfaceRank, s.childCount, s.belowDefViewIsUs ? 1 : 0, s.workerAboveUs ? 1 : 0,
             (void*)s.defView, (void*)s.worker, (unsigned)lastPresent, m_useComposition ? 1 : 0,
             m_forceHwndSwapchain ? 1 : 0);
}

void WallpaperHost::applyZOrder(HWND progman) {
    m_defView = findDefView(progman);
    m_worker = findChildWorkerW(progman);
    if (!m_hwnd) return;

    // Below icons (DefView), above Explorer's wallpaper WorkerW.
    if (m_defView && GetParent(m_hwnd) == GetParent(m_defView)) {
        // Only move OUR window. Never SetWindowPos Explorer's WorkerW — that is a
        // strong suspect for breaking Win+D / Show Desktop on 24H2+.
        SetWindowPos(m_hwnd, m_defView, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        LOG_INFO("Z-order: surface inserted below SHELLDLL_DefView %p (worker=%p)", (void*)m_defView,
                 (void*)m_worker);
    } else if (GetParent(m_hwnd) == m_worker && m_worker) {
        SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        LOG_INFO("Z-order: surface HWND_BOTTOM under WorkerW %p", (void*)m_worker);
    } else if (m_defView) {
        SetWindowPos(m_hwnd, m_defView, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        LOG_INFO("Z-order: surface inserted below SHELLDLL_DefView %p", (void*)m_defView);
    } else {
        LOG_WARN("SHELLDLL_DefView not found; using HWND_BOTTOM");
        SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

HWND WallpaperHost::spawnRaisedWorkerW(HWND progman) {
    // (0xD, 1) is the 24H2+ refcounted spawn. Send it on every attach, even when a
    // WorkerW is already there — skipping it left DefView opaque. (0, 0) *releases*
    // the WorkerW and Explorer destroys it.
    DWORD_PTR result = 0;
    LOG_INFO("WorkerW spawn: sending 0x052C (w=0xD, l=1) to Progman %p", (void*)progman);
    LRESULT sent = SendMessageTimeoutW(progman, 0x052C, 0xD, 1, SMTO_NORMAL, 1000, &result);
    LOG_INFO("WorkerW spawn: 0x052C sent=%ld result=%p err=%lu", (long)sent, (void*)result,
             sent ? 0ul : GetLastError());
    HWND found = nullptr;
    for (int i = 0; i < 12; ++i) {
        int n = 0;
        for (HWND w = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); w;
             w = FindWindowExW(progman, w, L"WorkerW", nullptr)) {
            if (i == 0) {
                HWND def = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr);
                LOG_INFO("WorkerW spawn: child[%d]=%p defViewInside=%p", n, (void*)w, (void*)def);
            }
            n++;
        }
        found = findChildWorkerW(progman);
        if (found) {
            if (i > 0) LOG_INFO("WorkerW spawn: appeared on poll %d (%d WorkerW child(ren))", i + 1, n);
            break;
        }
        Sleep(40);
    }
    m_worker = found;
    if (found) LOG_INFO("WorkerW spawn: using %p", (void*)found);
    else {
        LOG_WARN("WorkerW spawn: none found after 0x052C (0xD,1) — DefView may stay opaque");
        logDesktopWindowTree();
    }
    return found;
}

HWND WallpaperHost::ensureRaisedWorkerW() {
    m_progman = IsWindow(m_progman) ? m_progman : FindWindowW(L"Progman", nullptr);
    if (!m_progman) return nullptr;
    HWND w = spawnRaisedWorkerW(m_progman);
    if (w && m_hwnd && m_raised) applyZOrder(m_progman);
    return w;
}

bool WallpaperHost::createDesktopWindow(HINSTANCE instance, WNDPROC proc) {
    (void)proc;
    saveOriginalWallpaper();
    m_progman = FindWindowW(L"Progman", nullptr);
    if (!m_progman) {
        LOG_ERR("FindWindow(Progman) failed");
        return false;
    }

    m_raised = progmanIsRaisedDesktop(m_progman);
    LOG_INFO("Desktop attach: raised=%d (WS_EX_NOREDIRECTIONBITMAP on Progman)", m_raised ? 1 : 0);
    if (GetSystemMetrics(SM_REMOTESESSION)) {
        m_forceHwndSwapchain = true;
        LOG_INFO("Remote session: HWND path (DComp skipped)");
    }
    logDesktopWindowTree();

    if (m_raised) {
        HWND def = findDefView(m_progman);
        LOG_INFO("Raised desktop (DefView=%p) — spawning child WorkerW with 0x052C (0xD,1)", (void*)def);
        spawnRaisedWorkerW(m_progman);
        logDesktopWindowTree();
    } else {
        raiseDesktopClassic(m_progman);
        m_raised = progmanIsRaisedDesktop(m_progman);
        LOG_INFO("After 0x052C (0,0): raised=%d", m_raised ? 1 : 0);
        logDesktopWindowTree();
    }

    m_useComposition = m_raised && !m_forceHwndSwapchain;
    const bool ok = m_raised ? attachRaised(m_progman, instance) : attachClassic(m_progman, instance);
    writeWallpaperRestoreFile(m_raised, m_originalWallpaper);
    if (ok) logAttachResult();
    return ok;
}

bool WallpaperHost::createRaisedChild(HWND progman, HINSTANCE instance, bool noredirect) {
    int x, y, w, h;
    virtualScreen(x, y, w, h);
    POINT origin{x, y};
    ScreenToClient(progman, &origin);

    DWORD ex = WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;
    if (noredirect) ex |= WS_EX_NOREDIRECTIONBITMAP;
    m_hwnd = CreateWindowExW(ex, kSurfaceClass, L"Stalgia Wallpaper", WS_CHILD | WS_VISIBLE, origin.x,
                             origin.y, w, h, progman, nullptr, instance, m_user);
    if (!m_hwnd && noredirect) {
        LOG_WARN("CreateWindow raised (layered+noredirect) failed %lu, retrying without NOREDIRECTIONBITMAP",
                 GetLastError());
        return createRaisedChild(progman, instance, false);
    }
    if (!m_hwnd) {
        // Cross-process CreateWindow child can fail with ACCESS_DENIED; popup then SetParent.
        LOG_WARN("CreateWindow raised child failed %lu, trying popup+SetParent", GetLastError());
        m_hwnd = CreateWindowExW(ex, kSurfaceClass, L"Stalgia Wallpaper", WS_POPUP, x, y, w, h, nullptr,
                                 nullptr, instance, m_user);
        if (m_hwnd) {
            SetParent(m_hwnd, progman);
            LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
            style |= WS_CHILD;
            style &= ~(LONG_PTR)WS_POPUP;
            SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);
            SetWindowPos(m_hwnd, nullptr, origin.x, origin.y, w, h,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
    }
    if (!m_hwnd) {
        LOG_ERR("CreateWindow raised failed: %lu", GetLastError());
        return false;
    }
    if (!SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA)) {
        LOG_WARN("SetLayeredWindowAttributes failed: %lu", GetLastError());
    }
    return true;
}

bool WallpaperHost::attachRaised(HWND progman, HINSTANCE instance) {
    int x, y, w, h;
    virtualScreen(x, y, w, h);
    POINT origin{x, y};
    ScreenToClient(progman, &origin);
    LOG_INFO("Raised attach: virtual=(%d,%d) %dx%d parent-client origin=(%ld,%ld) worker=%p hwndPath=%s", x, y,
             w, h, origin.x, origin.y, (void*)m_worker, m_forceHwndSwapchain ? "layered-hwnd" : "dcomp");

    // Microsoft 24H2: WS_EX_LAYERED child of Progman, under DefView, above WorkerW.
    // NOREDIRECTIONBITMAP + DComp is required for children of a no-redirection Progman.
    // Last-resort fallback drops NOREDIRECTIONBITMAP and uses a HWND swapchain.
    if (!createRaisedChild(progman, instance, !m_forceHwndSwapchain)) return false;

    applyZOrder(progman);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    m_anchorWorker = false;
    m_useComposition = m_raised && !m_forceHwndSwapchain;
    m_compositionDirty = false;
    return true;
}

bool WallpaperHost::parentToWorkerW() {
    if (!m_hwnd || !m_progman) return false;
    HWND worker = m_worker ? m_worker : spawnRaisedWorkerW(m_progman);
    if (!worker) return false;
    HWND parent = GetParent(m_hwnd);
    if (parent == worker) {
        LOG_INFO("Already parented to WorkerW %p", (void*)worker);
        return true;
    }
    LOG_INFO("Fallback: SetParent surface %p -> WorkerW %p (was %p)", (void*)m_hwnd, (void*)worker,
             (void*)parent);
    SetParent(m_hwnd, worker);
    m_worker = worker;
    RECT rc{};
    GetClientRect(worker, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 8 || h < 8) {
        int x = 0, y = 0;
        virtualScreen(x, y, w, h);
    }
    SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    m_anchorWorker = true;
    m_compositionDirty = true;
    logAttachResult();
    return true;
}

bool WallpaperHost::recreateLayeredHwnd() {
    if (!m_progman || !m_instance) return false;
    LOG_INFO("Fallback: recreating as layered HWND (no NOREDIRECTIONBITMAP, no DComp)");
    HWND old = m_hwnd;
    m_hwnd = nullptr;
    m_forceHwndSwapchain = true;
    m_useComposition = false;
    if (!createRaisedChild(m_progman, m_instance, false)) {
        m_hwnd = old;
        m_forceHwndSwapchain = false;
        m_useComposition = m_raised;
        return false;
    }
    if (old && IsWindow(old)) DestroyWindow(old);
    m_anchorWorker = false;
    applyZOrder(m_progman);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    m_compositionDirty = true;
    logAttachResult();
    return true;
}

bool WallpaperHost::restoreDcompChild() {
    if (!m_raised || !m_progman || !m_instance) return false;
    if (!m_forceHwndSwapchain) {
        m_useComposition = true;
        return true;
    }
    LOG_INFO("Promote: recreating DComp child (NOREDIRECTIONBITMAP) after HWND fallback");
    HWND old = m_hwnd;
    m_hwnd = nullptr;
    m_forceHwndSwapchain = false;
    m_useComposition = true;
    if (!createRaisedChild(m_progman, m_instance, true)) {
        m_hwnd = old;
        m_forceHwndSwapchain = true;
        m_useComposition = false;
        return false;
    }
    if (old && IsWindow(old)) DestroyWindow(old);
    m_anchorWorker = false;
    applyZOrder(m_progman);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    m_compositionDirty = true;
    logAttachResult();
    return true;
}

bool WallpaperHost::attachClassic(HWND progman, HINSTANCE instance) {
    m_worker = findClassicWorkerW();
    if (!m_worker) {
        HWND child = findChildWorkerW(progman);
        if (child && !FindWindowExW(child, nullptr, L"SHELLDLL_DefView", nullptr)) m_worker = child;
    }
    // Already an HWND swapchain. Do not promote this to DComp later.
    m_forceHwndSwapchain = true;
    m_useComposition = false;

    int x, y, w, h;
    virtualScreen(x, y, w, h);
    if (!m_worker) {
        LOG_INFO("Classic attach: no WorkerW; layered HWND child of Progman under DefView");
        POINT origin{x, y};
        ScreenToClient(progman, &origin);
        m_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                                 kSurfaceClass, L"Stalgia Wallpaper", WS_CHILD | WS_VISIBLE, origin.x, origin.y,
                                 w, h, progman, nullptr, instance, m_user);
        if (!m_hwnd) {
            LOG_ERR("CreateWindow classic Progman child failed: %lu", GetLastError());
            return false;
        }
        SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
        applyZOrder(progman);
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        return true;
    }

    LOG_INFO("Classic WorkerW target %p", (void*)m_worker);
    m_hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, kSurfaceClass,
                             L"Stalgia Wallpaper", WS_POPUP, x, y, w, h, nullptr, nullptr, instance,
                             m_user);
    if (!m_hwnd) {
        LOG_ERR("CreateWindow classic failed: %lu", GetLastError());
        return false;
    }

    SetParent(m_hwnd, m_worker);
    LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    style |= WS_CHILD;
    style &= ~(LONG_PTR)WS_POPUP;
    SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);
    SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void WallpaperHost::logAttachResult() const {
    char desc[768];
    describeWindow(m_hwnd, desc, sizeof(desc));
    LOG_INFO("Surface: %s", desc);
    LOG_INFO("Parent now %p (Progman=%p WorkerW=%p DefView=%p raised=%d)", (void*)GetParent(m_hwnd),
             (void*)m_progman, (void*)m_worker, (void*)m_defView, m_raised ? 1 : 0);
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    LOG_INFO("Surface client %ldx%ld dpiScale=%.2f", rc.right - rc.left, rc.bottom - rc.top, dpiScale());
}

bool WallpaperHost::createPreviewWindow(HINSTANCE instance, WNDPROC proc) {
    (void)proc;
    const int w = 1280;
    const int h = 720;
    RECT r{0, 0, w, h};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
    m_hwnd = CreateWindowExW(0, kSurfaceClass, L"Stalgia Preview", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance,
                             m_user);
    if (!m_hwnd) return false;
    ShowWindow(m_hwnd, SW_SHOW);
    applyWindowIcons(m_hwnd, instance);
    UpdateWindow(m_hwnd);
    return true;
}

void WallpaperHost::virtualScreen(int& x, int& y, int& w, int& h) const {
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0) w = GetSystemMetrics(SM_CXSCREEN);
    if (h <= 0) h = GetSystemMetrics(SM_CYSCREEN);
}

bool WallpaperHost::syncToVirtualScreen() {
    if (!m_hwnd || m_mode != HostMode::Desktop) return false;
    int x, y, w, h;
    virtualScreen(x, y, w, h);
    HWND parent = GetParent(m_hwnd);
    int px = 0, py = 0;
    if (parent) {
        POINT origin{x, y};
        ScreenToClient(parent, &origin);
        px = origin.x;
        py = origin.y;
    }
    SetWindowPos(m_hwnd, nullptr, px, py, w, h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    if (m_raised && m_progman) applyZOrder(m_progman);
    LOG_INFO("syncToVirtualScreen pos=(%d,%d) %dx%d", px, py, w, h);
    return true;
}

bool WallpaperHost::reattach() {
    if (m_mode != HostMode::Desktop) return false;
    if (!m_hwnd || !IsWindow(m_hwnd)) {
        LOG_INFO("Reattach: surface HWND gone, recreating");
        return setupSurface();
    }
    m_progman = FindWindowW(L"Progman", nullptr);
    if (!m_progman) return false;
    m_raised = progmanIsRaisedDesktop(m_progman);
    if (m_raised) {
        spawnRaisedWorkerW(m_progman);
        if (m_anchorWorker && m_worker) SetParent(m_hwnd, m_worker);
        else SetParent(m_hwnd, m_progman);
        applyZOrder(m_progman);
        m_useComposition = !m_forceHwndSwapchain;
    } else {
        m_worker = findClassicWorkerW();
        if (!m_worker) {
            HWND child = findChildWorkerW(m_progman);
            if (child && !FindWindowExW(child, nullptr, L"SHELLDLL_DefView", nullptr)) m_worker = child;
        }
        if (!m_worker) m_worker = m_progman;
        SetParent(m_hwnd, m_worker);
        m_useComposition = false;
        m_forceHwndSwapchain = true;
    }
    m_compositionDirty = true;
    return syncToVirtualScreen();
}

int WallpaperHost::primaryHeight() const {
    return GetSystemMetrics(SM_CYSCREEN);
}

float WallpaperHost::dpiScale() const {
    HWND w = m_hwnd ? m_hwnd : GetDesktopWindow();
    UINT dpi = 96;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        auto fn = (GetDpiForWindowFn)(void*)GetProcAddress(user32, "GetDpiForWindow");
        if (fn) dpi = fn(w);
    }
    if (dpi == 0) dpi = 96;
    return dpi / 96.0f;
}

void WallpaperHost::saveOriginalWallpaper() {
    wchar_t path[MAX_PATH]{};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0)) {
        m_originalWallpaper = path;
        m_savedWallpaper = true;
        LOG_INFO("Saved original wallpaper: %s", narrow(m_originalWallpaper).c_str());
    }
}

void WallpaperHost::restoreOriginalWallpaper() {
    teardownSurface();
}

void WallpaperHost::restoreWallpaperFromCrash() {
    const std::wstring path = appDataDirectory() + L"\\wallpaper.restore";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[1200]{};
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);
    if (n >= 6 && std::strncmp(buf, "raised", 6) == 0) return;
    const char* nl = std::strchr(buf, '\n');
    std::string wall = nl ? std::string(nl + 1) : std::string();
    while (!wall.empty() && (wall.back() == '\n' || wall.back() == '\r')) wall.pop_back();
    std::wstring wide = widen(wall);
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wide.empty() ? nullptr : (PVOID)wide.c_str(),
                          SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

bool WallpaperHost::setupSurface() {
    if (m_hwnd && IsWindow(m_hwnd)) return true;
    if (!m_instance) return false;
    if (m_mode == HostMode::Window) return createPreviewWindow(m_instance, nullptr);
    return createDesktopWindow(m_instance, nullptr);
}

void WallpaperHost::teardownSurface() {
    if (m_hwnd) {
        releaseWindowIcons(m_hwnd);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    if (m_mode != HostMode::Desktop) return;

    // Never hide WorkerW windows we did not create. On 24H2+ Explorer's child WorkerW
    // is the static wallpaper surface and also involved in Show Desktop. On classic
    // Win10, SPI_SETDESKWALLPAPER below is enough to restore the bitmap.

    if (m_raised) {
        // Our child is gone; DefView shows the Windows wallpaper again. Re-applying a
        // cached static path here kills slideshow and Spotlight.
        LOG_INFO("Raised desktop: left the Windows wallpaper / slideshow alone");
    } else if (m_savedWallpaper) {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)m_originalWallpaper.c_str(),
                              SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    } else {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }

    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        InvalidateRect(progman, nullptr, TRUE);
        UpdateWindow(progman);
    }
    LOG_INFO("Restored original desktop wallpaper (surface torn down)");
}

void WallpaperHost::destroy() {
    teardownSurface();
    if (m_messageHwnd) {
        DestroyWindow(m_messageHwnd);
        m_messageHwnd = nullptr;
    }
    m_worker = nullptr;
    m_progman = nullptr;
    m_defView = nullptr;
}
