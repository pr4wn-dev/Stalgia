#pragma once

#include <cstdint>
#include <string>
#include <windows.h>

enum class HostMode {
    Desktop,  // attach behind desktop icons
    Window    // ordinary preview window
};

struct ZOrderSnapshot {
    HWND progman = nullptr;
    HWND defView = nullptr;
    HWND worker = nullptr;
    HWND surface = nullptr;
    HWND parent = nullptr;
    HWND belowDefView = nullptr;  // sibling immediately under DefView
    int surfaceRank = -1;         // 0 = topmost child of Progman
    int childCount = 0;
    bool visible = false;
    bool parentIsProgman = false;
    bool belowDefViewIsUs = false;
    bool workerAboveUs = false;
};

class WallpaperHost {
public:
    static constexpr wchar_t kHostClass[] = L"StalgiaHost";
    static constexpr wchar_t kHostTitle[] = L"Stalgia-IPC";
    static constexpr wchar_t kSurfaceClass[] = L"StalgiaSurface";
    static constexpr wchar_t kExitEventName[] = L"Local\\Stalgia.Exit";
    static constexpr wchar_t kMutexName[] = L"Local\\Stalgia.Instance";

    bool create(HINSTANCE instance, HostMode mode, WNDPROC proc, void* user);
    void destroy();
    bool setupSurface();
    void teardownSurface();

    HWND hwnd() const { return m_hwnd; }
    HWND messageHwnd() const { return m_messageHwnd ? m_messageHwnd : m_hwnd; }
    HostMode mode() const { return m_mode; }
    bool raisedDesktop() const { return m_raised; }
    bool usesComposition() const { return m_useComposition; }
    bool compositionNeedsRebind() const { return m_compositionDirty; }
    void clearCompositionDirty() { m_compositionDirty = false; }
    HWND defView() const { return m_defView; }
    HWND workerW() const { return m_worker; }

    HWND ensureRaisedWorkerW();
    bool parentToWorkerW();
    bool recreateLayeredHwnd();
    bool restoreDcompChild();
    bool hwndPath() const { return m_forceHwndSwapchain; }
    bool anchoredToWorker() const { return m_anchorWorker; }
    void markCompositionDirty() { m_compositionDirty = true; }

    void virtualScreen(int& x, int& y, int& w, int& h) const;
    bool syncToVirtualScreen();
    bool reattach();
    int primaryHeight() const;
    float dpiScale() const;

    // Cheap: only SetWindowPos if Explorer shoved WorkerW (or hid us). Returns true if it fixed something.
    bool maintainZOrder();
    ZOrderSnapshot snapshotZOrder() const;
    void logHeartbeat(const char* pausedReason, int frames, HRESULT lastPresent) const;

    void restoreOriginalWallpaper();
    static void restoreWallpaperFromCrash();

    void quietZOrder(DWORD milliseconds);

    static bool alreadyRunning();
    static void releaseInstanceMutex();
    static HWND findRunningHost();
    static bool sendIpc(uint32_t cmd, const char* payload = nullptr);
    static bool requestRunningInstanceExit();
    static bool requestRunningInstancePanel();
    static bool requestRunningInstanceWidget();
    static bool requestRunningInstanceHideWidget();
    static bool requestRunningInstanceIcons(bool hide);
    static bool requestRunningInstanceWallpaper(const std::string& id);
    static constexpr ULONG_PTR kCopyWallpaper = 0x53545750;  // 'STWP'
    static constexpr ULONG_PTR kCopyIpc = 0x53543137;        // 'ST17'

    enum IpcCmd : uint32_t {
        IpcWallpaper = 1,
        IpcIconsHide = 2,
        IpcIconsShow = 3,
        IpcExit = 4,
        IpcPanel = 5,
        IpcWidget = 6,
        IpcHideWidget = 7,
    };

    struct IpcMessage {
        uint32_t cmd = 0;
        char payload[192]{};
    };

private:
    bool createDesktopWindow(HINSTANCE instance, WNDPROC proc);
    bool createPreviewWindow(HINSTANCE instance, WNDPROC proc);
    bool attachRaised(HWND progman, HINSTANCE instance);
    bool attachClassic(HWND progman, HINSTANCE instance);
    void applyZOrder(HWND progman);
    HWND findDefView(HWND progman) const;
    HWND findChildWorkerW(HWND progman) const;
    HWND findClassicWorkerW() const;
    HWND spawnRaisedWorkerW(HWND progman);
    bool createRaisedChild(HWND progman, HINSTANCE instance, bool noredirect);
    void saveOriginalWallpaper();
    void raiseDesktopClassic(HWND progman);
    void logAttachResult() const;

    HostMode m_mode = HostMode::Desktop;
    HWND m_hwnd = nullptr;
    HWND m_messageHwnd = nullptr;
    HWND m_worker = nullptr;
    HWND m_progman = nullptr;
    HWND m_defView = nullptr;
    void* m_user = nullptr;
    HINSTANCE m_instance = nullptr;
    bool m_raised = false;
    bool m_useComposition = false;
    bool m_forceHwndSwapchain = false;
    bool m_anchorWorker = false;
    bool m_compositionDirty = false;
    bool m_fixingZ = false;
    ULONGLONG m_zQuietUntil = 0;
    std::wstring m_originalWallpaper;
    bool m_savedWallpaper = false;

    static HANDLE s_mutex;
};
