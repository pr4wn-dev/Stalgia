#include "core/Log.h"
#include "core/Util.h"
#include "host/Application.h"
#include "host/DesktopDiag.h"
#include "host/Install.h"
#include "host/WallpaperHost.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <shellapi.h>
#include <string>

namespace {

void attachConsole() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
#ifdef _MSC_VER
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);
#else
    FILE* unused = nullptr;
    unused = freopen("CONOUT$", "w", stdout);
    unused = freopen("CONOUT$", "w", stderr);
    unused = freopen("CONIN$", "r", stdin);
    (void)unused;
#endif
}

void initLogging() {
    const std::wstring dir = appDataDirectory();
    ensureDirectory(dir);
    std::wstring path = dir + L"\\stalgia.log";
    logx::init(path);
    if (logx::path()[0] == 0) {
        path = exeDirectory() + L"\\stalgia.log";
        logx::init(path);
    }
}

void printHelp() {
    const char* text =
        "Stalgia — lightweight animated wallpaper for Windows 10/11\n"
        "\n"
        "Usage: Stalgia.exe [options]\n"
        "  --window, -w              Open a preview window instead of the desktop background\n"
        "  --wallpaper NAME          Start with this wallpaper (matrix, starfield, aurora, lava, ape, myco).\n"
        "                            If Stalgia is already running, that instance switches to NAME.\n"
        "  --panel                   Open the control panel (or the running instance's panel)\n"
        "  --widget                  Show the desktop widget (or the running instance's widget)\n"
        "  --hide-widget             Hide the desktop widget on the running instance\n"
        "  --install                 Copy to %LOCALAPPDATA%\\Programs\\Stalgia, add a desktop shortcut, launch\n"
        "  --uninstall               Remove the Run key, shortcuts, and per-user install; restore icons\n"
        "  --diag                    Log the desktop window tree, adapters, and DPI, then exit\n"
        "  --exit                    Tell a running instance to quit and restore the wallpaper\n"
        "  --icons hide|show         Hide or show desktop icons (Explorer View menu)\n"
        "  --selftest                Attach, verify visibility, hit-test the gallery and widget settings, check the lava field, cycle every preset\n"
        "  --capture                 With --selftest, write a PNG of every preset to %APPDATA%\\Stalgia\\capture\n"
        "  --console                 Attach a console for log output\n"
        "  --help, -h                Show this help\n"
        "\n"
        "Example: Stalgia.exe --window --wallpaper ape\n"
        "The desktop widget is shown by default. Use --panel for the full control panel.\n"
        "Logs: %APPDATA%\\Stalgia\\stalgia.log\n"
        "To stop a running instance from a script:  Stalgia.exe --exit\n"
        "Self-test report: %APPDATA%\\Stalgia\\selftest.txt\n";
    MessageBoxA(nullptr, text, "Stalgia", MB_OK | MB_ICONINFORMATION);
}

int runDiag() {
    attachConsole();
    initLogging();
    LOG_INFO("=== Stalgia --diag (read-only, no 0x052C) ===");
    logOsVersion();
    logMonitorAndDpiInfo();
    logDesktopWindowTree();
    LOG_INFO("Diag written to %s", narrow(logx::path()).c_str());
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "Wrote diagnostics to:\n%s\n\nThis did not change the desktop.",
                  narrow(logx::path()).c_str());
    MessageBoxA(nullptr, buf, "Stalgia --diag", MB_OK | MB_ICONINFORMATION);
    logx::close();
    return 0;
}

int runExit() {
    attachConsole();
    initLogging();
    LOG_INFO("=== Stalgia --exit ===");
    const bool ok = WallpaperHost::requestRunningInstanceExit();
    LOG_INFO("--exit signaled=%d", ok ? 1 : 0);
    if (!ok) {
        MessageBoxW(nullptr,
                    L"No running Stalgia instance was found.\n"
                    L"If it is stuck, end Stalgia.exe in Task Manager.",
                    L"Stalgia", MB_OK | MB_ICONINFORMATION);
    }
    logx::close();
    return ok ? 0 : 1;
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool waitForInstanceExit(DWORD timeoutMs) {
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < timeoutMs) {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, WallpaperHost::kMutexName);
        if (!m) return true;
        CloseHandle(m);
        Sleep(200);
    }
    return false;
}

