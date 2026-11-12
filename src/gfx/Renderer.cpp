#include "gfx/Renderer.h"

#include "core/Log.h"
#include "core/Util.h"
#include "gfx/Shaders.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <wincodec.h>

namespace {

void logBlob(ID3DBlob* blob) {
    if (!blob) return;
    const char* msg = static_cast<const char*>(blob->GetBufferPointer());
    LOG_ERR("HLSL: %s", msg);
}

void releaseUnk(IUnknown*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

const char* featureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
        case D3D_FEATURE_LEVEL_11_0: return "11_0";
        case D3D_FEATURE_LEVEL_10_1: return "10_1";
        case D3D_FEATURE_LEVEL_10_0: return "10_0";
        default: return "?";
    }
}

}  // namespace

bool Renderer::initialize(HWND hwnd, bool useComposition) {
    release();
    m_hwnd = hwnd;
    m_composition = useComposition;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    m_width = std::max(1, (int)(rc.right - rc.left));
    m_height = std::max(1, (int)(rc.bottom - rc.top));
    LOG_INFO("Renderer init hwnd=%p client=%dx%d composition=%d", (void*)hwnd, m_width, m_height,
             useComposition ? 1 : 0);

    if (!createDevice()) return false;

    bool swapOk = false;
    if (useComposition) {
        swapOk = createCompositionSwapchain();
        if (!swapOk) {
            LOG_WARN("Composition swapchain failed, trying HWND swapchain");
            m_composition = false;
            swapOk = createHwndSwapchain();
        }
    } else {
        swapOk = createHwndSwapchain();
        if (!swapOk) {
            LOG_WARN("HWND swapchain failed, trying composition swapchain");
            m_composition = true;
            swapOk = createCompositionSwapchain();
        }
    }
    if (!swapOk) {
        LOG_ERR("No swapchain could be created");
        return false;
    }
    if (!compileShaders()) return false;
    createNoise();
    if (!createSwapchainBuffers()) return false;

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&samp, m_sampler.put());

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    // Premultiplied, so glyph coverage blends in linear space before the grade pass.
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&bd, m_blend.put());

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    m_device->CreateRasterizerState(&rd, m_raster.put());

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    m_device->CreateDepthStencilState(&dd, m_depth.put());

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&cbd, nullptr, m_cb.put());

    D3D11_BUFFER_DESC fscb{};
    fscb.ByteWidth = 512;
    fscb.Usage = D3D11_USAGE_DYNAMIC;
    fscb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    fscb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&fscb, nullptr, m_fsCb.put());

    m_deviceLost = false;
    LOG_INFO("D3D11 renderer ready %dx%d path=%s", m_width, m_height,
             m_composition ? "DirectComposition" : "HWND");
    return true;
}

void Renderer::logAdapters(IDXGIFactory1* factory) {
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(i, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        ad->GetDesc1(&desc);
        LOG_INFO("DXGI adapter[%u] '%s' vendor=0x%04X luid=%08X:%08X vram=%lluMB flags=0x%X", i,
                 narrow(std::wstring(desc.Description)).c_str(), desc.VendorId, desc.AdapterLuid.HighPart,
                 desc.AdapterLuid.LowPart, (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)),
                 desc.Flags);
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, out.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            LOG_INFO("  output[%u] '%s' attached=%d desktop=[%ld,%ld,%ld,%ld] monitor=%p", o,
                     narrow(std::wstring(od.DeviceName)).c_str(), od.AttachedToDesktop ? 1 : 0,
                     od.DesktopCoordinates.left, od.DesktopCoordinates.top, od.DesktopCoordinates.right,
                     od.DesktopCoordinates.bottom, (void*)od.Monitor);
        }
    }
}

ComPtr<IDXGIAdapter1> Renderer::pickAdapter() {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void());
    if (FAILED(hr)) {
        LOG_ERR("CreateDXGIFactory1 failed: 0x%08X", (unsigned)hr);
        return {};
    }
    logAdapters(factory.get());

    HMONITOR target = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    int matchedIndex = -1;
    int firstHwIndex = -1;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(i, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        ad->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (firstHwIndex < 0) firstHwIndex = (int)i;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, out.put()) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            out->GetDesc(&od);
            if (od.Monitor == target) {
                matchedIndex = (int)i;
                LOG_INFO("Picked adapter[%u] '%s' (owns the window's monitor)", i,
                         narrow(std::wstring(desc.Description)).c_str());
                break;
            }
        }
        if (matchedIndex >= 0) break;
    }
    const int use = matchedIndex >= 0 ? matchedIndex : firstHwIndex;
    if (use < 0) return {};
    ComPtr<IDXGIAdapter1> chosen;
    factory->EnumAdapters1((UINT)use, chosen.put());
    if (matchedIndex < 0 && chosen) {
        DXGI_ADAPTER_DESC1 desc{};
        chosen->GetDesc1(&desc);
        LOG_WARN("No adapter owned the window's monitor; using adapter[%d] '%s'", use,
                 narrow(std::wstring(desc.Description)).c_str());
    }
    return chosen;
}

