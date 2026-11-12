#pragma once

#include "core/ComPtr.h"

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <cstdint>
#include <string>
#include <vector>

struct GlyphInstance {
    float rect[4];
    float uv[4];
    float color[4];
};

class Renderer {
public:
    // useComposition: Win11 24H2+ raised desktop. Binds a DirectComposition visual
    // to the HWND and presents a flip-model composition swapchain (GDI/blt presents
    // are not composed for children of WS_EX_NOREDIRECTIONBITMAP Progman).
    bool initialize(HWND hwnd, bool useComposition);
    void release();
    bool rebindComposition();
    void fillRect(int x, int y, int w, int h, float r, float g, float b, float a);
    bool sampleScreenBgra(int screenX, int screenY, uint32_t* bgra);

    bool resize(int width, int height);
    bool beginFrame();
    void clear(float r, float g, float b, float a);
    // Draw into a small texture without presenting. endOffscreen restores the swapchain target.
    void beginOffscreen(ID3D11RenderTargetView* rtv, int width, int height);
    void endOffscreen();
    void drawGlyphs(ID3D11ShaderResourceView* atlas, const GlyphInstance* inst, int count);
    void endFrame();

    struct FullscreenCB {
        float time = 0;
        float width = 1;
        float height = 1;
        float speed = 1;
        float colorA[4] = {1, 1, 1, 1};
        float colorB[4] = {1, 1, 1, 1};
        float density = 1;
        float p0 = 0;
        float p1 = 0;
        float p2 = 0;
        float direction = 0;
        float lookX = 0;
        float lookY = 0;
        float p3 = 0;
        float scroll = 0;
        float quality = 1;
        float padA = 0;
        float padB = 0;
    };

    struct Grade {
        float brightness = 1;
        float saturation = 1;
        float vignette = 0.18f;
        float night = 0;
    };

    // 1 = shader written to out, 0 = compile still running, -1 = compile failed (cached).
    // blockMs 0 returns immediately. The bytecode cache outlives the device.
    int compilePixelShader(const char* src, size_t srcBytes, const char* name, const char* entry,
                           ID3D11PixelShader** out, int blockMs = 0);
    ID3D11PixelShader* fallbackShader() const { return m_fallbackPs.get(); }
    void drawFullscreen(ID3D11PixelShader* ps, const FullscreenCB& cb);
    void drawFullscreen(ID3D11PixelShader* ps, const void* data, UINT bytes);
    void setViewport(int x, int y, int w, int h);
    bool isWarp() const { return m_warp; }
    void beginGpuTime();
    void endGpuTime();
    float lastGpuMs() const { return m_lastGpuMs; }
    // Brightest linear pixel in the scene, and how many pixels clear a lit threshold.
    // A sparse effect (matrix glyphs) fails a patch average and passes this.
    float sampleSceneLuma(int* litPixels = nullptr);
    // Largest step, as a fraction of the band peak, where the Milky Way feather falls
    // toward the sky. A hard diagonal cutoff is well above a smooth gaussian.
    float sampleBandEdge();
    bool capturePng(const std::wstring& path);
    void setGrade(const Grade& grade) { m_grade = grade; }
    // Copy the scene target onto the swapchain. Call after the effect draws and before the visibility marker.
    void compositeToBackbuffer();

    ID3D11Device* device() const { return m_device.get(); }
    ID3D11DeviceContext* context() const { return m_context.get(); }
    ID3D11RenderTargetView* rtv() const { return m_rtv.get(); }

    int width() const { return m_width; }
    int height() const { return m_height; }
    bool deviceLost() const { return m_deviceLost; }
    bool occluded() const { return m_occluded; }
    int presentCount() const { return m_presentCount; }
    HRESULT lastPresentHr() const { return m_lastPresentHr; }
    HRESULT deviceRemovedReason() const;

private:
    bool createDevice();
    bool createNoise();
    void collectGpuTime(int slot);
    bool createHwndSwapchain();
    bool createCompositionSwapchain();
    bool createSwapchainBuffers();
    bool compileShaders();
    bool ensureInstanceCapacity(int count);
    bool ensureScene(int width, int height, bool offscreen);
    bool ensureSceneReadback();
    void composite(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, int width, int height);
    void releaseScenes();
    bool bindDcompVisual();
    void releaseDcomp();
    bool sampleDuplicationBgra(int screenX, int screenY, uint32_t* bgra);
    ComPtr<IDXGIAdapter1> pickAdapter();
    void logAdapters(IDXGIFactory1* factory);

    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_deviceLost = false;
    bool m_warp = false;
    bool m_occluded = false;
    bool m_composition = false;
    int m_presentCount = 0;
    HRESULT m_lastPresentHr = S_OK;
    ID3D11RenderTargetView* m_offRtv = nullptr;
    int m_offW = 0;
    int m_offH = 0;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain> m_swap;
    ComPtr<IDXGISwapChain1> m_swap1;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11VertexShader> m_fsVs;
    ComPtr<ID3D11Buffer> m_fsCb;
    ComPtr<ID3D11InputLayout> m_layout;
    ComPtr<ID3D11Buffer> m_cb;
    ComPtr<ID3D11Buffer> m_instanceBuf;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11BlendState> m_blend;
    ComPtr<ID3D11RasterizerState> m_raster;
    ComPtr<ID3D11DepthStencilState> m_depth;
    ComPtr<ID3D11PixelShader> m_gradePs;
    ComPtr<ID3D11PixelShader> m_fallbackPs;
    ComPtr<ID3D11Texture2D> m_noiseTex;
    ComPtr<ID3D11ShaderResourceView> m_noiseSrv;
    ComPtr<ID3D11SamplerState> m_noiseSamp;
    ComPtr<ID3D11Texture2D> m_lumaStage;
    ComPtr<ID3D11Texture2D> m_captureStage;
    struct GpuSlot {
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> t0;
        ComPtr<ID3D11Query> t1;
        bool pending = false;
    };
    GpuSlot m_gq[3];
    int m_gqSlot = 0;
    float m_lastGpuMs = -1.0f;
    ComPtr<ID3D11Texture2D> m_sceneTex;
    ComPtr<ID3D11RenderTargetView> m_sceneRtv;
    ComPtr<ID3D11ShaderResourceView> m_sceneSrv;
    ComPtr<ID3D11Texture2D> m_offSceneTex;
    ComPtr<ID3D11RenderTargetView> m_offSceneRtv;
    ComPtr<ID3D11ShaderResourceView> m_offSceneSrv;
    int m_sceneW = 0;
    int m_sceneH = 0;
    int m_offSceneW = 0;
    int m_offSceneH = 0;
    bool m_drawScene = false;
    bool m_offUsesScene = false;
    Grade m_grade;

    IUnknown* m_dcompDevice = nullptr;
    IUnknown* m_dcompTarget = nullptr;
    IUnknown* m_dcompVisual = nullptr;

    int m_instanceCapacity = 0;

    struct ShaderCacheEntry {
        std::string key;
        ComPtr<ID3D11PixelShader> ps;
    };
    std::vector<ShaderCacheEntry> m_shaderCache;
};
