#include "host/Install.h"

#include "core/Log.h"
#include "core/Util.h"
#include "host/Autostart.h"
#include "host/DesktopIcons.h"
#include "host/WallpaperHost.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>

int runInstall() {
    const std::wstring src = exePath();
    std::wstring destDir = localAppData() + L"\\Programs\\Stalgia";
    if (!ensureDirectory(destDir)) {
        LOG_ERR("Could not create %s", narrow(destDir).c_str());
        MessageBoxW(nullptr, L"Could not create the Stalgia install folder.", L"Stalgia",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    const std::wstring dest = destDir + L"\\Stalgia.exe";

    if (_wcsicmp(src.c_str(), dest.c_str()) != 0) {
        if (!CopyFileW(src.c_str(), dest.c_str(), FALSE)) {
            const DWORD err = GetLastError();
            LOG_ERR("CopyFile to %s failed: %lu", narrow(dest).c_str(), err);
            MessageBoxW(nullptr,
                        L"Could not copy Stalgia.exe into LocalAppData.\n"
                        L"If a copy is already running, use Stalgia.exe --exit and try again.",
                        L"Stalgia", MB_OK | MB_ICONERROR);
            return 1;
        }
        LOG_INFO("Installed exe to %s", narrow(dest).c_str());
    } else {
        LOG_INFO("Already running from install folder");
    }

    wchar_t* desktop = nullptr;
    std::wstring lnk;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) && desktop) {
        lnk = std::wstring(desktop) + L"\\Stalgia.lnk";
        CoTaskMemFree(desktop);
    }

    if (!lnk.empty()) {
        IShellLinkW* sl = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                      (void**)&sl);
        if (SUCCEEDED(hr) && sl) {
            sl->SetPath(dest.c_str());
            sl->SetWorkingDirectory(destDir.c_str());
            sl->SetDescription(L"Stalgia animated wallpaper");
            sl->SetIconLocation(dest.c_str(), 0);
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
                hr = pf->Save(lnk.c_str(), TRUE);
                pf->Release();
            }
            sl->Release();
            if (SUCCEEDED(hr)) LOG_INFO("Wrote desktop shortcut %s", narrow(lnk).c_str());
            else LOG_WARN("Saving Stalgia.lnk failed: 0x%08X", (unsigned)hr);
        } else {
            LOG_WARN("CoCreateInstance(IShellLink) failed: 0x%08X", (unsigned)hr);
        }
    }

    HINSTANCE launched = ShellExecuteW(nullptr, L"open", dest.c_str(), nullptr, destDir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)launched <= 32) {
        LOG_ERR("ShellExecute installed exe failed: %p", (void*)launched);
        MessageBoxW(nullptr, L"Stalgia was copied, but launching it failed.", L"Stalgia",
                    MB_OK | MB_ICONWARNING);
        return 1;
    }
    LOG_INFO("Launched installed Stalgia");
    LOG_INFO("Start with Windows was not enabled");
    return 0;
}

int runUninstall() {
    if (WallpaperHost::findRunningHost()) {
        LOG_INFO("Uninstall: asking the running instance to exit");
        WallpaperHost::requestRunningInstanceExit();
        for (int i = 0; i < 40 && WallpaperHost::findRunningHost(); ++i) Sleep(250);
        if (WallpaperHost::findRunningHost()) {
            LOG_WARN("Uninstall: instance still running");
        }
    }
    DesktopIcons::restoreOriginal();
    WallpaperHost::restoreWallpaperFromCrash();
    setAutostart(false);

    wchar_t* desktop = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) && desktop) {
        const std::wstring lnk = std::wstring(desktop) + L"\\Stalgia.lnk";
        CoTaskMemFree(desktop);
        if (DeleteFileW(lnk.c_str())) LOG_INFO("Removed %s", narrow(lnk).c_str());
    }
    wchar_t* programs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)) && programs) {
        const std::wstring lnk = std::wstring(programs) + L"\\Stalgia.lnk";
        CoTaskMemFree(programs);
        DeleteFileW(lnk.c_str());
    }

    const std::wstring destDir = localAppData() + L"\\Programs\\Stalgia";
    const std::wstring dest = destDir + L"\\Stalgia.exe";
    const bool self = _wcsicmp(exePath().c_str(), dest.c_str()) == 0;
    if (!self) {
        if (!DeleteFileW(dest.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            LOG_WARN("Could not delete %s (%lu)", narrow(dest).c_str(), GetLastError());
        } else {
            RemoveDirectoryW(destDir.c_str());
            LOG_INFO("Removed install folder %s", narrow(destDir).c_str());
        }
    } else {
        std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + dest + L"\" & rmdir \"" +
                           destDir + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS,
                           nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            LOG_INFO("Scheduled delete of %s", narrow(dest).c_str());
        } else {
            LOG_WARN("Could not schedule delete of the running exe (%lu)", GetLastError());
        }
    }

    MessageBoxW(nullptr,
                L"Stalgia was removed for this user.\n\n"
                L"Start with Windows is off, the desktop shortcut is gone, and desktop icons were restored.\n"
                L"Settings in %APPDATA%\\Stalgia were left in place.",
                L"Stalgia", MB_OK | MB_ICONINFORMATION);
    return 0;
}