bool Renderer::createDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    auto tryDevice = [&](IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, const char* label) -> bool {
        m_device.reset();
        m_context.reset();
        D3D_FEATURE_LEVEL got{};
        const HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, flags, levels, 3, D3D11_SDK_VERSION,
                                             m_device.put(), &got, m_context.put());
        LOG_INFO("D3D11CreateDevice(%s) hr=0x%08X fl=%s", label, (unsigned)hr, featureLevelName(got));
        if (FAILED(hr)) {
            m_device.reset();
            m_context.reset();
            return false;
        }
        return true;
    };

    ComPtr<IDXGIAdapter1> preferred = pickAdapter();
    bool created = preferred && tryDevice(preferred.get(), D3D_DRIVER_TYPE_UNKNOWN, "preferred");
    if (!created) {
        LUID prefLuid{};
        bool havePref = false;
        if (preferred) {
            DXGI_ADAPTER_DESC1 pref{};
            if (SUCCEEDED(preferred->GetDesc1(&pref))) {
                prefLuid = pref.AdapterLuid;
                havePref = true;
            }
        }
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) {
            for (UINT i = 0; !created; ++i) {
                ComPtr<IDXGIAdapter1> ad;
                if (factory->EnumAdapters1(i, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 desc{};
                ad->GetDesc1(&desc);
                if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                if (havePref && desc.AdapterLuid.HighPart == prefLuid.HighPart &&
                    desc.AdapterLuid.LowPart == prefLuid.LowPart) {
                    continue;
                }
                char label[160];
                std::snprintf(label, sizeof(label), "adapter%u:%s", i,
                              narrow(std::wstring(desc.Description)).c_str());
                created = tryDevice(ad.get(), D3D_DRIVER_TYPE_UNKNOWN, label);
            }
        }
    }
    if (!created) {
        LOG_WARN("Named adapters failed, trying default hardware");
        created = tryDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, "hardware");
    }
    if (!created) {
        LOG_WARN("No D3D11 hardware device; using WARP");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        created = tryDevice(nullptr, D3D_DRIVER_TYPE_WARP, "WARP");
    }
    if (!created) {
        LOG_ERR("D3D11CreateDevice failed on hardware and WARP");
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDev;
    if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice), dxgiDev.put_void()))) {
        ComPtr<IDXGIAdapter> used;
        if (SUCCEEDED(dxgiDev->GetAdapter(used.put()))) {
            DXGI_ADAPTER_DESC desc{};
            used->GetDesc(&desc);
            const std::wstring name(desc.Description);
            m_warp = desc.VendorId == 0x1414 || name.find(L"WARP") != std::wstring::npos ||
                     name.find(L"Basic Render") != std::wstring::npos;
            LOG_INFO("Active D3D adapter '%s' vendor=0x%04X warp=%d", narrow(name).c_str(), desc.VendorId,
                     m_warp ? 1 : 0);
        }
    }
    return true;
}

bool Renderer::createHwndSwapchain() {
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_device->QueryInterface(__uuidof(IDXGIDevice), dxgiDev.put_void()))) return false;
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(adapter.put());
    ComPtr<IDXGIFactory2> factory2;
    HRESULT hr = adapter->GetParent(__uuidof(IDXGIFactory2), factory2.put_void());

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = (UINT)m_width;
    scd.Height = (UINT)m_height;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    if (SUCCEEDED(hr) && factory2) {
        hr = factory2->CreateSwapChainForHwnd(m_device.get(), m_hwnd, &scd, nullptr, nullptr, m_swap1.put());
        LOG_INFO("CreateSwapChainForHwnd(FLIP_DISCARD) hr=0x%08X", (unsigned)hr);
        if (FAILED(hr)) {
            scd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
            hr = factory2->CreateSwapChainForHwnd(m_device.get(), m_hwnd, &scd, nullptr, nullptr, m_swap1.put());
            LOG_INFO("CreateSwapChainForHwnd(SEQUENTIAL) hr=0x%08X", (unsigned)hr);
        }
        if (SUCCEEDED(hr)) {
            m_swap1->QueryInterface(__uuidof(IDXGISwapChain), m_swap.put_void());
            return true;
        }
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = (UINT)m_width;
    sd.BufferDesc.Height = (UINT)m_height;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGIFactory> factory1;
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory), factory1.put_void());
    if (!factory1) return false;
    hr = factory1->CreateSwapChain(m_device.get(), &sd, m_swap.put());
    LOG_INFO("CreateSwapChain(DISCARD) hr=0x%08X", (unsigned)hr);
    return SUCCEEDED(hr);
}

void Renderer::releaseDcomp() {
    releaseUnk(m_dcompVisual);
    releaseUnk(m_dcompTarget);
    releaseUnk(m_dcompDevice);
}

bool Renderer::bindDcompVisual() {
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_device->QueryInterface(__uuidof(IDXGIDevice), dxgiDev.put_void()))) return false;

    IDCompositionDevice* dcomp = nullptr;
    HRESULT hr = DCompositionCreateDevice(dxgiDev.get(), __uuidof(IDCompositionDevice), (void**)&dcomp);
    LOG_INFO("DCompositionCreateDevice hr=0x%08X", (unsigned)hr);
    if (FAILED(hr) || !dcomp) return false;

    // FALSE = DComp layer behind this hwnd's children (the wallpaper slot). TRUE
    // is the overlay slot and can lose to DefView in raised mode. Try FALSE first.
    IDCompositionTarget* target = nullptr;
    BOOL usedTopmost = FALSE;
    hr = dcomp->CreateTargetForHwnd(m_hwnd, FALSE, &target);
    LOG_INFO("CreateTargetForHwnd topmost=0 hr=0x%08X", (unsigned)hr);
    if (FAILED(hr) || !target) {
        hr = dcomp->CreateTargetForHwnd(m_hwnd, TRUE, &target);
        usedTopmost = TRUE;
        LOG_INFO("CreateTargetForHwnd topmost=1 hr=0x%08X", (unsigned)hr);
    }
    if (FAILED(hr) || !target) {
        dcomp->Release();
        return false;
    }

    IDCompositionVisual* visual = nullptr;
    hr = dcomp->CreateVisual(&visual);
    if (FAILED(hr) || !visual) {
        target->Release();
        dcomp->Release();
        return false;
    }
    hr = visual->SetContent(m_swap1 ? (IUnknown*)m_swap1.get() : (IUnknown*)m_swap.get());
    LOG_INFO("DComp SetContent hr=0x%08X topmost=%d", (unsigned)hr, usedTopmost ? 1 : 0);
    hr = target->SetRoot(visual);
    LOG_INFO("DComp SetRoot hr=0x%08X", (unsigned)hr);
    hr = dcomp->Commit();
    LOG_INFO("DComp Commit hr=0x%08X", (unsigned)hr);
    dcomp->WaitForCommitCompletion();

    releaseDcomp();
    m_dcompDevice = dcomp;
    m_dcompTarget = target;
    m_dcompVisual = visual;
    return true;
}

bool Renderer::rebindComposition() {
    if (!m_composition || !m_hwnd || !m_device) return false;
    LOG_INFO("Rebinding DComp target for hwnd=%p after reparent/reinsert", (void*)m_hwnd);
    releaseDcomp();
    if (!bindDcompVisual()) {
        LOG_ERR("DComp rebind failed");
        return false;
    }
    return true;
}

void Renderer::fillRect(int x, int y, int w, int h, float r, float g, float b, float a) {
    if (!m_context || !m_rtv || w <= 0 || h <= 0) return;
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(m_context->QueryInterface(__uuidof(ID3D11DeviceContext1), ctx1.put_void())) || !ctx1) return;
    D3D11_RECT rc{(LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h)};
    const FLOAT c[4] = {r, g, b, a};
    ctx1->ClearView(m_rtv.get(), c, &rc, 1);
}