int writeSelftestFile(bool pass, const char* body) {
    const std::wstring dir = appDataDirectory();
    ensureDirectory(dir);
    const std::wstring path = dir + L"\\selftest.txt";
    std::string text = std::string("Stalgia ") + STALGIA_VERSION + " selftest\n";
    if (body) text += body;
    if (!text.empty() && text.back() != '\n') text += '\n';
    text += pass ? "Result: PASS\n" : "Result: FAIL\n";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
        CloseHandle(h);
        LOG_INFO("selftest report written to %s", narrow(path).c_str());
    } else {
        LOG_ERR("selftest: could not write report (%lu)", GetLastError());
    }
    return pass ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    HostMode mode = HostMode::Desktop;
    bool help = false;
    bool console = false;
    bool diag = false;
    bool exitCmd = false;
    bool panelCmd = false;
    bool widgetCmd = false;
    bool hideWidgetCmd = false;
    bool installCmd = false;
    bool uninstallCmd = false;
    bool selftest = false;
    bool capture = false;
    std::string wallpaperOverride;
    int hideIconsCli = -1;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (lstrcmpiW(argv[i], L"--window") == 0 || lstrcmpiW(argv[i], L"-w") == 0) {
                mode = HostMode::Window;
            } else if (lstrcmpiW(argv[i], L"--help") == 0 || lstrcmpiW(argv[i], L"-h") == 0) {
                help = true;
            } else if (lstrcmpiW(argv[i], L"--console") == 0) {
                console = true;
            } else if (lstrcmpiW(argv[i], L"--diag") == 0) {
                diag = true;
            } else if (lstrcmpiW(argv[i], L"--exit") == 0) {
                exitCmd = true;
            } else if (lstrcmpiW(argv[i], L"--panel") == 0) {
                panelCmd = true;
            } else if (lstrcmpiW(argv[i], L"--widget") == 0) {
                widgetCmd = true;
            } else if (lstrcmpiW(argv[i], L"--hide-widget") == 0) {
                hideWidgetCmd = true;
            } else if (lstrcmpiW(argv[i], L"--selftest") == 0) {
                selftest = true;
            } else if (lstrcmpiW(argv[i], L"--capture") == 0) {
                capture = true;
            } else if (lstrcmpiW(argv[i], L"--install") == 0) {
                installCmd = true;
            } else if (lstrcmpiW(argv[i], L"--uninstall") == 0) {
                uninstallCmd = true;
            } else if (lstrcmpiW(argv[i], L"--icons") == 0) {
                if (i + 1 < argc && argv[i + 1][0] != L'-') {
                    const std::string v = lower(narrow(argv[++i]));
                    if (v == "hide") hideIconsCli = 1;
                    else if (v == "show") hideIconsCli = 0;
                }
            } else if (lstrcmpiW(argv[i], L"--wallpaper") == 0 || lstrcmpiW(argv[i], L"--effect") == 0) {
                if (i + 1 < argc) wallpaperOverride = lower(narrow(argv[++i]));
            } else if (wcsstr(argv[i], L"--wallpaper=") == argv[i]) {
                wallpaperOverride = lower(narrow(argv[i] + 12));
            }
        }
        LocalFree(argv);
    }

    if (console) attachConsole();
    if (help) {
        printHelp();
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    if (diag) {
        const int rc = runDiag();
        if (SUCCEEDED(com)) CoUninitialize();
        return rc;
    }
    if (exitCmd) {
        const int rc = runExit();
        if (SUCCEEDED(com)) CoUninitialize();
        return rc;
    }
    if (uninstallCmd) {
        attachConsole();
        initLogging();
        LOG_INFO("=== Stalgia --uninstall ===");
        const int rc = runUninstall();
        logx::close();
        if (SUCCEEDED(com)) CoUninitialize();
        return rc;
    }
    if (installCmd) {
        attachConsole();
        initLogging();
        LOG_INFO("=== Stalgia --install ===");
        const int rc = runInstall();
        logx::close();
        if (SUCCEEDED(com)) CoUninitialize();
        return rc;
    }

    initLogging();
    LOG_INFO("=== Stalgia ===");
    logOsVersion();
    logMonitorAndDpiInfo();

    bool running = mode == HostMode::Desktop && WallpaperHost::alreadyRunning();
    bool relaunchAfterSelftest = false;
    if (running && selftest) {
        relaunchAfterSelftest = true;
        LOG_INFO("selftest: asking the running instance to exit");
        WallpaperHost::requestRunningInstanceExit();
        if (!waitForInstanceExit(12000)) {
            LOG_ERR("selftest: previous instance did not exit");
            const int rc = writeSelftestFile(false, "Attach: FAIL (could not take over running instance)\n");
            logx::close();
            if (SUCCEEDED(com)) CoUninitialize();
            return rc;
        }
        running = WallpaperHost::alreadyRunning();
    }

    if (running) {
        bool ok = true;
        bool any = false;
        if (!wallpaperOverride.empty()) {
            LOG_INFO("Already running; requesting wallpaper '%s' via WM_COPYDATA", wallpaperOverride.c_str());
            ok = WallpaperHost::requestRunningInstanceWallpaper(wallpaperOverride) && ok;
            any = true;
        }
        if (hideIconsCli >= 0) {
            LOG_INFO("Already running; requesting icons %s via WM_COPYDATA", hideIconsCli ? "hide" : "show");
            ok = WallpaperHost::requestRunningInstanceIcons(hideIconsCli != 0) && ok;
            any = true;
        }
        if (panelCmd) {
            LOG_INFO("Already running; requesting control panel via WM_COPYDATA");
            ok = WallpaperHost::requestRunningInstancePanel() && ok;
            any = true;
        }
        if (widgetCmd) {
            LOG_INFO("Already running; requesting desktop widget via WM_COPYDATA");
            ok = WallpaperHost::requestRunningInstanceWidget() && ok;
            any = true;
        }
        if (hideWidgetCmd) {
            LOG_INFO("Already running; requesting hide widget via WM_COPYDATA");
            ok = WallpaperHost::requestRunningInstanceHideWidget() && ok;
            any = true;
        }
        if (!any) {
            LOG_INFO("Already running; requesting desktop widget via WM_COPYDATA");
            ok = WallpaperHost::requestRunningInstanceWidget();
        }
        if (!ok) {
            MessageBoxW(nullptr,
                        L"Stalgia is already running, but the command could not be delivered.\n"
                        L"Look for the tray icon near the clock.\n\n"
                        L"To stop it: Stalgia.exe --exit",
                        L"Stalgia", MB_OK | MB_ICONINFORMATION);
        }
        logx::close();
        if (SUCCEEDED(com)) CoUninitialize();
        return ok ? 0 : 1;
    }

    Application app;
    int rc = 1;
    const bool openPanel = panelCmd;
    if (app.initialize(instance, mode, openPanel, widgetCmd, wallpaperOverride, hideIconsCli, selftest, capture)) {
        rc = app.run();
    } else {
        LOG_ERR("initialize() failed");
        if (selftest) rc = writeSelftestFile(false, "Attach: FAIL (initialize failed)\n");
    }
    app.shutdown();

    if (relaunchAfterSelftest) {
        wchar_t exe[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
            LOG_INFO("selftest: relaunching the previous instance");
            logx::close();
            ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr, SW_SHOW);
        }
    }

    if (SUCCEEDED(com)) CoUninitialize();
    return rc;
}
