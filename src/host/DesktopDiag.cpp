#include "host/DesktopDiag.h"

#include "core/Log.h"
#include "core/Util.h"

#include <dwmapi.h>
#include <string>

namespace {

typedef LONG(WINAPI* RtlGetVersionFn)(void*);

struct RtlOsVersionInfo {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion;
    ULONG dwMinorVersion;
    ULONG dwBuildNumber;
    ULONG dwPlatformId;
    WCHAR szCSDVersion[128];
};

void appendStyle(std::string& s, LONG_PTR v, LONG_PTR bit, const char* name) {
    if (v & bit) {
        if (!s.empty()) s += "|";
        s += name;
    }
}

std::string styleBits(LONG_PTR st) {
    std::string s;
    appendStyle(s, st, WS_CHILD, "CHILD");
    appendStyle(s, st, WS_POPUP, "POPUP");
    appendStyle(s, st, WS_VISIBLE, "VISIBLE");
    appendStyle(s, st, WS_DISABLED, "DISABLED");
    appendStyle(s, st, WS_CAPTION, "CAPTION");
    appendStyle(s, st, WS_THICKFRAME, "THICKFRAME");
    appendStyle(s, st, WS_CLIPSIBLINGS, "CLIPSIBLINGS");
    appendStyle(s, st, WS_CLIPCHILDREN, "CLIPCHILDREN");
    appendStyle(s, st, WS_MAXIMIZE, "MAXIMIZE");
    appendStyle(s, st, WS_MINIMIZE, "MINIMIZE");
    return s.empty() ? "0" : s;
}

std::string exStyleBits(LONG_PTR st) {
    std::string s;
    appendStyle(s, st, WS_EX_LAYERED, "LAYERED");
    appendStyle(s, st, WS_EX_TRANSPARENT, "TRANSPARENT");
    appendStyle(s, st, WS_EX_NOACTIVATE, "NOACTIVATE");
    appendStyle(s, st, WS_EX_TOOLWINDOW, "TOOLWINDOW");
    appendStyle(s, st, WS_EX_TOPMOST, "TOPMOST");
    appendStyle(s, st, WS_EX_NOREDIRECTIONBITMAP, "NOREDIRECTIONBITMAP");
    appendStyle(s, st, WS_EX_COMPOSITED, "COMPOSITED");
    appendStyle(s, st, WS_EX_APPWINDOW, "APPWINDOW");
    return s.empty() ? "0" : s;
}

bool cloaked(HWND hwnd) {
    BOOL c = FALSE;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &c, sizeof(c));
    return c != FALSE;
}

void dumpOne(HWND hwnd, int depth, int remaining) {
    if (!hwnd || remaining <= 0) return;
    char desc[768];
    describeWindow(hwnd, desc, sizeof(desc));
    LOG_INFO("%*s%s", depth * 2, "", desc);

    for (HWND c = GetWindow(hwnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        dumpOne(c, depth + 1, remaining - 1);
    }
}

}  // namespace

DWORD windowsBuildNumber() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    auto fn = (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion");
    if (!fn) return 0;
    RtlOsVersionInfo info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn(&info) != 0) return 0;
    return info.dwBuildNumber;
}

bool progmanIsRaisedDesktop(HWND progman) {
    if (!progman) return false;
    const LONG_PTR ex = GetWindowLongPtrW(progman, GWL_EXSTYLE);
    return (ex & WS_EX_NOREDIRECTIONBITMAP) != 0;
}

void describeWindow(HWND hwnd, char* out, int outChars) {
    if (!out || outChars <= 0) return;
    if (!hwnd) {
        std::snprintf(out, outChars, "(null)");
        return;
    }
    wchar_t cls[128]{};
    wchar_t title[128]{};
    GetClassNameW(hwnd, cls, 128);
    GetWindowTextW(hwnd, title, 128);
    RECT wr{}, cr{};
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);
    HWND parent = GetAncestor(hwnd, GA_PARENT);
    if (parent == GetDesktopWindow()) parent = GetParent(hwnd);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const LONG_PTR st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    std::snprintf(out, outChars,
                  "hwnd=%p class='%s' title='%s' parent=%p vis=%d cloaked=%d pid=%lu "
                  "rect=[%ld,%ld,%ld,%ld] client=%ldx%ld style=%s ex=%s",
                  (void*)hwnd, narrow(std::wstring(cls)).c_str(), narrow(std::wstring(title)).c_str(),
                  (void*)parent, IsWindowVisible(hwnd) ? 1 : 0, cloaked(hwnd) ? 1 : 0, pid, wr.left, wr.top,
                  wr.right, wr.bottom, cr.right - cr.left, cr.bottom - cr.top, styleBits(st).c_str(),
                  exStyleBits(ex).c_str());
}