namespace {

bool outputCovers(IDXGIOutput* output, int x, int y, DXGI_OUTPUT_DESC* od) {
    DXGI_OUTPUT_DESC d{};
    if (!output || FAILED(output->GetDesc(&d)) || !d.AttachedToDesktop) return false;
    if (x < d.DesktopCoordinates.left || x >= d.DesktopCoordinates.right) return false;
    if (y < d.DesktopCoordinates.top || y >= d.DesktopCoordinates.bottom) return false;
    if (od) *od = d;
    return true;
}

bool sampleGdiBgra(int screenX, int screenY, uint32_t* bgra) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, 1, 1);
    HGDIOBJ old = mem && bmp ? SelectObject(mem, bmp) : nullptr;
    bool ok = false;
    if (mem && bmp && old && BitBlt(mem, 0, 0, 1, 1, screen, screenX, screenY, SRCCOPY | CAPTUREBLT)) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 1;
        bi.bmiHeader.biHeight = -1;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        uint32_t px = 0;
        if (GetDIBits(mem, bmp, 0, 1, &px, &bi, DIB_RGB_COLORS)) {
            *bgra = px;
            ok = true;
        }
    }
    if (mem && old) SelectObject(mem, old);
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

}  // namespace

bool Renderer::sampleScreenBgra(int screenX, int screenY, uint32_t* bgra) {
    if (!bgra) return false;
    if (m_device && m_context && sampleDuplicationBgra(screenX, screenY, bgra)) return true;
    if (sampleGdiBgra(screenX, screenY, bgra)) {
        LOG_INFO("Visibility probe: GDI BitBlt 1x1 at (%d,%d) bgra=#%08X", screenX, screenY, (unsigned)*bgra);
        return true;
    }
    LOG_WARN("Visibility probe unavailable (duplication and GDI BitBlt both failed)");
    return false;
}

bool Renderer::sampleDuplicationBgra(int screenX, int screenY, uint32_t* bgra) {
    if (!m_device || !m_context || !bgra) return false;
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_device->QueryInterface(__uuidof(IDXGIDevice), dxgiDev.put_void()))) return false;
    ComPtr<IDXGIAdapter> deviceAdapter;
    if (FAILED(dxgiDev->GetAdapter(deviceAdapter.put())) || !deviceAdapter) return false;

    ComPtr<IDXGIOutput> found;
    DXGI_OUTPUT_DESC od{};
    bool onDeviceAdapter = false;
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIOutput> output;
        HRESULT er = deviceAdapter->EnumOutputs(i, output.put());
        if (er == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(er) || !output) continue;
        if (outputCovers(output.get(), screenX, screenY, &od)) {
            found = std::move(output);
            onDeviceAdapter = true;
            break;
        }
    }
    if (!found) {
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) {
            for (UINT a = 0;; ++a) {
                ComPtr<IDXGIAdapter1> ad;
                if (factory->EnumAdapters1(a, ad.put()) == DXGI_ERROR_NOT_FOUND) break;
                for (UINT i = 0;; ++i) {
                    ComPtr<IDXGIOutput> output;
                    HRESULT er = ad->EnumOutputs(i, output.put());
                    if (er == DXGI_ERROR_NOT_FOUND) break;
                    if (FAILED(er) || !output) continue;
                    if (outputCovers(output.get(), screenX, screenY, &od)) {
                        DXGI_ADAPTER_DESC desc{};
                        ad->GetDesc(&desc);
                        LOG_WARN("Visibility probe: output at (%d,%d) is on '%s', not the D3D device adapter",
                                 screenX, screenY, narrow(std::wstring(desc.Description)).c_str());
                        return false;
                    }
                }
            }
        }
        LOG_WARN("Visibility probe: no DXGI output covers (%d,%d)", screenX, screenY);
        return false;
    }
    (void)onDeviceAdapter;

    ComPtr<IDXGIOutput1> out1;
    if (FAILED(found->QueryInterface(__uuidof(IDXGIOutput1), out1.put_void())) || !out1) {
        LOG_WARN("Visibility probe: IDXGIOutput1 unavailable");
        return false;
    }

    const int lx = screenX - od.DesktopCoordinates.left;
    const int ly = screenY - od.DesktopCoordinates.top;
    if (lx < 0 || ly < 0) return false;

    for (int dupTry = 0; dupTry < 2; ++dupTry) {
        ComPtr<IDXGIOutputDuplication> dup;
        HRESULT hr = out1->DuplicateOutput(m_device.get(), dup.put());
        if (FAILED(hr) || !dup) {
            LOG_WARN("DuplicateOutput hr=0x%08X (try %d)", (unsigned)hr, dupTry + 1);
            return false;
        }
        bool lost = false;
        for (int attempt = 0; attempt < 8 && !lost; ++attempt) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> res;
            hr = dup->AcquireNextFrame(120, &info, res.put());
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (hr == DXGI_ERROR_ACCESS_LOST) {
                LOG_WARN("AcquireNextFrame ACCESS_LOST, recreating duplication");
                lost = true;
                break;
            }
            if (FAILED(hr) || !res) {
                LOG_WARN("AcquireNextFrame hr=0x%08X", (unsigned)hr);
                return false;
            }
            if (info.AccumulatedFrames == 0) {
                dup->ReleaseFrame();
                continue;
            }
            ComPtr<ID3D11Texture2D> tex;
            hr = res->QueryInterface(__uuidof(ID3D11Texture2D), tex.put_void());
            if (FAILED(hr) || !tex) {
                dup->ReleaseFrame();
                return false;
            }
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            if (lx >= (int)td.Width || ly >= (int)td.Height) {
                dup->ReleaseFrame();
                return false;
            }
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = 1;
            sd.Height = 1;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = td.Format;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(m_device->CreateTexture2D(&sd, nullptr, staging.put()))) {
                dup->ReleaseFrame();
                return false;
            }
            D3D11_BOX box{};
            box.left = (UINT)lx;
            box.top = (UINT)ly;
            box.front = 0;
            box.right = box.left + 1;
            box.bottom = box.top + 1;
            box.back = 1;
            m_context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);
            dup->ReleaseFrame();
            D3D11_MAPPED_SUBRESOURCE map{};
            if (FAILED(m_context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map)) || !map.pData) return false;
            const auto* px = static_cast<const unsigned char*>(map.pData);
            *bgra = (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16) | ((uint32_t)px[3] << 24);
            m_context->Unmap(staging.get(), 0);
            LOG_INFO("Visibility probe: duplication 1x1 (%d,%d) frames=%u bgra=#%08X", screenX, screenY,
                     info.AccumulatedFrames, (unsigned)*bgra);
            return true;
        }
        if (!lost) break;
    }
    LOG_WARN("Visibility probe: duplication produced no frame with AccumulatedFrames>0");
    return false;
}

