#include "gfx/Plates.h"

#include "core/Log.h"
#include "core/Util.h"
#include "resource.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <objbase.h>
#include <wincodec.h>

namespace {

struct BuiltIn {
    const wchar_t* effect;
    const wchar_t* preset;
    int ids[4];
};

const BuiltIn kBuiltIn[] = {
    {L"fireflies", L"summer-dusk", {IDR_PLATE_FIREFLIES_A, IDR_PLATE_FIREFLIES_B, IDR_PLATE_FIREFLIES_C, IDR_PLATE_FIREFLIES_D}},
    {L"snow", L"pine-forest", {IDR_PLATE_SNOW_A, IDR_PLATE_SNOW_B, IDR_PLATE_SNOW_C, IDR_PLATE_SNOW_D}},
    {L"stars", L"still-night", {IDR_PLATE_STARS_A, IDR_PLATE_STARS_B, IDR_PLATE_STARS_C, IDR_PLATE_STARS_D}},
    {L"stars", L"milky-way", {IDR_PLATE_STARS_A, IDR_PLATE_STARS_B, IDR_PLATE_STARS_C, IDR_PLATE_STARS_D}},
    {L"aurora", L"classic", {IDR_PLATE_AURORA_A, IDR_PLATE_AURORA_B, IDR_PLATE_AURORA_C, IDR_PLATE_AURORA_D}},
};

const wchar_t* kLayerName[4] = {L"layer-a-sky", L"layer-b-mountains", L"layer-c-mid", L"layer-d-foreground"};
const wchar_t* kLegacyName[4] = {L"far", L"mid", L"near", nullptr};

std::wstring envPlatesDir() {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"STALGIA_PLATES_DIR", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return buf;
}

const BuiltIn* builtinFor(const wchar_t* effect, const wchar_t* preset) {
    for (const BuiltIn& row : kBuiltIn) {
        if (wcscmp(effect, row.effect) == 0 && wcscmp(preset, row.preset) == 0) return &row;
    }
    return nullptr;
}

bool decodeBgra(IWICImagingFactory* factory, IStream* stream, std::vector<uint8_t>& bgra, UINT& w, UINT& h) {
    bgra.clear();
    w = h = 0;
    if (!factory || !stream) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, decoder.put());
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

bool streamFromMemory(const void* data, DWORD size, IStream** out) {
    if (!data || !size || !out) return false;
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!copy) return false;
    void* locked = GlobalLock(copy);
    if (!locked) {
        GlobalFree(copy);
        return false;
    }
    std::memcpy(locked, data, size);
    GlobalUnlock(copy);
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, out))) {
        GlobalFree(copy);
        return false;
    }
    return true;
}

bool loadStreamFile(const std::wstring& path, IStream** out) {
    if (!out || path.empty()) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(file, nullptr);
    if (size == 0 || size == INVALID_FILE_SIZE) {
        CloseHandle(file);
        return false;
    }
    std::vector<uint8_t> bytes(size);
    DWORD got = 0;
    const BOOL ok = ReadFile(file, bytes.data(), size, &got, nullptr);
    CloseHandle(file);
    if (!ok || got != size) return false;
    return streamFromMemory(bytes.data(), size, out);
}

