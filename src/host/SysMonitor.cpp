#include "host/SysMonitor.h"

#include "core/ComPtr.h"
#include "core/Log.h"
#include "core/Util.h"
#include "host/SehGuard.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powerbase.h>
#include <psapi.h>

namespace {

constexpr int kHistory = 60;

float clampf01(float v) { return std::max(0.0f, std::min(100.0f, v)); }

bool pdhOk(PDH_STATUS s) { return s == ERROR_SUCCESS; }

bool skipNetName(const wchar_t* n) {
    if (!n) return true;
    auto has = [&](const wchar_t* s) { return wcsstr(n, s) != nullptr; };
    return has(L"isatap") || has(L"Loopback") || has(L"Teredo") || has(L"WAN Miniport") ||
           has(L"Kernel Debug") || has(L"Pseudo") || has(L"VPN") || has(L"Bluetooth");
}

float sumCounterArray(PDH_HCOUNTER c, bool networkFilter, bool* ok) {
    if (ok) *ok = false;
    if (!c) return 0;
    DWORD bytes = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if (st != (PDH_STATUS)PDH_MORE_DATA && st != ERROR_SUCCESS) return 0;
    if (bytes == 0) return 0;
    std::vector<unsigned char> buf(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    count = 0;
    if (!pdhOk(PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, items))) return 0;
    float sum = 0;
    for (DWORD i = 0; i < count; ++i) {
        const float v = (items[i].FmtValue.CStatus == 0 || items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA)
                            ? (float)items[i].FmtValue.doubleValue
                            : 0;
        if (!std::isfinite(v)) continue;
        if (networkFilter) {
            if (skipNetName(items[i].szName)) continue;
            sum += std::max(0.0f, v);
            continue;
        }
        sum = std::max(sum, v);
    }
    if (ok) *ok = true;
    return sum;
}

bool tryFmt(PDH_HCOUNTER c, float* out) {
    if (!c || !out) return false;
    PDH_FMT_COUNTERVALUE v{};
    if (!pdhOk(PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v))) return false;
    if (v.CStatus != 0 && v.CStatus != PDH_CSTATUS_VALID_DATA) return false;
    if (!std::isfinite(v.doubleValue)) return false;
    *out = (float)v.doubleValue;
    return true;
}

unsigned gpuGroup(const wchar_t* name);

bool sumWildGpu(PDH_HCOUNTER c, float* out) {
    if (!c || !out) return false;
    DWORD bytes = 0, count = 0;
    const PDH_STATUS st = PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if ((st != (PDH_STATUS)PDH_MORE_DATA && st != ERROR_SUCCESS) || bytes == 0) return false;
    std::vector<unsigned char> buf(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    count = 0;
    if (!pdhOk(PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, items))) return false;
    struct Acc {
        unsigned id = 0;
        float sum = 0;
        int n = 0;
    };
    Acc acc[8]{};
    int na = 0;
    for (DWORD i = 0; i < count; ++i) {
        const wchar_t* name = items[i].szName ? items[i].szName : L"";
        if (!wcsstr(name, L"engtype_3D") && !wcsstr(name, L"engtype_3d")) continue;
        const bool okStatus =
            items[i].FmtValue.CStatus == 0 || items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA;
        if (!okStatus || !std::isfinite(items[i].FmtValue.doubleValue)) continue;
        const unsigned group = gpuGroup(name);
        int slot = -1;
        for (int g = 0; g < na; ++g) {
            if (acc[g].id == group) {
                slot = g;
                break;
            }
        }
        if (slot < 0 && na < 8) {
            slot = na++;
            acc[slot].id = group;
        }
        if (slot >= 0) {
            acc[slot].sum += std::max(0.0f, (float)items[i].FmtValue.doubleValue);
            acc[slot].n++;
        }
    }
    if (na <= 0) return false;
    float best = 0;
    for (int g = 0; g < na; ++g) {
        if (acc[g].n > 0 && acc[g].sum >= best) best = acc[g].sum;
    }
    *out = clampf01(best);
    return true;
}

unsigned gpuGroup(const wchar_t* name) {
    wchar_t key[80] = L"default";
    if (name) {
        if (const wchar_t* luid = wcsstr(name, L"luid_")) {
            const wchar_t* end = wcsstr(luid, L"_phys_");
            size_t len = end ? (size_t)(end - luid) : wcslen(luid);
            if (len >= 80) len = 79;
            wcsncpy(key, luid, len);
            key[len] = 0;
        }
    }
    unsigned h = 2166136261u;
    for (const wchar_t* p = key; *p; ++p) h = (h ^ (unsigned)*p) * 16777619u;
    return h ? h : 1u;
}