bool Renderer::createCompositionSwapchain() {
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(m_device->QueryInterface(__uuidof(IDXGIDevice), dxgiDev.put_void()))) return false;
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(adapter.put());
    ComPtr<IDXGIFactory2> factory2;
    HRESULT hr = adapter->GetParent(__uuidof(IDXGIFactory2), factory2.put_void());
    if (FAILED(hr) || !factory2) {
        LOG_ERR("IDXGIFactory2 unavailable: 0x%08X", (unsigned)hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = (UINT)m_width;
    scd.Height = (UINT)m_height;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.Scaling = DXGI_SCALING_STRETCH;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory2->CreateSwapChainForComposition(m_device.get(), &scd, nullptr, m_swap1.put());
    LOG_INFO("CreateSwapChainForComposition %dx%d hr=0x%08X", m_width, m_height, (unsigned)hr);
    if (FAILED(hr)) return false;
    m_swap1->QueryInterface(__uuidof(IDXGISwapChain), m_swap.put_void());
    return bindDcompVisual();
}

bool Renderer::compileShaders() {
    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> err;

    HRESULT hr = D3DCompile(kSpriteHLSL, sizeof(kSpriteHLSL), "sprite.hlsl", nullptr, nullptr, "VSMain",
                            "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vsBlob.put(), err.put());
    if (FAILED(hr)) {
        logBlob(err.get());
        return false;
    }
    err.reset();
    hr = D3DCompile(kSpriteHLSL, sizeof(kSpriteHLSL), "sprite.hlsl", nullptr, nullptr, "PSMain",
                    "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, psBlob.put(), err.put());
    if (FAILED(hr)) {
        logBlob(err.get());
        return false;
    }

    if (FAILED(m_device->CreateVertexShader(vsBlob.get()->GetBufferPointer(), vsBlob.get()->GetBufferSize(),
                                            nullptr, m_vs.put()))) {
        return false;
    }
    if (FAILED(m_device->CreatePixelShader(psBlob.get()->GetBufferPointer(), psBlob.get()->GetBufferSize(),
                                           nullptr, m_ps.put()))) {
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"IRECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"IUV", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"ICOL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    hr = m_device->CreateInputLayout(layout, 3, vsBlob.get()->GetBufferPointer(), vsBlob.get()->GetBufferSize(),
                                     m_layout.put());
    if (FAILED(hr)) {
        LOG_ERR("CreateInputLayout failed: 0x%08X", (unsigned)hr);
        return false;
    }

    vsBlob.reset();
    err.reset();
    hr = D3DCompile(kFullscreenHLSL, sizeof(kFullscreenHLSL), "fullscreen.hlsl", nullptr, nullptr, "VSMain",
                    "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vsBlob.put(), err.put());
    if (FAILED(hr)) {
        logBlob(err.get());
        return false;
    }
    if (FAILED(m_device->CreateVertexShader(vsBlob.get()->GetBufferPointer(), vsBlob.get()->GetBufferSize(),
                                            nullptr, m_fsVs.put()))) {
        return false;
    }
    ID3D11PixelShader* grade = nullptr;
    if (compilePixelShader(kGradeHLSL, sizeof(kGradeHLSL), "grade.hlsl", "PSGrade", &grade, 8000) != 1 || !grade) {
        LOG_ERR("Grade shader failed");
        return false;
    }
    m_gradePs = ComPtr<ID3D11PixelShader>(grade);
    ID3D11PixelShader* fb = nullptr;
    if (compilePixelShader(kFullscreenHLSL, sizeof(kFullscreenHLSL), "fullscreen.hlsl", "PSFallback", &fb, 8000) == 1 &&
        fb) {
        m_fallbackPs = ComPtr<ID3D11PixelShader>(fb);
    }
    return true;
}

namespace {

uint64_t fnv64(const char* s, size_t n) {
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Process-lifetime bytecode. D3DCompile runs on a background thread. A new device
// only calls CreatePixelShader. Failures are remembered so a broken shader is not
// recompiled every frame.
class BytecodeCache {
public:
    static BytecodeCache& get() {
        static BytecodeCache c;
        return c;
    }

    void request(const std::string& key, const std::string& src, const std::string& name, const std::string& entry) {
        std::lock_guard<std::mutex> lock(mu_);
        startLocked();
        if (blobs_.count(key) || queued_.count(key)) return;
        if (loadFile(key)) return;
        queued_.insert(key); // set
        queue_.push_back(Job{key, src, name, entry});
        cv_.notify_one();
    }

    int poll(const std::string& key, std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = blobs_.find(key);
        if (it == blobs_.end()) return 0;
        if (it->second.failed) return -1;
        out = it->second.bytes;
        return 1;
    }

    int waitFor(const std::string& key, std::vector<uint8_t>& out, int ms) {
        std::unique_lock<std::mutex> lock(mu_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (true) {
            auto it = blobs_.find(key);
            if (it != blobs_.end()) {
                if (it->second.failed) return -1;
                out = it->second.bytes;
                return 1;
            }
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout && !blobs_.count(key)) return 0;
        }
    }

private:
    struct Job {
        std::string key, src, name, entry;
    };
    struct Blob {
        std::vector<uint8_t> bytes;
        bool failed = false;
    };

    BytecodeCache() = default;
    ~BytecodeCache() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void startLocked() {
        if (started_) return;
        started_ = true;
        worker_ = std::thread([this] { threadMain(); });
    }

    std::wstring fileFor(const std::string& key) const {
        uint64_t h = fnv64(key.data(), key.size());
        wchar_t name[64];
        swprintf(name, 64, L"%016llx.cso", (unsigned long long)h);
        return appDataDirectory() + L"\\shadercache\\" + name;
    }

    bool loadFile(const std::string& key) {
        const std::wstring path = fileFor(key);
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return false;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n < 64 || n > 8 * 1024 * 1024) {
            fclose(f);
            return false;
        }
        Blob b;
        b.bytes.resize((size_t)n);
        if (fread(b.bytes.data(), 1, b.bytes.size(), f) != b.bytes.size()) {
            fclose(f);
            return false;
        }
        fclose(f);
        blobs_[key] = std::move(b);
        LOG_INFO("Shader bytecode cache hit %s", key.c_str());
        return true;
    }

    void saveFile(const std::string& key, const std::vector<uint8_t>& bytes) {
        const std::wstring path = fileFor(key);
        const std::wstring dir = appDataDirectory() + L"\\shadercache";
        ensureDirectory(dir);
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return;
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
    }

    void threadMain() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                job = std::move(queue_.back());
                queue_.pop_back();
            }
            ComPtr<ID3DBlob> blob;
            ComPtr<ID3DBlob> err;
            const HRESULT hr = D3DCompile(job.src.data(), job.src.size(), job.name.c_str(), nullptr, nullptr,
                                          job.entry.c_str(), "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(),
                                          err.put());
            Blob stored;
            if (FAILED(hr) || !blob) {
                logBlob(err.get());
                stored.failed = true;
                LOG_ERR("Background compile failed: %s", job.entry.c_str());
            } else {
                const auto* p = static_cast<const uint8_t*>(blob->GetBufferPointer());
                stored.bytes.assign(p, p + blob->GetBufferSize());
                saveFile(job.key, stored.bytes);
                LOG_INFO("Compiled pixel shader %s (%u bytes)", job.entry.c_str(), (unsigned)stored.bytes.size());
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                blobs_[job.key] = std::move(stored);
                queued_.erase(job.key);
            }
            cv_.notify_all();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::thread worker_;
    bool started_ = false;
    bool stop_ = false;
    std::vector<Job> queue_;
    std::unordered_map<std::string, Blob> blobs_;
    std::unordered_set<std::string> queued_;
};

}  // namespace

int Renderer::compilePixelShader(const char* src, size_t srcBytes, const char* name, const char* entry,
                                 ID3D11PixelShader** out, int blockMs) {
    if (!m_device || !src || !out || !entry) return -1;
    const uint64_t h = fnv64(src, srcBytes);
    char hex[32];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);
    const std::string key = std::string(entry) + "\n" + hex;
    for (auto& cached : m_shaderCache) {
        if (cached.key == key && cached.ps) {
            cached.ps->AddRef();
            *out = cached.ps.get();
            return 1;
        }
    }
    auto& cache = BytecodeCache::get();
    // The source includes the trailing NUL from sizeof(); D3DCompile accepts it.
    cache.request(key, std::string(src, srcBytes), name ? name : "", entry);
    std::vector<uint8_t> bytes;
    int st = blockMs > 0 ? cache.waitFor(key, bytes, blockMs) : cache.poll(key, bytes);
    if (st != 1) return st;
    HRESULT hr = m_device->CreatePixelShader(bytes.data(), bytes.size(), nullptr, out);
    if (FAILED(hr) || !*out) {
        LOG_ERR("CreatePixelShader(%s) failed: 0x%08X", entry, (unsigned)hr);
        return -1;
    }
    (*out)->AddRef();
    ShaderCacheEntry cached;
    cached.key = key;
    cached.ps = ComPtr<ID3D11PixelShader>(*out);
    m_shaderCache.push_back(std::move(cached));
    return 1;
}

void Renderer::drawFullscreen(ID3D11PixelShader* ps, const FullscreenCB& cb) {
    drawFullscreen(ps, &cb, sizeof(cb));
}

void Renderer::drawFullscreen(ID3D11PixelShader* ps, const void* data, UINT bytes) {
    if (!m_context || !ps || !m_fsVs || !m_fsCb || !data || bytes == 0) return;

    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(m_context->Map(m_fsCb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        const UINT n = bytes < 512 ? bytes : 512;
        std::memcpy(map.pData, data, n);
        if (n < 512) std::memset(static_cast<char*>(map.pData) + n, 0, 512 - n);
        m_context->Unmap(m_fsCb.get(), 0);
    }

    ID3D11Buffer* noneVb = nullptr;
    UINT stride = 0, offset = 0;
    m_context->IASetVertexBuffers(0, 1, &noneVb, &stride, &offset);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_fsVs.get(), nullptr, 0);
    m_context->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cbuf = m_fsCb.get();
    m_context->VSSetConstantBuffers(0, 1, &cbuf);
    m_context->PSSetConstantBuffers(0, 1, &cbuf);
    if (m_noiseSrv && m_noiseSamp) {
        ID3D11ShaderResourceView* noise = m_noiseSrv.get();
        m_context->PSSetShaderResources(1, 1, &noise);
        ID3D11SamplerState* ns = m_noiseSamp.get();
        m_context->PSSetSamplers(1, 1, &ns);
    }
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    m_context->PSSetShaderResources(1, 1, &none);
}

void Renderer::setViewport(int x, int y, int w, int h) {
    if (!m_context) return;
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = (float)x;
    vp.TopLeftY = (float)y;
    vp.Width = (float)std::max(1, w);
    vp.Height = (float)std::max(1, h);
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

namespace {
float hash12px(float x, float y) {
    auto frac = [](float v) { return v - std::floor(v); };
    float p3x = frac(x * 0.1031f);
    float p3y = frac(y * 0.1031f);
    float p3z = frac(x * 0.1031f);
    float d = p3x * (p3y + 33.33f) + p3y * (p3z + 33.33f) + p3z * (p3x + 33.33f);
    p3x += d;
    p3y += d;
    p3z += d;
    return frac((p3x + p3y) * p3z);
}

float halfToFloat(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16;
    uint32_t e = (h & 0x7C00u) >> 10;
    uint32_t m = h & 0x03FFu;
    uint32_t out = s;
    if (e == 31) out |= 0x7F800000u | (m << 13);
    else if (e != 0) out |= ((e + 112) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &out, 4);
    return f;
}
}

bool Renderer::createNoise() {
    if (!m_device) return false;
    std::vector<uint8_t> pix(256 * 256);
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            float h = hash12px((float)x, (float)y);
            int q = (int)std::lround(h * 255.0f);
            if (q < 0) q = 0;
            if (q > 255) q = 255;
            pix[(size_t)y * 256 + (size_t)x] = (uint8_t)q;
        }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 256;
    td.Height = 256;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = pix.data();
    sd.SysMemPitch = 256;
    if (FAILED(m_device->CreateTexture2D(&td, &sd, m_noiseTex.put()))) return false;
    if (FAILED(m_device->CreateShaderResourceView(m_noiseTex.get(), nullptr, m_noiseSrv.put()))) return false;
    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&samp, m_noiseSamp.put());
    return true;
}

void Renderer::collectGpuTime(int slot) {
    GpuSlot& q = m_gq[slot];
    if (!q.pending || !m_context || !q.disjoint) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (m_context->GetData(q.disjoint.get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    UINT64 a = 0, b = 0;
    if (m_context->GetData(q.t0.get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    if (m_context->GetData(q.t1.get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    q.pending = false;
    if (!dj.Disjoint && dj.Frequency > 0 && b >= a) {
        m_lastGpuMs = (float)((double)(b - a) / (double)dj.Frequency * 1000.0);
    }
}

void Renderer::beginGpuTime() {
    if (!m_context || !m_device) return;
    collectGpuTime((m_gqSlot + 2) % 3);
    GpuSlot& q = m_gq[m_gqSlot];
    if (!q.disjoint) {
        D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(m_device->CreateQuery(&d, q.disjoint.put()))) return;
        d.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(m_device->CreateQuery(&d, q.t0.put())) || FAILED(m_device->CreateQuery(&d, q.t1.put()))) return;
    }
    if (!q.t0 || !q.t1 || !q.disjoint) return;
    m_context->Begin(q.disjoint.get());
    m_context->End(q.t0.get());
    q.pending = true;
}

void Renderer::endGpuTime() {
    if (!m_context) return;
    GpuSlot& q = m_gq[m_gqSlot];
    if (!q.pending || !q.t1 || !q.disjoint) return;
    m_context->End(q.t1.get());
    m_context->End(q.disjoint.get());
    m_gqSlot = (m_gqSlot + 1) % 3;
}

bool Renderer::ensureSceneReadback() {
    if (!m_context || !m_sceneTex || m_sceneW < 2 || m_sceneH < 2) return false;
    bool rebuild = !m_lumaStage;
    if (m_lumaStage) {
        D3D11_TEXTURE2D_DESC have{};
        m_lumaStage->GetDesc(&have);
        if ((int)have.Width != m_sceneW || (int)have.Height != m_sceneH) {
            m_lumaStage.reset();
            rebuild = true;
        }
    }
    if (rebuild) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)m_sceneW;
        td.Height = (UINT)m_sceneH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_lumaStage.put()))) return false;
    }
    m_context->CopyResource(m_lumaStage.get(), m_sceneTex.get());
    return true;
}

float Renderer::sampleSceneLuma(int* litPixels) {
    if (litPixels) *litPixels = 0;
    if (!ensureSceneReadback()) return 0.0f;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(m_context->Map(m_lumaStage.get(), 0, D3D11_MAP_READ, 0, &map))) return 0.0f;
    float best = 0.0f;
    int lit = 0;
    // A glyph or a star is a few bright pixels. An average of a small window misses them.
    constexpr float kLit = 0.02f;
    for (int y = 0; y < m_sceneH; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const char*>(map.pData) + (size_t)y * map.RowPitch);
        for (int x = 0; x < m_sceneW; ++x) {
            const float r = halfToFloat(row[x * 4 + 0]);
            const float g = halfToFloat(row[x * 4 + 1]);
            const float b = halfToFloat(row[x * 4 + 2]);
            const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            if (l > best) best = l;
            if (l >= kLit) ++lit;
        }
    }
    m_context->Unmap(m_lumaStage.get(), 0);
    if (litPixels) *litPixels = lit;
    return best;
}

