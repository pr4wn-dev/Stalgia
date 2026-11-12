#pragma once

#include <windows.h>

class PauseMonitor {
public:
    enum class Cause {
        None,
        User,
        Locked,
        Battery,
        Fullscreen,  // exclusive D3D / presentation / borderless covering the monitor
        Maximized,   // zoomed or near-fullscreen over the work area (taskbar still visible)
        Covered,     // every monitor covered by some window
    };

    void configure(bool pauseOnFullscreen, bool pauseOnCovered, bool pauseOnBatterySaver);

    Cause evaluate();
    static const char* causeName(Cause c);
    const char* lastDetail() const { return m_detail; }

    bool sessionLocked() const { return m_locked; }
    void setSessionLocked(bool locked) { m_locked = locked; }

private:
    Cause rawCause();
    bool exclusiveFullscreen();
    bool borderlessMonitor(HWND fg);
    bool maximizedWork(HWND fg);
    bool allMonitorsCovered() const;
    bool batterySaverOn() const;
    HWND foregroundRoot() const;
    void describe(HWND fg, HWND raw, const char* why);

    bool m_pauseFullscreen = true;
    bool m_pauseCovered = true;
    bool m_pauseBattery = true;
    bool m_locked = false;

    ULONGLONG m_lastCheck = 0;
    Cause m_latched = Cause::None;
    Cause m_pending = Cause::None;
    int m_enterStreak = 0;
    int m_leaveStreak = 0;
    char m_detail[384]{};
};
