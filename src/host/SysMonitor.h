#pragma once

#include <cstdint>
#include <vector>
#include <windows.h>

// 1 Hz system snapshot for the desktop widget. PDH + cheap Win32 APIs.
// Full GPU/disk/net query runs only while the widget is expanded.
struct SysSample {
    float cpuPct = 0;
    float cpuMhz = 0;
    float ramUsedGB = 0;
    float ramTotalGB = 0;
    float ramPct = 0;
    float gpuPct = 0;
    float vramUsedGB = 0;
    float vramTotalGB = 0;
    bool cpuValid = false;
    bool ramValid = false;
    bool gpuValid = false;
    bool vramValid = false;
    bool diskValid = false;
    bool diskIoValid = false;
    bool netValid = false;
    float diskReadMBs = 0;
    float diskWriteMBs = 0;
    float diskFreeGB = 0;
    float diskTotalGB = 0;
    wchar_t diskRoot[8]{};
    float netDownBps = 0;
    float netUpBps = 0;
    float selfCpuPct = 0;
    float selfRamMB = 0;
    ULONGLONG uptimeMs = 0;
    float cpuHistory[60]{};
    int historyCount = 0;
    int historyHead = 0;
    bool cpuTempValid = false;
    float cpuTempC = 0;
    bool gpuTempValid = false;
    float gpuTempC = 0;
    bool ready = false;
};

class SysMonitor {
public:
    SysMonitor();
    ~SysMonitor();

    bool start(HWND notify, UINT msg);
    void stop();
    void setPaused(bool paused);   // hidden: no sampling
    void setMiniMode(bool mini);   // mini: cheap CPU-only
    SysSample snapshot() const;

private:
    static DWORD WINAPI threadProc(void* self);
    void run();
    bool openQuery();
    void closeQuery();
    void sampleFull();
    void sampleLite();
    void pushCpu(float pct);
    void readRam();
    void readDiskFree();
    void readSelf();
    void readMhz();
    void readVramTotalDxgi();
    void tryAdlTemp();
    void tryAdlActivity(float* pct, bool* ok);
    bool cacheGpuCounters(float* first);
    bool readCachedGpu(float* pct);
    void dropFixedGpuCounters();
    void disableAdl(const char* why);

    HWND m_notify = nullptr;
    UINT m_msg = 0;
    HANDLE m_thread = nullptr;
    HANDLE m_stop = nullptr;
    HANDLE m_wake = nullptr;
    mutable CRITICAL_SECTION m_lock{};
    SysSample m_sample;
    bool m_paused = true;
    bool m_mini = false;
    bool m_started = false;

    void* m_query = nullptr;  // PDH_HQUERY
    void* m_cpu = nullptr;
    void* m_diskRead = nullptr;
    void* m_diskWrite = nullptr;
    void* m_netDown = nullptr;
    void* m_netUp = nullptr;
    void* m_gpuEng = nullptr;
    struct GpuFixedCounter {
        void* handle = nullptr;
        unsigned group = 0;
    };
    std::vector<GpuFixedCounter> m_gpuFixed;
    bool m_gpuCached = false;
    bool m_gpuWildOnly = false;
    int m_gpuMiss = 0;
    void* m_vramDed = nullptr;
    void* m_vramLim = nullptr;
    bool m_queryOk = false;
    bool m_primed = false;

    FILETIME m_prevIdle{}, m_prevKernel{}, m_prevUser{};
    FILETIME m_prevProcKernel{}, m_prevProcUser{};
    ULONGLONG m_prevWall = 0;
    DWORD m_ncpu = 1;
    float m_vramTotalGB = 0;

    HMODULE m_adl = nullptr;
    void* m_adlContext = nullptr;
    void* m_adlCreate2 = nullptr;
    void* m_adlDestroy2 = nullptr;
    void* m_adlCount = nullptr;
    void* m_adlPmLog = nullptr;
    void* m_adlSupport = nullptr;
    void* m_adlCreate1 = nullptr;
    void* m_adlDestroy1 = nullptr;
    void* m_adlTemp = nullptr;
    void* m_adlActivity = nullptr;
    int m_adlAdapter = -1;
    int m_adlCountN = 0;
    bool m_adlTried = false;
    bool m_adlDead = false;
    bool m_lockInit = false;
};