float Renderer::sampleBandEdge() {
    if (!ensureSceneReadback()) return 1.0f;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(m_context->Map(m_lumaStage.get(), 0, D3D11_MAP_READ, 0, &map))) return 1.0f;
    const int bs = 24;
    if (m_sceneW < bs * 6 || m_sceneH < bs * 6) {
        m_context->Unmap(m_lumaStage.get(), 0);
        return 0.0f;
    }
    const int gw = m_sceneW / bs;
    const int gh = m_sceneH / bs;
    auto at = [&](int x, int y) {
        x = std::clamp(x, 0, m_sceneW - 1);
        y = std::clamp(y, 0, m_sceneH - 1);
        const auto* row =
            reinterpret_cast<const uint16_t*>(static_cast<const char*>(map.pData) + (size_t)y * map.RowPitch);
        const float r = halfToFloat(row[x * 4 + 0]);
        const float g = halfToFloat(row[x * 4 + 1]);
        const float b = halfToFloat(row[x * 4 + 2]);
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    };
    std::vector<float> med((size_t)gw * gh);
    for (int j = 0; j < gh; ++j) {
        for (int i = 0; i < gw; ++i) {
            float s[25];
            int n = 0;
            for (int a = 0; a < 5; ++a) {
                for (int b = 0; b < 5; ++b) {
                    const int x = i * bs + (b * 2 + 1) * bs / 10;
                    const int y = j * bs + (a * 2 + 1) * bs / 10;
                    s[n++] = at(x, y);
                }
            }
            std::sort(s, s + n);
            med[(size_t)j * gw + i] = s[n / 2];
        }
    }
    m_context->Unmap(m_lumaStage.get(), 0);
    std::vector<float> sorted = med;
    std::sort(sorted.begin(), sorted.end());
    const float peak = sorted[(size_t)((sorted.size() - 1) * 0.98f)];
    if (peak < 0.004f) return 0.0f;
    float best = 0.0f;
    for (int j = 1; j < gh - 1; ++j) {
        for (int i = 1; i < gw - 1; ++i) {
            const float a = med[(size_t)j * gw + i];
            if (a < 0.10f * peak || a > 0.40f * peak) continue;
            const int nb[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
            for (const auto& d : nb) {
                const float b = med[(size_t)(j + d[1]) * gw + (i + d[0])];
                if (b > a || b > 0.55f * peak) continue;
                best = std::max(best, (a - b) / peak);
            }
        }
    }
    return best;
}

