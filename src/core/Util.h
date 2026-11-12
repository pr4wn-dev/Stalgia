#pragma once

#include <cstdint>
#include <string>
#include <windows.h>

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
std::wstring exeDirectory();
std::wstring exePath();
std::wstring roamingAppData();
std::wstring appDataDirectory();
std::wstring legacyAppDataDirectory();
std::wstring localAppData();
bool fileExists(const std::wstring& path);
bool ensureDirectory(const std::wstring& path);
bool copyFile(const std::wstring& src, const std::wstring& dst);
void openFileInDefaultApp(const std::wstring& path);
HICON loadAppIcon(HINSTANCE instance, int cx);
int smallIconPx(HWND hwnd);
int largeIconPx(HWND hwnd);
void applyWindowIcons(HWND hwnd, HINSTANCE instance);
void releaseWindowIcons(HWND hwnd);
COLORREF colorFromHex(uint32_t rgb);
void rgbFromHex(uint32_t rgb, float& r, float& g, float& b);
uint32_t hexFromString(const std::string& s, uint32_t fallback);
std::string hexToString(uint32_t rgb);
