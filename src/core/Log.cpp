#include "core/Log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <windows.h>

namespace logx {
namespace {

constexpr DWORD kRotateAt = 2u * 1024u * 1024u;
constexpr int kRing = 48 * 1024;

std::mutex g_mu;
HANDLE g_file = INVALID_HANDLE_VALUE;
wchar_t g_path[MAX_PATH]{};
DWORD g_size = 0;
char g_ring[kRing]{};
int g_ringPos = 0;
int g_ringLen = 0;

void ringAppend(const char* text, int len) {
    if (!text || len <= 0) return;
    for (int i = 0; i < len; ++i) {
        g_ring[g_ringPos] = text[i];
        g_ringPos = (g_ringPos + 1) % kRing;
        if (g_ringLen < kRing) g_ringLen++;
    }
}

void openLog() {
    g_file = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    g_size = 0;
    if (g_file != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(g_file, &sz) && sz.QuadPart > 0 && sz.QuadPart < 0x7fffffff) g_size = (DWORD)sz.QuadPart;
    }
}

void rotateUnlocked() {
    if (g_path[0] == 0) return;
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    wchar_t prev[MAX_PATH]{};
    _snwprintf(prev, MAX_PATH - 1, L"%s.1", g_path);
    DeleteFileW(prev);
    MoveFileW(g_path, prev);
    openLog();
}

void writeHandle(const char* text, int len) {
    if (len <= 0) return;
    ringAppend(text, len);
    if (g_file == INVALID_HANDLE_VALUE) return;
    if (g_size >= kRotateAt) rotateUnlocked();
    DWORD written = 0;
    WriteFile(g_file, text, (DWORD)len, &written, nullptr);
    g_size += written;
    FlushFileBuffers(g_file);
}

}  // namespace

const wchar_t* path() {
    return g_path;
}

void init(const std::wstring& filePath) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_path[0] = 0;
    if (filePath.empty()) return;
    wcsncpy(g_path, filePath.c_str(), MAX_PATH - 1);

    // MinGW's `_wfopen(..., L"a, ccs=UTF-8")` writes a 3-byte BOM and then
    // drops `fputs` of narrow text — that's why the first build's log was empty.
    openLog();
    if (g_size >= kRotateAt) rotateUnlocked();
    if (g_file == INVALID_HANDLE_VALUE) {
        char err[256];
        std::snprintf(err, sizeof(err), "ERR  Failed to open log (win32 %lu)\n", GetLastError());
        OutputDebugStringA(err);
    }
}

void write(const char* level, const char* fmt, ...) {
    char body[1536];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1792];
    const int n = std::snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] %s\n",
                                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                                st.wMilliseconds, level, body);

    OutputDebugStringA(line);
    HANDLE con = GetStdHandle(STD_ERROR_HANDLE);
    if (con && con != INVALID_HANDLE_VALUE) {
        DWORD dummy = 0;
        DWORD mode = 0;
        if (GetConsoleMode(con, &mode)) WriteFile(con, line, (DWORD)std::max(0, n), &dummy, nullptr);
    }

    std::lock_guard<std::mutex> lock(g_mu);
    writeHandle(line, n);
}

int copyRecent(char* dst, int cap) {
    if (!dst || cap < 2) return 0;
    dst[0] = 0;
    const bool locked = g_mu.try_lock();
    const int n = g_ringLen < cap - 1 ? g_ringLen : cap - 1;
    const int start = g_ringLen < kRing ? 0 : g_ringPos;
    for (int i = 0; i < n; ++i) dst[i] = g_ring[(start + (g_ringLen - n) + i) % kRing];
    dst[n] = 0;
    if (locked) g_mu.unlock();
    return n;
}

void close() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

}  // namespace logx