void logOsVersion() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    RtlOsVersionInfo info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn && fn(&info) == 0) {
        LOG_INFO("OS %lu.%lu build %lu platform %lu", info.dwMajorVersion, info.dwMinorVersion,
                 info.dwBuildNumber, info.dwPlatformId);
    } else {
        LOG_INFO("OS version unknown");
    }
}

void logMonitorAndDpiInfo() {
    LOG_INFO("SM_CXSCREEN=%d SM_CYSCREEN=%d virtual=(%d,%d) %dx%d dpiAwareness process",
             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
             GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
             GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    auto getDpi = user32 ? (GetDpiForWindowFn)(void*)GetProcAddress(user32, "GetDpiForWindow") : nullptr;

    struct Ctx {
        GetDpiForWindowFn getDpi;
        UINT minDpi = 0;
        UINT maxDpi = 0;
        int monitors = 0;
    } ctx{getDpi, 0, 0, 0};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(mon, (MONITORINFO*)&mi)) return TRUE;
            UINT dpi = 96;
            if (c->getDpi) dpi = c->getDpi(GetDesktopWindow());
            HMODULE shcore = GetModuleHandleW(L"Shcore.dll");
            if (!shcore) shcore = LoadLibraryW(L"Shcore.dll");
            if (shcore) {
                using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
                auto fn = (GetDpiForMonitorFn)(void*)GetProcAddress(shcore, "GetDpiForMonitor");
                UINT x = 0, y = 0;
                if (fn && SUCCEEDED(fn(mon, 0, &x, &y))) dpi = x;
            }
            LOG_INFO("Monitor '%s' primary=%d monitor=[%ld,%ld,%ld,%ld] work=[%ld,%ld,%ld,%ld] dpi=%u scale=%.2f",
                     narrow(mi.szDevice).c_str(), (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0, mi.rcMonitor.left,
                     mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom, mi.rcWork.left, mi.rcWork.top,
                     mi.rcWork.right, mi.rcWork.bottom, dpi, dpi / 96.0f);
            c->monitors++;
            if (c->minDpi == 0 || dpi < c->minDpi) c->minDpi = dpi;
            if (dpi > c->maxDpi) c->maxDpi = dpi;
            return TRUE;
        },
        (LPARAM)&ctx);
    if (ctx.monitors > 1 && ctx.minDpi != ctx.maxDpi) {
        LOG_INFO("Mixed DPI %u..%u: one swapchain covers the virtual screen in physical pixels", ctx.minDpi,
                 ctx.maxDpi);
    }
}

HWND findWindowByClass(const wchar_t* className) {
    HWND found = FindWindowW(className, nullptr);
    if (found) return found;
    struct Ctx {
        const wchar_t* cls;
        HWND hwnd;
    } ctx{className, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[128]{};
            GetClassNameW(hwnd, cls, 128);
            if (lstrcmpW(cls, c->cls) == 0) {
                c->hwnd = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&ctx);
    return ctx.hwnd;
}

void logDesktopWindowTree() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    HWND shell = GetShellWindow();
    LOG_INFO("GetShellWindow=%p FindWindow(Progman)=%p raised=%d", (void*)shell, (void*)progman,
             progmanIsRaisedDesktop(progman) ? 1 : 0);

    LOG_INFO("--- Progman subtree (z-order high to low) ---");
    dumpOne(progman, 0, 8);

    LOG_INFO("--- Top-level WorkerW windows ---");
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
            wchar_t cls[64]{};
            GetClassNameW(hwnd, cls, 64);
            if (lstrcmpW(cls, L"WorkerW") == 0) {
                dumpOne(hwnd, 0, 6);
            }
            return TRUE;
        },
        0);

    HWND host = findWindowByClass(L"StalgiaHost");
    HWND surface = findWindowByClass(L"StalgiaSurface");
    if (host) {
        LOG_INFO("--- StalgiaHost ---");
        dumpOne(host, 0, 3);
    } else {
        LOG_INFO("StalgiaHost not found");
    }
    if (surface) {
        LOG_INFO("--- StalgiaSurface ---");
        dumpOne(surface, 0, 3);
    } else {
        LOG_INFO("StalgiaSurface not found");
    }
}
