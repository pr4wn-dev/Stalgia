#include "host/Autostart.h"

#include "core/Log.h"
#include "core/Util.h"

#include <string>
#include <windows.h>

namespace {
constexpr wchar_t kValueName[] = L"Stalgia";
constexpr wchar_t kLegacyValueName[] = L"DesktopRain";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
}  // namespace

static bool queryRunValue(HKEY key, const wchar_t* name, std::wstring& out) {
    wchar_t buf[MAX_PATH + 8]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LSTATUS st = RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)buf, &size);
    if (st != ERROR_SUCCESS || type != REG_SZ || buf[0] == 0) return false;
    out = buf;
    return true;
}

bool autostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    std::wstring v;
    const bool on = queryRunValue(key, kValueName, v);
    RegCloseKey(key);
    return on;
}

void migrateLegacyAutostart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    std::wstring legacy;
    const bool hadLegacy = queryRunValue(key, kLegacyValueName, legacy);
    std::wstring current;
    const bool hasNew = queryRunValue(key, kValueName, current);
    if (hadLegacy && !hasNew) {
        const LSTATUS st = RegSetValueExW(key, kValueName, 0, REG_SZ, (const BYTE*)legacy.c_str(),
                                          (DWORD)((legacy.size() + 1) * sizeof(wchar_t)));
        if (st == ERROR_SUCCESS) LOG_INFO("Migrated Start with Windows value DesktopRain → Stalgia");
    }
    if (hadLegacy) {
        RegDeleteValueW(key, kLegacyValueName);
        LOG_INFO("Removed legacy HKCU Run value DesktopRain");
    }
    RegCloseKey(key);
}

bool setAutostart(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        LOG_ERR("Cannot open HKCU Run key");
        return false;
    }
    RegDeleteValueW(key, kLegacyValueName);
    LSTATUS st;
    if (enabled) {
        std::wstring cmd = L"\"" + exePath() + L"\"";
        st = RegSetValueExW(key, kValueName, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kValueName);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) {
        LOG_ERR("Autostart registry write failed: %ld", (long)st);
        return false;
    }
    LOG_INFO("Start with Windows: %s", enabled ? "on" : "off");
    return true;
}