// WebP often stores nearly-opaque edges as 250-254. Snap those to solid
// so the cutout does not fringe against the layer behind it.
// Aurora peaks and ice are strongly teal. Pull that toward an equal-channel
// gray, more on the bright ice than on the dark water. Luma stays green on a
// teal pixel, so the gray is the channel average, not the luma.
void finishPlate(std::vector<uint8_t>& bgra, bool aurora) {
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
        uint8_t& b = bgra[i + 0];
        uint8_t& g = bgra[i + 1];
        uint8_t& r = bgra[i + 2];
        uint8_t& a = bgra[i + 3];
        if (a >= 250) a = 255;
        if (!aurora || a == 0) continue;
        const float rf = r;
        const float gf = g;
        const float bf = b;
        const float mx = std::max(rf, std::max(gf, bf));
        const float mn = std::min(rf, std::min(gf, bf));
        if (mx < 8.0f) continue;
        const float sat = (mx - mn) / mx;
        if (!(gf > rf + 6.0f && bf > rf + 6.0f && sat > 0.08f)) continue;
        const float luma = 0.2126f * rf + 0.7152f * gf + 0.0722f * bf;
        const float neutral = (rf + gf + bf) / 3.0f;
        float t = std::clamp((luma - 40.0f) / 140.0f, 0.0f, 1.0f);
        t = t * t * (3.0f - 2.0f * t);
        const float k = 0.38f + 0.45f * t;
        r = (uint8_t)std::clamp(rf + (neutral - rf) * k + 0.5f, 0.0f, 255.0f);
        g = (uint8_t)std::clamp(gf + (neutral - gf) * k + 0.5f, 0.0f, 255.0f);
        b = (uint8_t)std::clamp(bf + (neutral - bf) * k + 0.5f, 0.0f, 255.0f);
    }
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

std::wstring findFile(const std::wstring& root, const wchar_t* effect, const wchar_t* preset, const wchar_t* layer) {
    if (!layer || !layer[0]) return {};
    const wchar_t* folders[2] = {preset, L"sky"};
    const wchar_t* exts[] = {L".webp", L".png", L".jpg", L".jpeg"};
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

std::wstring findExternal(const wchar_t* effect, const wchar_t* preset, int layer) {
    std::wstring roots[3] = {envPlatesDir(), exeDirectory() + L"\\plates", appDataDirectory() + L"\\plates"};
    const wchar_t* names[2] = {kLayerName[layer], kLegacyName[layer]};
    for (const std::wstring& root : roots) {
        if (root.empty()) continue;
        for (const wchar_t* name : names) {
            if (!name) continue;
            const std::wstring path = findFile(root, effect, preset, name);
            if (!path.empty()) return path;
        }
    }
    return {};
}

struct CpuLayer {
    std::vector<uint8_t> bgra;
    UINT w = 0;
    UINT h = 0;
};

std::wstring cacheKey(const wchar_t* effect, const wchar_t* preset) {
    if (!effect || !preset || !effect[0] || !preset[0]) return {};
    const bool external = !findExternal(effect, preset, 0).empty();
    const BuiltIn* built = builtinFor(effect, preset);
    if (!external && !built) return {};
    // Still night and Milky Way are the same embedded photographs.
    if (!external && wcscmp(effect, L"stars") == 0 &&
        (wcscmp(preset, L"still-night") == 0 || wcscmp(preset, L"milky-way") == 0)) {
        return L"stars/builtin";
    }
    return std::wstring(effect) + L"/" + preset;
}

bool loadResourceLayer(IWICImagingFactory* factory, int id, IStream** out);

bool decodePlateCpu(const wchar_t* effect, const wchar_t* preset, CpuLayer layer[4], float& mask) {
    mask = 0.0f;
    for (int i = 0; i < 4; ++i) layer[i] = {};
    if (!effect || !preset) return false;
    const bool aurora = wcscmp(effect, L"aurora") == 0;
    const BuiltIn* built = builtinFor(effect, preset);
    const bool externalSky = !findExternal(effect, preset, 0).empty();
    if (!externalSky && !built) return false;

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
    if (FAILED(hr) || !factory) {
        LOG_WARN("WIC factory failed: 0x%08X", (unsigned)hr);
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        ComPtr<IStream> stream;
        bool opened = false;
        if (externalSky) {
            const std::wstring path = findExternal(effect, preset, i);
            opened = !path.empty() && loadStreamFile(path, stream.put());
        } else if (built) {
            opened = loadResourceLayer(factory.get(), built->ids[i], stream.put());
        }
        if (!opened || !stream) continue;
        UINT w = 0, h = 0;
        if (!decodeBgra(factory.get(), stream.get(), layer[i].bgra, w, h)) {
            layer[i] = {};
            continue;
        }
        finishPlate(layer[i].bgra, aurora);
        layer[i].w = w;
        layer[i].h = h;
        mask += (i == 0) ? 1.0f : (i == 1 ? 2.0f : (i == 2 ? 4.0f : 8.0f));
        LOG_INFO("Plate decoded %ls/%ls %ls %ux%u", effect, preset, kLayerName[i], w, h);
    }
    return mask > 0.0f;
}

struct PlateSet {
    std::wstring key;
    std::wstring effect;
    std::wstring preset;
    CpuLayer cpu[4];
    float mask = 0.0f;
    bool decoded = false;
    ComPtr<ID3D11ShaderResourceView> srv[4];
    ID3D11Device* uploadedDevice = nullptr;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::thread g_worker;
bool g_started = false;
bool g_stop = false;
std::vector<PlateSet> g_sets;

void queueBuiltinsLocked() {
    const wchar_t* jobs[][2] = {
        {L"stars", L"still-night"},
        {L"stars", L"milky-way"},
        {L"fireflies", L"summer-dusk"},
        {L"snow", L"pine-forest"},
        {L"aurora", L"classic"},
    };
    for (const auto& job : jobs) {
        const std::wstring key = cacheKey(job[0], job[1]);
        if (key.empty()) continue;
        bool seen = false;
        for (const auto& set : g_sets)
            if (set.key == key) seen = true;
        if (seen) continue;
        PlateSet set;
        set.key = key;
        set.effect = job[0];
        set.preset = job[1];
        g_sets.push_back(std::move(set));
    }
}

void workerMain() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto t0 = std::chrono::steady_clock::now();
    bool logged = false;
    for (;;) {
        std::wstring effect, preset;
        size_t index = 0;
        {
            std::unique_lock<std::mutex> lock(g_mu);
            for (;;) {
                if (g_stop) break;
                index = g_sets.size();
                for (size_t i = 0; i < g_sets.size(); ++i) {
                    if (!g_sets[i].decoded) {
                        index = i;
                        effect = g_sets[i].effect;
                        preset = g_sets[i].preset;
                        break;
                    }
                }
                if (index != g_sets.size()) break;
                if (!logged && !g_sets.empty()) {
                    const auto ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    LOG_INFO("Plate preload finished in %.0f ms", ms);
                    logged = true;
                }
                g_cv.wait(lock);
            }
            if (g_stop) break;
        }
        CpuLayer cpu[4];
        float mask = 0.0f;
        decodePlateCpu(effect.c_str(), preset.c_str(), cpu, mask);
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_stop) break;
            if (index >= g_sets.size()) continue;
            if (!g_sets[index].decoded) {
                for (int i = 0; i < 4; ++i) g_sets[index].cpu[i] = std::move(cpu[i]);
                g_sets[index].mask = mask;
                g_sets[index].decoded = true;
            }
        }
        g_cv.notify_all();
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

void ensureWorkerLocked() {
    if (g_started) return;
    queueBuiltinsLocked();
    g_stop = false;
    g_started = true;
    g_worker = std::thread(workerMain);
}

void preloadImpl() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        ensureWorkerLocked();
    }
    g_cv.notify_all();
}

