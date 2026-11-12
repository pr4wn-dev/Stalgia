#include "core/Util.h"

#include "resource.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring exePath() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

std::wstring exeDirectory() {
    std::wstring p = exePath();
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return p.substr(0, slash);
}

std::wstring roamingAppData() {
    wchar_t* known = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &known)) && known) {
        dir = known;
        CoTaskMemFree(known);
    } else {
        dir = exeDirectory();
    }
    return dir;
}

std::wstring localAppData() {
    wchar_t* known = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) {
        dir = known;
        CoTaskMemFree(known);
    } else {
        dir = exeDirectory();
    }
    return dir;
}

std::wstring appDataDirectory() {
    std::wstring dir = roamingAppData();
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    dir += L"Stalgia";
    return dir;
}

std::wstring legacyAppDataDirectory() {
    std::wstring dir = roamingAppData();
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    dir += L"DesktopRain";
    return dir;
}

bool fileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        ensureDirectory(path.substr(0, slash));
    }
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool copyFile(const std::wstring& src, const std::wstring& dst) {
    return CopyFileW(src.c_str(), dst.c_str(), FALSE) != FALSE;
}

HICON loadAppIcon(HINSTANCE instance, int cx) {
    if (cx < 16) cx = 16;
    HICON icon = nullptr;
    HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
    if (!comctl) comctl = LoadLibraryW(L"comctl32.dll");
    if (comctl) {
        using Fn = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, int, HICON*);
        auto fn = (Fn)(void*)GetProcAddress(comctl, "LoadIconWithScaleDown");
        if (fn && SUCCEEDED(fn(instance, MAKEINTRESOURCEW(IDI_APPICON), cx, cx, &icon)) && icon) return icon;
    }
    icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cx, LR_DEFAULTCOLOR);
    if (icon) return icon;
    HICON shared = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    return shared ? CopyIcon(shared) : nullptr;
}

static UINT windowDpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 && hwnd) {
        using Fn = UINT(WINAPI*)(HWND);
        auto fn = (Fn)(void*)GetProcAddress(user32, "GetDpiForWindow");
        if (fn) {
            UINT d = fn(hwnd);
            if (d) return d;
        }
    }
    if (user32) {
        using SysFn = UINT(WINAPI*)();
        auto sys = (SysFn)(void*)GetProcAddress(user32, "GetDpiForSystem");
        if (sys) {
            UINT d = sys();
            if (d) return d;
        }
    }
    return 96;
}

static int metricForDpi(int index, UINT dpi, int fallback96) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using Fn = int(WINAPI*)(int, UINT);
    auto fn = user32 ? (Fn)(void*)GetProcAddress(user32, "GetSystemMetricsForDpi") : nullptr;
    if (fn) {
        int v = fn(index, dpi);
        if (v > 0) return v;
    }
    return MulDiv(fallback96, (int)dpi, 96);
}

int smallIconPx(HWND hwnd) {
    // GetSystemMetrics(SM_CXSMICON) is already DPI-scaled on a Per-Monitor-aware
    // process; multiplying by dpi/96 double-scaled the tray/title icons. Ask for
    // the metric at this window's DPI instead.
    int v = metricForDpi(SM_CXSMICON, windowDpi(hwnd), 16);
    return v > 0 ? v : 16;
}

int largeIconPx(HWND hwnd) {
    int v = metricForDpi(SM_CXICON, windowDpi(hwnd), 32);
    return v > 0 ? v : 32;
}

static const wchar_t kIconBigProp[] = L"Stalgia.IconBig";
static const wchar_t kIconSmallProp[] = L"Stalgia.IconSmall";

void applyWindowIcons(HWND hwnd, HINSTANCE instance) {
    if (!hwnd) return;
    HICON big = loadAppIcon(instance, largeIconPx(hwnd));
    HICON small = loadAppIcon(instance, smallIconPx(hwnd));
    HICON prevBig = (HICON)GetPropW(hwnd, kIconBigProp);
    HICON prevSmall = (HICON)GetPropW(hwnd, kIconSmallProp);
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
    if (big) SetPropW(hwnd, kIconBigProp, big);
    if (small) SetPropW(hwnd, kIconSmallProp, small);
    if (prevBig && prevBig != big) DestroyIcon(prevBig);
    if (prevSmall && prevSmall != small && prevSmall != prevBig) DestroyIcon(prevSmall);
}

void releaseWindowIcons(HWND hwnd) {
    if (!hwnd) return;
    HICON big = (HICON)RemovePropW(hwnd, kIconBigProp);
    HICON small = (HICON)RemovePropW(hwnd, kIconSmallProp);
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, 0);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, 0);
    if (big) DestroyIcon(big);
    if (small && small != big) DestroyIcon(small);
}

void openFileInDefaultApp(const std::wstring& path) {
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void rgbFromHex(uint32_t rgb, float& r, float& g, float& b) {
    r = ((rgb >> 16) & 0xFF) / 255.0f;
    g = ((rgb >> 8) & 0xFF) / 255.0f;
    b = (rgb & 0xFF) / 255.0f;
}

COLORREF colorFromHex(uint32_t rgb) {
    return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

uint32_t hexFromString(const std::string& s, uint32_t fallback) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) {
        if (c != '#' && !std::isspace(static_cast<unsigned char>(c))) t.push_back(c);
    }
    if (t.size() == 3) {
        // #RGB -> #RRGGBB
        std::string exp;
        for (char c : t) {
            exp.push_back(c);
            exp.push_back(c);
        }
        t.swap(exp);
    }
    if (t.size() != 6) return fallback;
    unsigned int v = 0;
    if (std::sscanf(t.c_str(), "%x", &v) != 1) return fallback;
    return v & 0xFFFFFFu;
}

std::string hexToString(uint32_t rgb) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    return buf;
}