struct AdlActivity {
    int iSize;
    int iEngineClock;
    int iMemoryClock;
    int iVddc;
    int iActivityPercent;
    int iCurrentPerformanceLevel;
    int iCurrentBusSpeed;
    int iCurrentBusLanes;
    int iMaximumBusLanes;
    int iReserved;
};

ULONGLONG ft64(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

float cpuFromTimes(const FILETIME& idle, const FILETIME& kernel, const FILETIME& user, FILETIME& prevIdle,
                   FILETIME& prevKernel, FILETIME& prevUser) {
    const ULONGLONG i = ft64(idle), k = ft64(kernel), u = ft64(user);
    const ULONGLONG pi = ft64(prevIdle), pk = ft64(prevKernel), pu = ft64(prevUser);
    prevIdle = idle;
    prevKernel = kernel;
    prevUser = user;
    const ULONGLONG dIdle = i - pi;
    const ULONGLONG dKrnl = k - pk;
    const ULONGLONG dUser = u - pu;
    const ULONGLONG dTot = dKrnl + dUser;
    if (dTot == 0) return 0;
    // Kernel includes idle.
    const double busy = (double)(dTot - dIdle) / (double)dTot;
    return (float)std::max(0.0, std::min(100.0, busy * 100.0));
}

struct AdlTemp {
    int iSize;
    int iTemperature;  // millidegrees C
};

void* __stdcall adlAlloc(int sz) { return malloc((size_t)std::max(sz, 1)); }

}  // namespace

SysMonitor::SysMonitor() {
    InitializeCriticalSection(&m_lock);
    m_lockInit = true;
}

SysMonitor::~SysMonitor() {
    stop();
    if (m_lockInit && !m_thread) {
        DeleteCriticalSection(&m_lock);
        m_lockInit = false;
    }
}

bool SysMonitor::start(HWND notify, UINT msg) {
    if (m_started) return true;
    m_notify = notify;
    m_msg = msg;
    m_ncpu = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    GetSystemTimes(&m_prevIdle, &m_prevKernel, &m_prevUser);
    FILETIME dummyC{}, dummyE{};
    GetProcessTimes(GetCurrentProcess(), &dummyC, &dummyE, &m_prevProcKernel, &m_prevProcUser);
    m_prevWall = GetTickCount64();
    readVramTotalDxgi();
    m_thread = CreateThread(nullptr, 0, threadProc, this, 0, nullptr);
    if (!m_thread) {
        LOG_WARN("SysMonitor thread failed: %lu", GetLastError());
        if (m_stop) CloseHandle(m_stop);
        if (m_wake) CloseHandle(m_wake);
        m_stop = m_wake = nullptr;
        return false;
    }
    m_started = true;
    LOG_INFO("SysMonitor started ncpu=%u vramTotal=%.1fGB", m_ncpu, m_vramTotalGB);
    return true;
}

void SysMonitor::stop() {
    if (!m_started) return;
    if (m_stop) SetEvent(m_stop);
    bool joined = true;
    if (m_thread) {
        if (WaitForSingleObject(m_thread, 8000) == WAIT_OBJECT_0) {
            CloseHandle(m_thread);
            m_thread = nullptr;
        } else {
            joined = false;
            LOG_ERR("SysMonitor thread did not exit; leaving PDH and ADL loaded");
        }
    }
    if (joined) {
        closeQuery();
        if (m_adl && !m_adlDead) {
            int fault = 0;
            if (m_adlContext && m_adlDestroy2) seh_adl_destroy2(m_adlDestroy2, m_adlContext, &fault);
            else if (m_adlDestroy1) seh_adl_destroy1(m_adlDestroy1, &fault);
            if (!fault) FreeLibrary(m_adl);
        }
        m_adl = nullptr;
        m_adlContext = nullptr;
        m_adlCreate2 = m_adlDestroy2 = m_adlCount = m_adlPmLog = m_adlSupport = nullptr;
        m_adlCreate1 = m_adlDestroy1 = m_adlTemp = m_adlActivity = nullptr;
    }
    if (m_stop) CloseHandle(m_stop);
    if (m_wake) CloseHandle(m_wake);
    m_stop = m_wake = nullptr;
    m_started = false;
}