void releaseImpl() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    std::lock_guard<std::mutex> lock(g_mu);
    g_sets.clear();
    g_started = false;
    g_stop = false;
}

size_t findSetLocked(const std::wstring& key) {
    for (size_t i = 0; i < g_sets.size(); ++i)
        if (g_sets[i].key == key) return i;
    return g_sets.size();
}

void enqueueLocked(const std::wstring& key, const wchar_t* effect, const wchar_t* preset) {
    if (findSetLocked(key) != g_sets.size()) return;
    PlateSet set;
    set.key = key;
    set.effect = effect;
    set.preset = preset;
    g_sets.push_back(std::move(set));
}

// One 4K layer per call. A preset change must not stall on the whole set.
void serviceImpl(ID3D11Device* device) {
    if (!device) return;
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& set : g_sets) {
        if (!set.decoded || set.uploadedDevice == device) continue;
        if (set.mask <= 0.0f) {
            set.uploadedDevice = device;
            return;
        }
        for (int i = 0; i < 4; ++i) {
            if (set.srv[i] || set.cpu[i].bgra.empty()) continue;
            ID3D11ShaderResourceView* srv = nullptr;
            if (!upload(device, set.cpu[i].bgra, set.cpu[i].w, set.cpu[i].h, &srv) || !srv) return;
            set.srv[i] = ComPtr<ID3D11ShaderResourceView>(srv);
            set.cpu[i].bgra.clear();
            set.cpu[i].bgra.shrink_to_fit();
            bool more = false;
            for (int j = 0; j < 4; ++j)
                if (!set.srv[j] && !set.cpu[j].bgra.empty()) more = true;
            if (!more) {
                set.uploadedDevice = device;
                LOG_INFO("Plate uploaded %ls", set.key.c_str());
            }
            return;
        }
        set.uploadedDevice = device;
        return;
    }
}