bool Renderer::capturePng(const std::wstring& path) {
    if (!m_context || !m_swap || path.empty()) return false;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(m_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void())) || !back) return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    if (!m_captureStage || m_width != (int)desc.Width || m_height != (int)desc.Height) {
        m_captureStage.reset();
        D3D11_TEXTURE2D_DESC td = desc;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, m_captureStage.put()))) return false;
    }
    m_context->CopyResource(m_captureStage.get(), back.get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(m_context->Map(m_captureStage.get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    ComPtr<IWICImagingFactory> fac;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(fac.put()));
    bool ok = false;
    if (SUCCEEDED(hr) && fac) {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (SUCCEEDED(fac->CreateStream(stream.put())) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) &&
            SUCCEEDED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(frame.put(), nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
            frame->SetSize(desc.Width, desc.Height);
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            frame->SetPixelFormat(&fmt);
            ok = SUCCEEDED(frame->WritePixels(desc.Height, map.RowPitch, map.RowPitch * desc.Height,
                                              static_cast<BYTE*>(map.pData))) &&
                 SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
        }
    }
    m_context->Unmap(m_captureStage.get(), 0);
    return ok;
}

bool Renderer::createSwapchainBuffers() {
    m_rtv.reset();
    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = m_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void());
    if (FAILED(hr)) {
        LOG_ERR("GetBuffer failed: 0x%08X", (unsigned)hr);
        return false;
    }
    hr = m_device->CreateRenderTargetView(back.get(), nullptr, m_rtv.put());
    if (FAILED(hr)) LOG_ERR("CreateRenderTargetView failed: 0x%08X", (unsigned)hr);
    return SUCCEEDED(hr);
}