void SysMonitor::setPaused(bool paused) {
    if (!m_lockInit) return;
    EnterCriticalSection(&m_lock);
    const bool changed = m_paused != paused;
    m_paused = paused;
    LeaveCriticalSection(&m_lock);
    if (changed && m_wake) SetEvent(m_wake);
}

void SysMonitor::setMiniMode(bool mini) {
    if (!m_lockInit) return;
    EnterCriticalSection(&m_lock);
    const bool changed = m_mini != mini;
    m_mini = mini;
    LeaveCriticalSection(&m_lock);
    if (changed && m_wake) SetEvent(m_wake);
}

SysSample SysMonitor::snapshot() const {
    if (!m_lockInit) return {};
    EnterCriticalSection(&m_lock);
    SysSample s = m_sample;
    LeaveCriticalSection(&m_lock);
    return s;
}

DWORD WINAPI SysMonitor::threadProc(void* self) {
    static_cast<SysMonitor*>(self)->run();
    return 0;
}

void SysMonitor::run() {
    HANDLE waits[2] = {m_stop, m_wake};
    while (WaitForMultipleObjects(2, waits, FALSE, 0) != WAIT_OBJECT_0) {
        bool paused, mini;
        EnterCriticalSection(&m_lock);
        paused = m_paused;
        mini = m_mini;
        LeaveCriticalSection(&m_lock);
        if (paused) {
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            continue;
        }
        if (mini) sampleLite();
        else sampleFull();
        if (m_notify && m_msg) PostMessageW(m_notify, m_msg, 0, 0);
        if (WaitForMultipleObjects(2, waits, FALSE, 1000) == WAIT_OBJECT_0) break;
    }
}

bool SysMonitor::openQuery() {
    if (m_queryOk) return true;
    PDH_HQUERY q = nullptr;
    if (!pdhOk(PdhOpenQueryW(nullptr, 0, &q))) {
        LOG_WARN("PdhOpenQuery failed");
        return false;
    }
    auto add = [&](const wchar_t* path, void** slot) {
        PDH_HCOUNTER c = nullptr;
        PDH_STATUS s = PdhAddEnglishCounterW(q, path, 0, &c);
        if (!pdhOk(s)) s = PdhAddCounterW(q, path, 0, &c);
        if (pdhOk(s) && c) {
            *slot = c;
            return true;
        }
        LOG_INFO("PDH skip %ls (0x%08X)", path, (unsigned)s);
        *slot = nullptr;
        return false;
    };
    m_query = q;
    add(L"\\Processor(_Total)\\% Processor Time", &m_cpu);
    add(L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", &m_diskRead);
    add(L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", &m_diskWrite);
    add(L"\\Network Interface(*)\\Bytes Received/sec", &m_netDown);
    add(L"\\Network Interface(*)\\Bytes Sent/sec", &m_netUp);
    add(L"\\GPU Engine(*)\\Utilization Percentage", &m_gpuEng);
    add(L"\\GPU Adapter Memory(*)\\Dedicated Usage", &m_vramDed);
    add(L"\\GPU Adapter Memory(*)\\Dedicated Limit", &m_vramLim);
    PdhCollectQueryData(q);
    m_queryOk = true;
    m_primed = false;
    return true;
}

void SysMonitor::closeQuery() {
    m_gpuFixed.clear();
    m_gpuCached = false;
    m_gpuWildOnly = false;
    m_gpuMiss = 0;
    if (m_query) PdhCloseQuery((PDH_HQUERY)m_query);
    m_query = m_cpu = m_diskRead = m_diskWrite = m_netDown = m_netUp = m_gpuEng = m_vramDed = m_vramLim =
        nullptr;
    m_queryOk = false;
    m_primed = false;
}

void SysMonitor::pushCpu(float pct) {
    pct = std::max(0.0f, std::min(100.0f, pct));
    m_sample.cpuPct = pct;
    m_sample.cpuHistory[m_sample.historyHead] = pct;
    m_sample.historyHead = (m_sample.historyHead + 1) % kHistory;
    if (m_sample.historyCount < kHistory) m_sample.historyCount++;
}

void SysMonitor::readRam() {
    m_sample.ramValid = false;
    MEMORYSTATUSEX mx{};
    mx.dwLength = sizeof(mx);
    if (!GlobalMemoryStatusEx(&mx) || mx.ullTotalPhys == 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_WARN("GlobalMemoryStatusEx failed gle=%lu total=%llu", GetLastError(),
                     (unsigned long long)mx.ullTotalPhys);
        }
        return;
    }
    const double tot = (double)mx.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    const double avail = (double)mx.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
    if (!(tot > 0.05)) return;
    m_sample.ramTotalGB = (float)tot;
    m_sample.ramUsedGB = (float)std::max(0.0, tot - avail);
    m_sample.ramPct = (float)((tot - avail) / tot * 100.0);
    m_sample.ramValid = true;
}

void SysMonitor::readDiskFree() {
    m_sample.diskValid = false;
    wchar_t windir[MAX_PATH]{};
    if (!GetWindowsDirectoryW(windir, MAX_PATH) || windir[1] != L':') return;
    wchar_t root[8] = {windir[0], L':', L'\\', 0};
    ULARGE_INTEGER freeB{}, totalB{};
    if (!GetDiskFreeSpaceExW(root, &freeB, &totalB, nullptr) || totalB.QuadPart == 0) return;
    m_sample.diskFreeGB = (float)((double)freeB.QuadPart / (1024.0 * 1024.0 * 1024.0));
    m_sample.diskTotalGB = (float)((double)totalB.QuadPart / (1024.0 * 1024.0 * 1024.0));
    wcsncpy(m_sample.diskRoot, root, 7);
    m_sample.diskValid = true;
}

void SysMonitor::readSelf() {
    FILETIME create{}, exit{}, kr{}, us{};
    if (GetProcessTimes(GetCurrentProcess(), &create, &exit, &kr, &us)) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG dWall = now - m_prevWall;
        const ULONGLONG dProc = (ft64(kr) - ft64(m_prevProcKernel)) + (ft64(us) - ft64(m_prevProcUser));
        m_prevProcKernel = kr;
        m_prevProcUser = us;
        m_prevWall = now;
        if (dWall > 0) {
            // FILETIME is 100ns. Wall is ms. Process time is sum of all CPUs.
            const double pct = ((double)dProc / 10000.0) / (double)dWall / (double)m_ncpu * 100.0;
            m_sample.selfCpuPct = (float)std::max(0.0, std::min(100.0, pct));
        }
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        m_sample.selfRamMB = (float)((double)pmc.WorkingSetSize / (1024.0 * 1024.0));
    }
}

