#include "host/CrashGuard.h"

#include "core/Log.h"
#include "core/Util.h"
#include "host/DesktopIcons.h"
#include "host/WallpaperHost.h"

#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;
std::terminate_handler g_prevTerm = nullptr;
bool g_installed = false;
bool g_handling = false;

struct MiniInfo {
    DWORD ThreadId;
    EXCEPTION_POINTERS* ExceptionPointers;
    BOOL ClientPointers;
};

using MiniDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD, MiniInfo*, void*, void*);

void writeCrashFiles(EXCEPTION_POINTERS* info, const char* kind) {
    const std::wstring dir = appDataDirectory() + L"\\crash";
    ensureDirectory(appDataDirectory());
    ensureDirectory(dir);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64]{};
    _snwprintf(stamp, 63, L"stalgia-%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    const std::wstring dmpPath = dir + L"\\" + stamp + L".dmp";
    const std::wstring logPath = dir + L"\\" + stamp + L".log";

    if (info) {
        HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
        auto dump = dbg ? (MiniDumpFn)(void*)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
        if (dump) {
            HANDLE file = CreateFileW(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                MiniInfo mi{};
                mi.ThreadId = GetCurrentThreadId();
                mi.ExceptionPointers = info;
                mi.ClientPointers = FALSE;
                dump(GetCurrentProcess(), GetCurrentProcessId(), file, 0, &mi, nullptr, nullptr);
                CloseHandle(file);
            }
        }
    }

    HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return;
    const DWORD code = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionCode : 0;
    void* addr = (info && info->ExceptionRecord) ? info->ExceptionRecord->ExceptionAddress : nullptr;
    char head[320];
    const int hn = std::snprintf(head, sizeof(head),
                                 "Stalgia crash kind=%s code=0x%08X address=%p\r\n"
                                 "Zip this file with %%APPDATA%%\\Stalgia\\stalgia.log and selftest.txt.\r\n\r\n",
                                 kind ? kind : "?", (unsigned)code, addr);
    DWORD wrote = 0;
    if (hn > 0) WriteFile(log, head, (DWORD)hn, &wrote, nullptr);
    char recent[48 * 1024];
    const int n = logx::copyRecent(recent, (int)sizeof(recent));
    if (n > 0) WriteFile(log, recent, (DWORD)n, &wrote, nullptr);
    CloseHandle(log);
}

void restoreAll() {
    DesktopIcons::restoreOriginal();
    WallpaperHost::restoreWallpaperFromCrash();
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    if (!g_handling) {
        g_handling = true;
        restoreAll();
        writeCrashFiles(info, "unhandled");
    }
    if (g_prev) return g_prev(info);
    return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() {
    if (!g_handling) {
        g_handling = true;
        restoreAll();
        writeCrashFiles(nullptr, "terminate");
    }
    if (g_prevTerm) g_prevTerm();
    else std::abort();
}

}  // namespace

void CrashGuard::install() {
    if (g_installed) return;
    g_installed = true;
    g_prev = SetUnhandledExceptionFilter(crashFilter);
    g_prevTerm = std::set_terminate(onTerminate);
}
