#include "gfx/Plates.h"

#include "core/Log.h"
#include "core/Util.h"

#include <algorithm>
#include <string>
#include <vector>
#include <wincodec.h>

namespace {

std::wstring envPlatesDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"STALGIA_PLATES_DIR", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return buf;
}

bool loadBgra(IWICImagingFactory* factory, const std::wstring& path, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    bgra.clear();
    w = h = 0;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                    decoder.put());
    if (FAILED(hr) || !decoder) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put())) || !frame) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(factory->CreateFormatConverter(conv.put())) || !conv) return false;
    hr = conv->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                          WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    UINT sw = 0, sh = 0;
    conv->GetSize(&sw, &sh);
    if (sw < 2 || sh < 2) return false;
    UINT tw = sw;
    UINT th = sh;
    const UINT cap = 3840;
    if (tw > cap || th > cap) {
        const float s = (float)cap / (float)std::max(tw, th);
        tw = std::max(2u, (UINT)(tw * s));
        th = std::max(2u, (UINT)(th * s));
    }

    ComPtr<IWICBitmapScaler> scaler;
    IWICBitmapSource* src = conv.get();
    if (tw != sw || th != sh) {
        if (FAILED(factory->CreateBitmapScaler(scaler.put())) || !scaler) return false;
        hr = scaler->Initialize(conv.get(), tw, th, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return false;
        src = scaler.get();
    }

    bgra.resize((size_t)tw * (size_t)th * 4);
    hr = src->CopyPixels(nullptr, tw * 4, (UINT)bgra.size(), bgra.data());
    if (FAILED(hr)) {
        bgra.clear();
        return false;
    }
    w = tw;
    h = th;
    return true;
}

bool upload(ID3D11Device* device, const std::vector<uint8_t>& bgra, UINT w, UINT h, ID3D11ShaderResourceView** out) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = bgra.data();
    sd.SysMemPitch = w * 4;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&td, &sd, tex.put())) || !tex) return false;
    return SUCCEEDED(device->CreateShaderResourceView(tex.get(), nullptr, out));
}

std::wstring findLayer(const std::wstring& root, const wchar_t* effect, const wchar_t* preset, const wchar_t* layer) {
    const wchar_t* folders[2] = {preset, L"sky"};
    const wchar_t* exts[] = {L".png", L".jpg", L".jpeg"};
    for (const wchar_t* folder : folders) {
        if (!folder || !folder[0]) continue;
        for (const wchar_t* ext : exts) {
            const std::wstring path = root + L"\\" + effect + L"\\" + folder + L"\\" + layer + ext;
            if (fileExists(path)) return path;
        }
        if (preset && wcscmp(folder, preset) == 0 && wcscmp(preset, L"sky") == 0) break;
    }
    return {};
}

}  // namespace

bool loadPlateLayers(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset, PlateLayers& out) {
    out = {};
    if (!device || !effect || !effect[0] || !preset || !preset[0]) return false;

    std::wstring roots[3] = {envPlatesDir(), exeDirectory() + L"\\plates", appDataDirectory() + L"\\plates"};
    ComPtr<IWICImagingFactory> factory;
    const wchar_t* layers[3] = {L"far", L"mid", L"near"};
    for (int i = 0; i < 3; ++i) {
        std::wstring path;
        for (const std::wstring& root : roots) {
            if (root.empty()) continue;
            path = findLayer(root, effect, preset, layers[i]);
            if (!path.empty()) break;
        }
        if (path.empty()) continue;
        if (!factory) {
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
            if (FAILED(hr) || !factory) {
                LOG_WARN("WIC factory failed: 0x%08X", (unsigned)hr);
                return false;
            }
        }
        std::vector<uint8_t> bgra;
        UINT w = 0, h = 0;
        if (!loadBgra(factory.get(), path, bgra, w, h)) {
            LOG_WARN("Could not decode plate");
            continue;
        }
        ID3D11ShaderResourceView* srv = nullptr;
        if (!upload(device, bgra, w, h, &srv) || !srv) continue;
        out.layer[i] = ComPtr<ID3D11ShaderResourceView>(srv);
        out.mask += (i == 0) ? 1.0f : (i == 1 ? 2.0f : 4.0f);
        LOG_INFO("Plate %ls/%ls %ls %ux%u", effect, preset, layers[i], w, h);
    }
    return out.mask > 0.0f;
}