void SysMonitor::readMhz() {
    struct Ppi {
        ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
    };
    std::vector<Ppi> info(m_ncpu);
    const LONG st = (LONG)CallNtPowerInformation((POWER_INFORMATION_LEVEL)11, nullptr, 0, info.data(),
                                                 (ULONG)(info.size() * sizeof(Ppi)));
    if (st < 0) return;
    double sum = 0;
    int n = 0;
    for (const auto& p : info) {
        if (p.CurrentMhz > 0) {
            sum += p.CurrentMhz;
            n++;
        }
    }
    if (n) m_sample.cpuMhz = (float)(sum / n);
}

void SysMonitor::readVramTotalDxgi() {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return;
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    auto create = (Fn)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
    if (!create) {
        FreeLibrary(dxgi);
        return;
    }
    ComPtr<IDXGIFactory1> fac;
    if (FAILED(create(__uuidof(IDXGIFactory1), fac.put_void()))) {
        FreeLibrary(dxgi);
        return;
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter> ad;
        if (fac->EnumAdapters(i, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(ad->GetDesc(&desc))) continue;
        if (desc.VendorId == 0x1414) continue;  // Microsoft Basic Render
        m_vramTotalGB = (float)((double)desc.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0));
        if (m_vramTotalGB > 0.05f) break;
    }
    FreeLibrary(dxgi);
}

struct AdlPmLog {
    int size;
    int sensors[256];
};

constexpr int kAdlTempEdge = 12;
constexpr int kAdlActivityGfx = 23;
constexpr int kAdlTempHotspot = 31;

bool tempFromSensor(int raw, float* c) {
    if (raw <= 0) return false;
    float v = (float)raw;
    if (v > 1000.0f && v < 150000.0f) v /= 1000.0f;
    if (v < 1.0f || v > 150.0f) return false;
    *c = v;
    return true;
}

void SysMonitor::disableAdl(const char* why) {
    if (!m_adlDead) LOG_WARN("ADL off (%s); GPU%% stays on PDH", why ? why : "unavailable");
    m_adlDead = true;
    m_adlCreate2 = m_adlDestroy2 = m_adlCount = m_adlPmLog = m_adlSupport = nullptr;
    m_adlCreate1 = m_adlDestroy1 = m_adlTemp = m_adlActivity = nullptr;
    m_adlContext = nullptr;
    m_adlCountN = 0;
    if (m_lockInit) {
        EnterCriticalSection(&m_lock);
        m_sample.gpuTempValid = false;
        LeaveCriticalSection(&m_lock);
    }
}