bool Renderer::resize(int width, int height) {
    if (!m_swap || !m_context) return false;
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == m_width && height == m_height) return true;

    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv.reset();
    HRESULT hr = m_swap->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LOG_ERR("ResizeBuffers failed: 0x%08X", (unsigned)hr);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            LOG_ERR("ResizeBuffers TDR GetDeviceRemovedReason=0x%08X", (unsigned)deviceRemovedReason());
        }
        m_deviceLost = true;
        return false;
    }
    m_width = width;
    m_height = height;
    if (m_composition) {
        if (m_dcompVisual) {
            auto* visual = static_cast<IDCompositionVisual*>(m_dcompVisual);
            visual->SetContent(m_swap1 ? (IUnknown*)m_swap1.get() : (IUnknown*)m_swap.get());
        }
        if (m_dcompDevice) static_cast<IDCompositionDevice*>(m_dcompDevice)->Commit();
    }
    LOG_INFO("Swapchain resized to %dx%d", width, height);
    return createSwapchainBuffers();
}

bool Renderer::ensureScene(int width, int height, bool offscreen) {
    width = std::max(1, width);
    height = std::max(1, height);
    int& sw = offscreen ? m_offSceneW : m_sceneW;
    int& sh = offscreen ? m_offSceneH : m_sceneH;
    auto& tex = offscreen ? m_offSceneTex : m_sceneTex;
    auto& rtv = offscreen ? m_offSceneRtv : m_sceneRtv;
    auto& srv = offscreen ? m_offSceneSrv : m_sceneSrv;
    if (tex && sw == width && sh == height) return true;
    tex.reset();
    rtv.reset();
    srv.reset();
    sw = 0;
    sh = 0;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width;
    td.Height = (UINT)height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    // Linear color does not survive an 8-bit UNORM target: the grade pass then
    // stretches those steps into visible bands. Float16 keeps the field smooth.
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, tex.put());
    if (FAILED(hr)) return false;
    hr = m_device->CreateRenderTargetView(tex.get(), nullptr, rtv.put());
    if (FAILED(hr)) return false;
    hr = m_device->CreateShaderResourceView(tex.get(), nullptr, srv.put());
    if (FAILED(hr)) return false;
    sw = width;
    sh = height;
    return true;
}

