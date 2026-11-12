#include "host/DesktopIcons.h"

#include "core/Log.h"
#include "core/Util.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <shlobj.h>
#include <shellapi.h>
#include <string>

namespace {

bool g_guard = false;
bool g_captured = false;
bool g_originalVisible = true;
bool g_quiet = false;
int g_lastLogged = -1;  // -1 unknown, 0 shown, 1 hidden

std::wstring markerPath() {
    return appDataDirectory() + L"\\icons.state";
}

bool readMarker(bool* originalVisible) {
    FILE* f = _wfopen(markerPath().c_str(), L"rb");
    if (!f) return false;
    char buf[32]{};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!n) return false;
    if (std::strncmp(buf, "orig=0", 6) == 0) {
        *originalVisible = false;
        return true;
    }
    if (std::strncmp(buf, "orig=1", 6) == 0) {
        *originalVisible = true;
        return true;
    }
    return false;
}

void writeMarker(bool originalVisible) {
    ensureDirectory(appDataDirectory());
    FILE* f = _wfopen(markerPath().c_str(), L"wb");
    if (!f) return;
    const char* text = originalVisible ? "orig=1\n" : "orig=0\n";
    fwrite(text, 1, 7, f);
    fclose(f);
}

void deleteMarker() {
    DeleteFileW(markerPath().c_str());
}
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
std::terminate_handler g_prevTerm = nullptr;

BOOL CALLBACK enumDefView(HWND hwnd, LPARAM lp) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (lstrcmpW(cls, L"Progman") != 0 && lstrcmpW(cls, L"WorkerW") != 0) return TRUE;
    HWND def = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!def) {
        HWND w = FindWindowExW(hwnd, nullptr, L"WorkerW", nullptr);
        while (w && !def) {
            def = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr);
            w = FindWindowExW(hwnd, w, L"WorkerW", nullptr);
        }
    }
    if (def) {
        *reinterpret_cast<HWND*>(lp) = def;
        return FALSE;
    }
    return TRUE;
}

HWND findDefView() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        HWND def = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (def) return def;
        HWND w = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        while (w) {
            def = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr);
            if (def) return def;
            w = FindWindowExW(progman, w, L"WorkerW", nullptr);
        }
    }
    HWND found = nullptr;
    EnumWindows(enumDefView, (LPARAM)&found);
    return found;
}

HWND findListView(HWND defView) {
    if (!defView) return nullptr;
    HWND lv = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
    if (lv) return lv;
    HWND child = GetWindow(defView, GW_CHILD);
    while (child) {
        wchar_t cls[64]{};
        GetClassNameW(child, cls, 64);
        if (lstrcmpW(cls, L"SysListView32") == 0) return child;
        HWND nested = FindWindowExW(child, nullptr, L"SysListView32", nullptr);
        if (nested) return nested;
        child = GetWindow(child, GW_HWNDNEXT);
    }
    return nullptr;
}

bool readHideIconsReg() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD val = 0, sz = sizeof(val), type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"HideIcons", nullptr, &type, (LPBYTE)&val, &sz);
    RegCloseKey(key);
    return st == ERROR_SUCCESS && type == REG_DWORD && val != 0;
}

void writeHideIconsReg(bool hide) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    DWORD val = hide ? 1 : 0;
    RegSetValueExW(key, L"HideIcons", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(key);
}

bool listViewVisible(HWND lv) {
    if (!lv || !IsWindow(lv)) return true;
    return IsWindowVisible(lv) != FALSE;
}

bool queryVisibleNow() {
    HWND def = findDefView();
    HWND lv = findListView(def);
    if (lv) return listViewVisible(lv);
    return !readHideIconsReg();
}

void logIfChanged(bool visible, const char* how) {
    const int state = visible ? 0 : 1;
    if (state == g_lastLogged) return;
    g_lastLogged = state;
    LOG_INFO("Desktop icons %s (%s)", visible ? "shown" : "hidden", how);
}

bool sendToggle(HWND hwnd) {
    if (!hwnd) return false;
    DWORD_PTR result = 0;
    const LRESULT ok =
        SendMessageTimeoutW(hwnd, WM_COMMAND, DesktopIcons::kToggleCmd, 0, SMTO_ABORTIFHUNG, 800, &result);
    return ok != 0;
}

void setVisible(bool wantVisible) {
    HWND def = findDefView();
    HWND lv = findListView(def);
    HWND progman = FindWindowW(L"Progman", nullptr);
    const bool now = queryVisibleNow();
    if (now == wantVisible) {
        logIfChanged(now, "already");
        return;
    }

    const char* how = "WM_COMMAND 0x7402";
    if (sendToggle(def)) {
        // If this timed out but Explorer still handled it, don't send a second toggle.
    } else if (queryVisibleNow() != wantVisible) {
        sendToggle(progman);
    }
    bool after = queryVisibleNow();
    if (after != wantVisible && lv) {
        ShowWindow(lv, wantVisible ? SW_SHOW : SW_HIDE);
        writeHideIconsReg(!wantVisible);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        how = "ShowWindow";
        after = queryVisibleNow();
    }
    if (!g_quiet) logIfChanged(after, how);
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    DesktopIcons::restoreOriginal();
    if (g_prevFilter) return g_prevFilter(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

void onTerminate() {
    DesktopIcons::restoreOriginal();
    if (g_prevTerm) g_prevTerm();
    else std::abort();
}

}  // namespace

void DesktopIcons::installCrashGuard() {
    if (g_guard) return;
    g_guard = true;
    g_prevFilter = SetUnhandledExceptionFilter(&crashFilter);
    g_prevTerm = std::set_terminate(&onTerminate);
}

void DesktopIcons::captureOriginal() {
    if (g_captured) return;
    bool marked = false;
    if (readMarker(&marked)) {
        g_originalVisible = marked;
        LOG_INFO("Desktop icons original state from icons.state: %s (Stalgia hid them last time)",
                 g_originalVisible ? "visible" : "hidden");
    } else {
        g_originalVisible = queryVisibleNow();
        LOG_INFO("Desktop icons original state: %s", g_originalVisible ? "visible" : "hidden");
    }
    g_captured = true;
    g_lastLogged = queryVisibleNow() ? 0 : 1;
}

bool DesktopIcons::originalVisible() { return g_originalVisible; }

bool DesktopIcons::queryVisible() { return queryVisibleNow(); }

void DesktopIcons::apply(bool hide) {
    if (!g_captured) captureOriginal();
    const bool wantVisible = hide ? false : g_originalVisible;
    if (hide) writeMarker(g_originalVisible);
    else if (wantVisible == g_originalVisible) deleteMarker();
    setVisible(wantVisible);
}

void DesktopIcons::restoreOriginal() {
    bool marked = false;
    if (!g_captured && readMarker(&marked)) {
        g_originalVisible = marked;
        g_captured = true;
    }
    if (!g_captured) return;
    const bool quiet = g_quiet;
    g_quiet = true;
    setVisible(g_originalVisible);
    deleteMarker();
    g_quiet = quiet;
}