void SysMonitor::dropFixedGpuCounters() {
    for (GpuFixedCounter& c : m_gpuFixed) {
        if (c.handle) PdhRemoveCounter((PDH_HCOUNTER)c.handle);
        c.handle = nullptr;
    }
    m_gpuFixed.clear();
}

bool buildGpuCounterPath(wchar_t* dst, int cap, const wchar_t* instance) {
    if (!dst || cap < 48 || !instance || !instance[0]) return false;
    const wchar_t* prefix = L"\\GPU Engine(";
    const wchar_t* suffix = L")\\Utilization Percentage";
    int n = 0;
    for (const wchar_t* p = prefix; *p && n < cap - 1; ++p) dst[n++] = *p;
    for (const wchar_t* p = instance; *p && n < cap - 1; ++p) dst[n++] = *p;
    for (const wchar_t* p = suffix; *p && n < cap - 1; ++p) dst[n++] = *p;
    dst[n] = 0;
    return *instance && dst[n - 1] == L'e';
}

bool SysMonitor::cacheGpuCounters(float* first) {
    dropFixedGpuCounters();
    if (first) *first = 0;
    if (!m_query || !m_gpuEng) {
        m_gpuCached = true;
        return false;
    }
    const PDH_HCOUNTER wild = (PDH_HCOUNTER)m_gpuEng;
    DWORD bytes = 0, count = 0;
    const PDH_STATUS st = PdhGetFormattedCounterArrayW(wild, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if ((st != (PDH_STATUS)PDH_MORE_DATA && st != ERROR_SUCCESS) || bytes == 0) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_INFO("GPU Engine cache skipped");
        }
        return false;
    }
    std::vector<unsigned char> buf(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    count = 0;
    if (!pdhOk(PdhGetFormattedCounterArrayW(wild, PDH_FMT_DOUBLE, &bytes, &count, items))) return false;

    struct Acc {
        unsigned id = 0;
        float sum = 0;
        int n = 0;
    };
    Acc acc[8]{};
    int na = 0;
    int n3d = 0;
    const PDH_HQUERY q = (PDH_HQUERY)m_query;
    for (DWORD i = 0; i < count; ++i) {
        const wchar_t* name = items[i].szName ? items[i].szName : L"";
        if (!wcsstr(name, L"engtype_3D") && !wcsstr(name, L"engtype_3d")) continue;
        ++n3d;
        const unsigned group = gpuGroup(name);
        wchar_t path[512];
        if (buildGpuCounterPath(path, 512, name)) {
            PDH_HCOUNTER c = nullptr;
            PDH_STATUS add = PdhAddEnglishCounterW(q, path, 0, &c);
            if (!pdhOk(add)) add = PdhAddCounterW(q, path, 0, &c);
            if (pdhOk(add) && c) m_gpuFixed.push_back(GpuFixedCounter{c, group});
        }
        const bool okStatus =
            items[i].FmtValue.CStatus == 0 || items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA;
        if (!okStatus || !std::isfinite(items[i].FmtValue.doubleValue)) continue;
        int slot = -1;
        for (int g = 0; g < na; ++g) {
            if (acc[g].id == group) {
                slot = g;
                break;
            }
        }
        if (slot < 0 && na < 8) {
            slot = na++;
            acc[slot].id = group;
        }
        if (slot >= 0) {
            acc[slot].sum += std::max(0.0f, (float)items[i].FmtValue.doubleValue);
            acc[slot].n++;
        }
    }
    float best = 0;
    for (int g = 0; g < na; ++g) {
        if (acc[g].n > 0 && acc[g].sum >= best) best = acc[g].sum;
    }
    m_gpuCached = true;
    if (m_gpuFixed.empty()) m_gpuWildOnly = true;
    else PdhCollectQueryData(q);
    LOG_INFO("GPU Engine 3D busiest=%.1f cached %d/%d counters across %d adapters", best,
             (int)m_gpuFixed.size(), n3d, na);
    if (na <= 0) return false;
    if (first) *first = clampf01(best);
    return true;
}

