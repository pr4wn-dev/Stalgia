#pragma once

#include <windows.h>

// Hide / show Explorer's desktop icon ListView, matching View > Show desktop icons.
class DesktopIcons {
public:
    // Explorer's own toggle (DefView WM_COMMAND). Confirmed via IsWindowVisible.
    static constexpr WPARAM kToggleCmd = 0x7402;

    static void installCrashGuard();
    static void captureOriginal();
    static bool originalVisible();
    static bool queryVisible();

    // hide=true hides icons. hide=false restores the captured Explorer state
    // (does not force icons visible if they were already hidden before Stalgia).
    static void apply(bool hide);
    static void restoreOriginal();

private:
    DesktopIcons() = delete;
};
