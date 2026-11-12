#include "effects/ImageMask.h"

#include "core/ComPtr.h"
#include "core/Log.h"

#include <algorithm>
#include <wincodec.h>

bool loadImageMask(const std::wstring& path, std::vector<uint8_t>& luma, std::vector<uint8_t>& rgb, int& w, int& h) {
    luma.clear();
    rgb.clear();
    w = h = 0;
    if (path.empty()) return false;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
    if (FAILED(hr) || !factory) {
        LOG_WARN("WIC factory failed: 0x%08X", (unsigned)hr);
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                            decoder.put());
    if (FAILED(hr) || !decoder) {
        LOG_WARN("Could not open image");
        return false;
    }
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
    int tw = (int)sw;
    int th = (int)sh;
    const int cap = 256;
    if (tw > cap || th > cap) {
        const float s = (float)cap / (float)std::max(tw, th);
        tw = std::max(2, (int)(tw * s));
        th = std::max(2, (int)(th * s));
    }

    ComPtr<IWICBitmapScaler> scaler;
    IWICBitmapSource* src = conv.get();
    if (tw != (int)sw || th != (int)sh) {
        if (FAILED(factory->CreateBitmapScaler(scaler.put())) || !scaler) return false;
        hr = scaler->Initialize(conv.get(), (UINT)tw, (UINT)th, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return false;
        src = scaler.get();
    }

    std::vector<uint8_t> bgra((size_t)tw * (size_t)th * 4);
    hr = src->CopyPixels(nullptr, (UINT)tw * 4, (UINT)bgra.size(), bgra.data());
    if (FAILED(hr)) return false;

    luma.resize((size_t)tw * (size_t)th);
    rgb.resize((size_t)tw * (size_t)th * 3);
    for (int i = 0; i < tw * th; ++i) {
        const uint8_t b = bgra[(size_t)i * 4 + 0];
        const uint8_t g = bgra[(size_t)i * 4 + 1];
        const uint8_t r = bgra[(size_t)i * 4 + 2];
        const uint8_t a = bgra[(size_t)i * 4 + 3];
        const float af = a / 255.0f;
        const float rf = r / 255.0f * af;
        const float gf = g / 255.0f * af;
        const float bf = b / 255.0f * af;
        const float y = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;
        luma[(size_t)i] = (uint8_t)std::clamp((int)(y * 255.0f), 0, 255);
        rgb[(size_t)i * 3 + 0] = (uint8_t)std::clamp((int)(rf * 255.0f), 0, 255);
        rgb[(size_t)i * 3 + 1] = (uint8_t)std::clamp((int)(gf * 255.0f), 0, 255);
        rgb[(size_t)i * 3 + 2] = (uint8_t)std::clamp((int)(bf * 255.0f), 0, 255);
    }
    w = tw;
    h = th;
    LOG_INFO("Custom reveal image %dx%d", w, h);
    return true;
}