bool SysMonitor::readCachedGpu(float* out) {
    if (!out) return false;
    if (m_gpuFixed.empty()) return sumWildGpu((PDH_HCOUNTER)m_gpuEng, out);
    struct Acc {
        unsigned id = 0;
        float sum = 0;
        int n = 0;
    };
    Acc acc[8]{};
    int na = 0;
    bool any = false;
    for (const GpuFixedCounter& c : m_gpuFixed) {
        float v = 0;
        if (!tryFmt((PDH_HCOUNTER)c.handle, &v)) continue;
        any = true;
        int slot = -1;
        for (int g = 0; g < na; ++g) {
            if (acc[g].id == c.group) {
                slot = g;
                break;
            }
        }
        if (slot < 0 && na < 8) {
            slot = na++;
            acc[slot].id = c.group;
        }
        if (slot >= 0) {
            acc[slot].sum += std::max(0.0f, v);
            acc[slot].n++;
        }
    }
    if (!any) return false;
    float best = 0;
    for (int g = 0; g < na; ++g) {
        if (acc[g].n > 0 && acc[g].sum >= best) best = acc[g].sum;
    }
    *out = clampf01(best);
    return true;
}

void SysMonitor::tryAdlTemp() {
    if (m_adlDead) return;
    if (!m_adlTried) {
        m_adlTried = true;
        m_adl = LoadLibraryW(L"atiadlxx.dll");
        if (!m_adl) m_adl = LoadLibraryW(L"atiadlxy.dll");
        if (!m_adl) {
            m_adlDead = true;
            return;
        }
        m_adlCreate2 = (void*)GetProcAddress(m_adl, "ADL2_Main_Control_Create");
        m_adlDestroy2 = (void*)GetProcAddress(m_adl, "ADL2_Main_Control_Destroy");
        m_adlCount = (void*)GetProcAddress(m_adl, "ADL2_Adapter_NumberOfAdapters_Get");
        m_adlPmLog = (void*)GetProcAddress(m_adl, "ADL2_New_QueryPMLogData_Get");
        m_adlSupport = (void*)GetProcAddress(m_adl, "ADL2_Adapter_PMLog_Support_Get");
        m_adlCreate1 = (void*)GetProcAddress(m_adl, "ADL_Main_Control_Create");
        m_adlDestroy1 = (void*)GetProcAddress(m_adl, "ADL_Main_Control_Destroy");
        m_adlTemp = (void*)GetProcAddress(m_adl, "ADL_Overdrive5_Temperature_Get");
        m_adlActivity = (void*)GetProcAddress(m_adl, "ADL_Overdrive5_CurrentActivity_Get");

        const bool pmReady = m_adlCreate2 && m_adlDestroy2 && m_adlCount && m_adlPmLog && m_adlSupport;
        if (!pmReady) {
            LOG_INFO("ADL2 PMLog skipped missing create=%d destroy=%d count=%d query=%d support=%d",
                     m_adlCreate2 ? 1 : 0, m_adlDestroy2 ? 1 : 0, m_adlCount ? 1 : 0, m_adlPmLog ? 1 : 0,
                     m_adlSupport ? 1 : 0);
            m_adlCreate2 = m_adlDestroy2 = m_adlCount = m_adlPmLog = m_adlSupport = nullptr;
        } else {
            void* ctx = nullptr;
            int fault = 0;
            const int rc = seh_adl_create2(m_adlCreate2, adlAlloc, 1, &ctx, &fault);
            if (fault) {
                disableAdl("ADL2_Main_Control_Create fault");
                return;
            }
            if (rc != 0 || !ctx) {
                LOG_INFO("ADL2_Main_Control_Create rc=%d", rc);
                m_adlCreate2 = m_adlDestroy2 = m_adlCount = m_adlPmLog = m_adlSupport = nullptr;
            } else {
                m_adlContext = ctx;
                int n = 0;
                fault = 0;
                const int crc = seh_adl_count(m_adlCount, ctx, &n, &fault);
                if (fault) {
                    disableAdl("ADL2_Adapter_NumberOfAdapters_Get fault");
                    return;
                }
                if (crc != 0 || n < 1) n = 0;
                if (n > 8) n = 8;
                m_adlCountN = n;
                m_adlAdapter = -1;
                LOG_INFO("ADL2 PMLog available adapters=%d", n);
            }
        }

        if (!m_adlContext) {
            const bool od5 = m_adlCreate1 && m_adlDestroy1 && (m_adlTemp || m_adlActivity);
            if (!od5) {
                m_adlCreate1 = m_adlDestroy1 = m_adlTemp = m_adlActivity = nullptr;
                FreeLibrary(m_adl);
                m_adl = nullptr;
                m_adlDead = true;
                LOG_INFO("ADL disabled; GPU%% uses PDH only");
                return;
            }
            int fault = 0;
            const int rc = seh_adl_create1(m_adlCreate1, adlAlloc, 1, &fault);
            if (fault) {
                disableAdl("ADL_Main_Control_Create fault");
                return;
            }
            if (rc != 0) {
                FreeLibrary(m_adl);
                m_adl = nullptr;
                m_adlCreate1 = m_adlDestroy1 = m_adlTemp = m_adlActivity = nullptr;
                m_adlDead = true;
                LOG_INFO("ADL_Main_Control_Create rc=%d; ADL disabled", rc);
                return;
            }
            LOG_INFO("ADL Overdrive5 fallback temp=%d activity=%d", m_adlTemp ? 1 : 0, m_adlActivity ? 1 : 0);
        } else {
            m_adlCreate1 = m_adlDestroy1 = m_adlTemp = m_adlActivity = nullptr;
        }
    }

    if (m_adlDead || !m_adl) return;

    bool gotTemp = false;
    float tempC = 0;
    if (m_adlContext && m_adlPmLog && m_adlSupport && m_adlCountN > 0) {
        for (int i = 0; i < m_adlCountN && !gotTemp; ++i) {
            int support[272]{};
            int fault = 0;
            const int src = seh_adl_pmlog(m_adlSupport, m_adlContext, i, support, &fault);
            if (fault) {
                disableAdl("ADL2_Adapter_PMLog_Support_Get fault");
                return;
            }
            if (src != 0) continue;
            AdlPmLog log{};
            log.size = sizeof(log);
            fault = 0;
            const int rc = seh_adl_pmlog(m_adlPmLog, m_adlContext, i, &log, &fault);
            if (fault) {
                disableAdl("ADL2_New_QueryPMLogData_Get fault");
                return;
            }
            if (rc != 0) continue;
            m_adlAdapter = i;
            if (tempFromSensor(log.sensors[kAdlTempHotspot], &tempC) ||
                tempFromSensor(log.sensors[kAdlTempEdge], &tempC)) {
                gotTemp = true;
            }
        }
    }
    if (!gotTemp && m_adlTemp) {
        AdlTemp t{};
        t.iSize = sizeof(t);
        int fault = 0;
        const int rc = seh_adl_od5_temp(m_adlTemp, 0, 0, &t, &fault);
        if (fault) {
            disableAdl("ADL_Overdrive5_Temperature_Get fault");
            return;
        }
        if (rc == 0 && tempFromSensor(t.iTemperature, &tempC)) gotTemp = true;
    }
    EnterCriticalSection(&m_lock);
    m_sample.gpuTempValid = gotTemp;
    if (gotTemp) m_sample.gpuTempC = tempC;
    LeaveCriticalSection(&m_lock);
}