void Renderer::composite(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, int width, int height) {
    if (!m_context || !src || !dst || !m_gradePs || !m_fsVs) return;
    m_context->OMSetRenderTargets(1, &dst, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width = (float)std::max(1, width);
    vp.Height = (float)std::max(1, height);
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    struct GradeCB {
        float brightness, saturation, vignette, night, w, h, pad0, pad1;
    } cb{m_grade.brightness, m_grade.saturation, m_grade.vignette, m_grade.night, vp.Width, vp.Height, 0, 0};
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(m_context->Map(m_fsCb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        std::memcpy(map.pData, &cb, sizeof(cb));
        if (sizeof(cb) < 512) std::memset(static_cast<char*>(map.pData) + sizeof(cb), 0, 512 - sizeof(cb));
        m_context->Unmap(m_fsCb.get(), 0);
    }
    ID3D11Buffer* noneVb = nullptr;
    UINT stride = 0, offset = 0;
    m_context->IASetVertexBuffers(0, 1, &noneVb, &stride, &offset);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_fsVs.get(), nullptr, 0);
    m_context->PSSetShader(m_gradePs.get(), nullptr, 0);
    ID3D11Buffer* cbuf = m_fsCb.get();
    m_context->VSSetConstantBuffers(0, 1, &cbuf);
    m_context->PSSetConstantBuffers(0, 1, &cbuf);
    m_context->PSSetShaderResources(0, 1, &src);
    ID3D11SamplerState* samp = m_sampler.get();
    m_context->PSSetSamplers(0, 1, &samp);
    m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    m_context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    m_context->PSSetShaderResources(0, 1, &none);
}

void Renderer::compositeToBackbuffer() {
    if (!m_drawScene || !m_sceneSrv || !m_rtv) return;
    composite(m_sceneSrv.get(), m_rtv.get(), m_width, m_height);
    m_drawScene = false;
    ID3D11RenderTargetView* rtv = m_rtv.get();
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
}

void Renderer::releaseScenes() {
    m_sceneSrv.reset();
    m_sceneRtv.reset();
    m_sceneTex.reset();
    m_offSceneSrv.reset();
    m_offSceneRtv.reset();
    m_offSceneTex.reset();
    m_sceneW = m_sceneH = m_offSceneW = m_offSceneH = 0;
    m_drawScene = false;
}

bool Renderer::beginFrame() {
    if (!m_context || !m_rtv) return false;
    m_offRtv = nullptr;
    m_drawScene = ensureScene(m_width, m_height, false);
    ID3D11RenderTargetView* rtv = m_drawScene ? m_sceneRtv.get() : m_rtv.get();
    m_context->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    m_context->RSSetState(m_raster.get());
    m_context->OMSetDepthStencilState(m_depth.get(), 0);
    const float blendFactor[4] = {0, 0, 0, 0};
    m_context->OMSetBlendState(m_blend.get(), blendFactor, 0xFFFFFFFF);
    return true;
}

void Renderer::clear(float r, float g, float b, float a) {
    ID3D11RenderTargetView* rtv = m_offRtv ? m_offSceneRtv.get() : (m_drawScene ? m_sceneRtv.get() : m_rtv.get());
    if (!m_context || !rtv) return;
    const float c[4] = {r, g, b, a};
    m_context->ClearRenderTargetView(rtv, c);
}

void Renderer::beginOffscreen(ID3D11RenderTargetView* rtv, int width, int height) {
    m_offRtv = rtv;
    m_offW = std::max(1, width);
    m_offH = std::max(1, height);
    if (!m_context || !rtv) return;
    m_offUsesScene = ensureScene(m_offW, m_offH, true);
    ID3D11RenderTargetView* draw = m_offUsesScene ? m_offSceneRtv.get() : rtv;
    m_context->OMSetRenderTargets(1, &draw, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width = (float)m_offW;
    vp.Height = (float)m_offH;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    m_context->RSSetState(m_raster.get());
    m_context->OMSetDepthStencilState(m_depth.get(), 0);
    const float blendFactor[4] = {0, 0, 0, 0};
    m_context->OMSetBlendState(m_blend.get(), blendFactor, 0xFFFFFFFF);
}

void Renderer::endOffscreen() {
    if (m_offUsesScene && m_context && m_offRtv && m_offSceneSrv) composite(m_offSceneSrv.get(), m_offRtv, m_offW, m_offH);
    m_offUsesScene = false;
    m_offRtv = nullptr;
    if (!m_context || !m_rtv) return;
    ID3D11RenderTargetView* rtv = m_drawScene ? m_sceneRtv.get() : m_rtv.get();
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width = (float)std::max(1, m_width);
    vp.Height = (float)std::max(1, m_height);
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

bool Renderer::ensureInstanceCapacity(int count) {
    if (count <= m_instanceCapacity && m_instanceBuf) return true;
    int cap = m_instanceCapacity ? m_instanceCapacity : 1024;
    while (cap < count) cap *= 2;
    m_instanceBuf.reset();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)(cap * sizeof(GlyphInstance));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = m_device->CreateBuffer(&bd, nullptr, m_instanceBuf.put());
    if (FAILED(hr)) {
        LOG_ERR("Create instance buffer failed: 0x%08X", (unsigned)hr);
        return false;
    }
    m_instanceCapacity = cap;
    return true;
}

void Renderer::drawGlyphs(ID3D11ShaderResourceView* atlas, const GlyphInstance* inst, int count) {
    if (!m_context || count <= 0 || !atlas) return;
    if (!ensureInstanceCapacity(count)) return;

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(m_context->Map(m_instanceBuf.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    std::memcpy(map.pData, inst, (size_t)count * sizeof(GlyphInstance));
    m_context->Unmap(m_instanceBuf.get(), 0);

    D3D11_MAPPED_SUBRESOURCE cmap{};
    if (SUCCEEDED(m_context->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cmap))) {
        float* cb = static_cast<float*>(cmap.pData);
        cb[0] = (float)(m_offRtv ? m_offW : m_width);
        cb[1] = (float)(m_offRtv ? m_offH : m_height);
        cb[2] = 0;
        cb[3] = 0;
        m_context->Unmap(m_cb.get(), 0);
    }

    UINT stride = sizeof(GlyphInstance);
    UINT offset = 0;
    ID3D11Buffer* vb = m_instanceBuf.get();
    m_context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    m_context->IASetInputLayout(m_layout.get());
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->VSSetShader(m_vs.get(), nullptr, 0);
    m_context->PSSetShader(m_ps.get(), nullptr, 0);
    ID3D11Buffer* cb = m_cb.get();
    m_context->VSSetConstantBuffers(0, 1, &cb);
    m_context->PSSetShaderResources(0, 1, &atlas);
    ID3D11SamplerState* samp = m_sampler.get();
    m_context->PSSetSamplers(0, 1, &samp);
    m_context->DrawInstanced(4, (UINT)count, 0, 0);

    ID3D11ShaderResourceView* none = nullptr;
    m_context->PSSetShaderResources(0, 1, &none);
}

HRESULT Renderer::deviceRemovedReason() const {
    if (!m_device) return DXGI_ERROR_DEVICE_REMOVED;
    return m_device->GetDeviceRemovedReason();
}

void Renderer::endFrame() {
    if (!m_swap) return;
    const HRESULT hr = m_swap->Present(0, 0);
    const bool hrChanged = hr != m_lastPresentHr;
    m_lastPresentHr = hr;
    m_occluded = (hr == DXGI_STATUS_OCCLUDED);
    if (SUCCEEDED(hr)) m_presentCount++;
    if (m_presentCount <= 5 || FAILED(hr) || m_occluded || hrChanged) {
        LOG_INFO("Present #%d hr=0x%08X occluded=%d path=%s", m_presentCount, (unsigned)hr,
                 m_occluded ? 1 : 0, m_composition ? "dcomp" : "hwnd");
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        const HRESULT reason = deviceRemovedReason();
        LOG_ERR("D3D device lost Present=0x%08X GetDeviceRemovedReason=0x%08X (TDR/reset)", (unsigned)hr,
                (unsigned)reason);
        m_deviceLost = true;
    }
}

void Renderer::release() {
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    releaseDcomp();
    m_instanceBuf.reset();
    m_cb.reset();
    m_depth.reset();
    m_raster.reset();
    m_blend.reset();
    m_sampler.reset();
    m_layout.reset();
    m_ps.reset();
    m_vs.reset();
    m_fsVs.reset();
    m_fsCb.reset();
    m_gradePs.reset();
    m_fallbackPs.reset();
    m_noiseSrv.reset();
    m_noiseSamp.reset();
    m_noiseTex.reset();
    m_lumaStage.reset();
    m_captureStage.reset();
    for (auto& q : m_gq) {
        q.disjoint.reset();
        q.t0.reset();
        q.t1.reset();
        q.pending = false;
    }
    m_shaderCache.clear();
    releaseScenes();
    m_rtv.reset();
    m_swap1.reset();
    m_swap.reset();
    m_context.reset();
    m_device.reset();
    m_instanceCapacity = 0;
    m_hwnd = nullptr;
}