bool acquireImpl(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset, PlateLayers& out) {
    out.mask = 0.0f;
    out.pending = false;
    for (auto& layer : out.layer) layer.reset();
    if (!device || !effect || !preset) return false;
    const std::wstring key = cacheKey(effect, preset);
    if (key.empty()) return false;

    bool wake = false;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_started) {
            ensureWorkerLocked();
            wake = true;
        }
        size_t index = findSetLocked(key);
        if (index == g_sets.size()) {
            // Never decode here. The worker picks the set up, and this frame
            // draws without the photo.
            enqueueLocked(key, effect, preset);
            out.pending = true;
            wake = true;
        } else {
            PlateSet& set = g_sets[index];
            if (set.decoded && set.mask <= 0.0f) {
                // This preset has no plates.
            } else if (!set.decoded || set.uploadedDevice != device) {
                out.pending = true;
                wake = !set.decoded;
            } else {
                for (int i = 0; i < 4; ++i) {
                    if (!set.srv[i]) continue;
                    set.srv[i]->AddRef();
                    out.layer[i] = ComPtr<ID3D11ShaderResourceView>(set.srv[i].get());
                }
                out.mask = set.mask;
                ok = out.mask > 0.0f;
            }
        }
    }
    if (wake) g_cv.notify_all();
    return ok;
}

bool plateSetReadyImpl(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset) {
    if (!device || !effect || !preset) return true;
    const std::wstring key = cacheKey(effect, preset);
    if (key.empty()) return true;
    bool wake = false;
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_started) {
            ensureWorkerLocked();
            wake = true;
        }
        const size_t index = findSetLocked(key);
        if (index == g_sets.size()) {
            enqueueLocked(key, effect, preset);
            wake = true;
        } else if (g_sets[index].decoded && g_sets[index].mask <= 0.0f) {
            ready = true;
        } else {
            ready = g_sets[index].decoded && g_sets[index].uploadedDevice == device;
            wake = !g_sets[index].decoded;
        }
    }
    if (wake) g_cv.notify_all();
    return ready;
}

bool plateUploadsIdleImpl(ID3D11Device* device) {
    if (!device) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_started) return false;
    for (const auto& set : g_sets) {
        if (!set.decoded) return false;
        if (set.mask > 0.0f && set.uploadedDevice != device) return false;
    }
    return true;
}

bool loadResourceLayer(IWICImagingFactory* factory, int id, IStream** out) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL hg = LoadResource(mod, res);
    const void* data = hg ? LockResource(hg) : nullptr;
    const DWORD size = SizeofResource(mod, res);
    if (!data || !size) return false;
    return streamFromMemory(data, size, out);
}

}  // namespace

void startPlatePreload() { preloadImpl(); }

void releasePlateCache() { releaseImpl(); }

void servicePlateUploads(ID3D11Device* device) { serviceImpl(device); }

bool acquirePlateLayers(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset, PlateLayers& out) {
    return acquireImpl(device, effect, preset, out);
}

bool plateSetReady(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset) {
    return plateSetReadyImpl(device, effect, preset);
}

bool plateUploadsIdle(ID3D11Device* device) { return plateUploadsIdleImpl(device); }