void SysMonitor::sampleLite() {
    FILETIME idle, kernel, user;
    EnterCriticalSection(&m_lock);
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const float cpu = cpuFromTimes(idle, kernel, user, m_prevIdle, m_prevKernel, m_prevUser);
        pushCpu(cpu);
        m_sample.cpuValid = true;
    } else {
        m_sample.cpuValid = false;
    }
    readRam();
    readSelf();
    m_sample.uptimeMs = GetTickCount64();
    m_sample.ready = m_sample.cpuValid || m_sample.ramValid;
    LeaveCriticalSection(&m_lock);
}

void SysMonitor::sampleFull() {
    if (!openQuery()) {
        sampleLite();
        EnterCriticalSection(&m_lock);
        readDiskFree();
        readMhz();
        m_sample.diskIoValid = false;
        m_sample.netValid = false;
        LeaveCriticalSection(&m_lock);
        return;
    }
    PdhCollectQueryData((PDH_HQUERY)m_query);
    if (!m_primed) {
        m_primed = true;
        sampleLite();
        return;
    }

    FILETIME idle, kernel, user;
    float cpu = 0;
    bool cpuOk = false;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        cpu = cpuFromTimes(idle, kernel, user, m_prevIdle, m_prevKernel, m_prevUser);
        cpuOk = true;
    }
    float pdhCpu = 0;
    if (tryFmt((PDH_HCOUNTER)m_cpu, &pdhCpu)) {
        cpu = pdhCpu;
        cpuOk = true;
    }

    float diskR = 0, diskW = 0;
    const bool diskIo = tryFmt((PDH_HCOUNTER)m_diskRead, &diskR) && tryFmt((PDH_HCOUNTER)m_diskWrite, &diskW);
    diskR /= (1024.0f * 1024.0f);
    diskW /= (1024.0f * 1024.0f);
    bool netDnOk = false, netUpOk = false;
    const float netDn = sumCounterArray((PDH_HCOUNTER)m_netDown, true, &netDnOk);
    const float netUp = sumCounterArray((PDH_HCOUNTER)m_netUp, true, &netUpOk);
    float gpu = 0;
    bool gpuOk = false;
    if (m_gpuWildOnly) {
        gpuOk = sumWildGpu((PDH_HCOUNTER)m_gpuEng, &gpu);
    } else if (!m_gpuCached) {
        gpuOk = cacheGpuCounters(&gpu);
    } else if (!m_gpuFixed.empty() && readCachedGpu(&gpu)) {
        gpuOk = true;
        m_gpuMiss = 0;
    } else if (m_gpuEng) {
        gpuOk = sumWildGpu((PDH_HCOUNTER)m_gpuEng, &gpu);
        if (!m_gpuFixed.empty()) {
            m_gpuMiss++;
            if (m_gpuMiss >= 3) {
                dropFixedGpuCounters();
                m_gpuWildOnly = true;
                LOG_WARN("GPU Engine cached counters stayed unreadable; keeping the wildcard sum");
            }
        }
    }
    if (!gpuOk) tryAdlActivity(&gpu, &gpuOk);
    bool vramUsedOk = false, vramLimOk = false;
    const float vramB = sumCounterArray((PDH_HCOUNTER)m_vramDed, false, &vramUsedOk);
    const float vramLim = sumCounterArray((PDH_HCOUNTER)m_vramLim, false, &vramLimOk);

    EnterCriticalSection(&m_lock);
    if (cpuOk) pushCpu(cpu);
    m_sample.cpuValid = cpuOk;
    readRam();
    readDiskFree();
    readSelf();
    readMhz();
    m_sample.diskIoValid = diskIo;
    if (diskIo) {
        m_sample.diskReadMBs = std::max(0.0f, diskR);
        m_sample.diskWriteMBs = std::max(0.0f, diskW);
    }
    m_sample.netValid = netDnOk && netUpOk;
    if (m_sample.netValid) {
        m_sample.netDownBps = std::max(0.0f, netDn);
        m_sample.netUpBps = std::max(0.0f, netUp);
    }
    if (gpuOk) {
        m_sample.gpuValid = true;
        m_sample.gpuPct = std::max(0.0f, std::min(100.0f, gpu));
    } else {
        m_sample.gpuValid = false;
        m_sample.gpuPct = 0;
    }
    if (vramUsedOk) {
        m_sample.vramValid = true;
        m_sample.vramUsedGB = std::max(0.0f, vramB) / (1024.0f * 1024.0f * 1024.0f);
        if (vramLimOk && vramLim > 1) m_sample.vramTotalGB = vramLim / (1024.0f * 1024.0f * 1024.0f);
        else if (m_vramTotalGB > 0) m_sample.vramTotalGB = m_vramTotalGB;
    } else {
        m_sample.vramValid = false;
    }
    m_sample.uptimeMs = GetTickCount64();
    m_sample.ready = true;
    LeaveCriticalSection(&m_lock);

    tryAdlTemp();
}

