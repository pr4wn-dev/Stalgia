#pragma once

#include "core/Config.h"
#include "effect/EffectRegistry.h"
#include "gfx/Renderer.h"
#include "host/PauseMonitor.h"
#include "host/DesktopWidget.h"
#include "host/SettingsPanel.h"
#include "host/ThumbnailStage.h"
#include "host/TrayIcon.h"
#include "host/WallpaperGallery.h"
#include "host/WallpaperHost.h"

#include <memory>
#include <string>
#include <vector>
#include <windows.h>

class IEffect;

class Application : public SettingsPanel::Host {
public:
    bool initialize(HINSTANCE instance, HostMode mode, bool openPanel, bool openWidget,
                    const std::string& effectOverride = {}, int hideIconsCli = -1,
                    bool selftest = false, bool capture = false);
    int run();
    void shutdown();

    static Application* instance() { return s_app; }

    AppSettings& settings() override { return m_config.settings(); }
    const EffectRegistry& registry() const override { return m_registry; }
    void applySettingsLive() override;
    void flushSettings() override;
    void switchEffect(const std::string& id) override;
    void setWallpaperHidden(bool hidden) override;
    void setUserPaused(bool paused) override;
    bool wallpaperHidden() const override { return m_hidden; }
    bool userPaused() const override { return m_userPaused; }
    void openConfigFile() override;
    void setHideDesktopIcons(bool hide) override;
    void exitApp() override { requestQuit(); }
    void openGallery() override;
    void cycleWallpaper(int dir) override;
    bool isFavorite(const std::string& id) const override;
    void toggleFavorite(const std::string& id) override;
    ThumbnailStage* thumbs() override { return &m_thumbs; }

    void openPanel();
    void openWidget();
    void hideWidget();

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void renderFrame(float dt, double time);
    bool recreateRenderer();
    bool loadEffect(const std::string& id);
    void warmSkyShaders();
    void applyPauseConfig();
    void saveConfig();
    void handleCommand(UINT cmd);
    void onDisplayChange();
    void enableDpiAwareness();
    void requestQuit();
    void notifyDesktopChanged();
    void heartbeat();
    void installDesktopHooks();
    void removeDesktopHooks();
    void bindSurfaceUserData();
    void syncUi();
    void armVisibilityWait(const char* why);
    void tickVisibility();
    void checkWallpaperVisibility();
    bool switchToHwndFallback(const char* why);
    int probeWallpaperVisible();
    void drawVisibilityMarker();
    void scheduleReattach(const char* why);
    void runReattach();
    void handleIpc(const WallpaperHost::IpcMessage& msg);
    void tickSelftest();
    void notePresetSwitch(const std::string& id, int preset);
    void awaitSwitchReady(const std::string& id, int preset);
    void advanceSelftestPreset(const std::vector<EffectInfo>& list);
    void finishSelftest(bool pass, const char* extra);
    void writeSelftestReport() const;
    static void CALLBACK winEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                      LONG idChild, DWORD thread, DWORD time);

    static constexpr UINT kZOrderMsg = WM_APP + 21;
    static constexpr UINT kIpcDeferred = WM_APP + 22;
    static constexpr UINT kTimerZOrder = 1;
    static constexpr UINT kTimerHeartbeat = 2;
    static constexpr UINT kTimerReattach = 3;
    static constexpr int kVisMinPresents = 45;
    static constexpr DWORD kVisMinWaitMs = 1500;
    static constexpr DWORD kVisPeriodicMs = 60000;

    static Application* s_app;

    HINSTANCE m_instance = nullptr;
    HostMode m_mode = HostMode::Desktop;
    bool m_running = false;
    bool m_userPaused = false;
    bool m_hidden = false;
    bool m_needsResize = false;
    bool m_needsRecreate = false;
    int m_warmupFrames = 0;
    bool m_visibilityOk = false;
    bool m_drawMarker = false;
    bool m_visUnavailable = false;
    bool m_hwndFallbackLatched = false;
    bool m_triedWorkerParent = false;
    int m_visMiss = 0;
    int m_visUnavail = 0;
    int m_visArmPresent = 0;
    ULONGLONG m_visNextAt = 0;
    ULONGLONG m_reattachAt = 0;
    ULONGLONG m_recreateRetryAt = 0;
    const char* m_reattachWhy = nullptr;
    UINT m_taskbarCreated = 0;
    UINT m_exitMsg = 0;
    UINT m_panelMsg = 0;
    UINT m_widgetMsg = 0;
    UINT m_iconsMsg = 0;
    HANDLE m_exitEvent = nullptr;
    void* m_suspendNotify = nullptr;
    HWINEVENTHOOK m_winEventHookZ = nullptr;
    PauseMonitor::Cause m_lastPause = PauseMonitor::Cause::None;

    bool m_selftest = false;
    bool m_capture = false;
    bool m_settingsDirty = false;
    bool m_autostartApplied = false;
    ULONGLONG m_saveDeadline = 0;
    bool m_stWindow = false;
    bool m_stHaveGrid = false;
    double m_stSwitchMs = 0;
    float m_framePlateMask = 0.0f;
    std::vector<float> m_stGrid;
    int m_stPhase = 0;
    int m_stEffect = 0;
    int m_stPreset = 0;
    int m_stPresentsMark = 0;
    int m_stExitCode = 1;
    bool m_stFail = false;
    ULONGLONG m_stStart = 0;
    ULONGLONG m_stPhaseAt = 0;
    std::string m_stReport;

    Config m_config;
    EffectRegistry m_registry;
    WallpaperHost m_host;
    Renderer m_renderer;
    PauseMonitor m_pause;
    TrayIcon m_tray;
    SettingsPanel m_panel;
    DesktopWidget m_widget;
    WallpaperGallery m_gallery;
    ThumbnailStage m_thumbs;
    uint32_t m_tuneGen = 1;
    std::unique_ptr<IEffect> m_effect;

    double m_time = 0;
    float m_lookX = 0;
    float m_lookY = 0;
};