void SysMonitor::tryAdlActivity(float* pct, bool* ok) {
    if (!pct || !ok) return;
    *ok = false;
    if (m_adlDead) return;
    if (!m_adlTried) tryAdlTemp();
    if (m_adlDead) return;
    if (m_adlContext && m_adlPmLog) {
        AdlPmLog log{};
        log.size = sizeof(log);
        const int adapter = m_adlAdapter >= 0 ? m_adlAdapter : 0;
        int fault = 0;
        const int rc = seh_adl_pmlog(m_adlPmLog, m_adlContext, adapter, &log, &fault);
        if (fault) {
            disableAdl("ADL2_New_QueryPMLogData_Get fault");
            return;
        }
        if (rc == 0) {
            const int act = log.sensors[kAdlActivityGfx];
            if (act >= 0 && act <= 100) {
                *pct = (float)act;
                *ok = true;
                return;
            }
        }
    }
    if (!m_adlActivity) return;
    AdlActivity a{};
    a.iSize = sizeof(a);
    int fault = 0;
    const int rc = seh_adl_od5_act(m_adlActivity, 0, &a, &fault);
    if (fault) {
        disableAdl("ADL_Overdrive5_CurrentActivity_Get fault");
        return;
    }
    if (rc != 0) return;
    if (a.iActivityPercent < 0 || a.iActivityPercent > 100) return;
    *pct = (float)a.iActivityPercent;
    *ok = true;
}
