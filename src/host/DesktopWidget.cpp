#include "host/DesktopWidget.h"
#include "host/UiMetrics.h"

#include "core/ComPtr.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Util.h"
#include "effect/EffectRegistry.h"
#include "effect/Presets.h"
#include "host/ThumbnailStage.h"
#include "resource.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>

#include <initguid.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <d2derr.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <objbase.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3
#endif

#ifdef DrawText
#undef DrawText
#endif
#ifdef DrawTextW
#undef DrawTextW
#endif

using D2D1::ColorF;
using D2D1::Point2F;
using D2D1::RectF;
using D2D1::RoundedRect;
using D2D1::SizeF;

namespace {

constexpr float kShadow = 26.0f;
constexpr float kCardW = 360.0f;
constexpr float kMiniW = 236.0f;
constexpr float kMiniH = 44.0f;
constexpr float kSysH = 148.0f;
constexpr float kDrawerMax = 292.0f;
constexpr float kRadius = 20.0f;
constexpr float kInset = ui::kPad;
constexpr UINT kTimerAnim = 1;
constexpr UINT kTimerZ = 2;

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

float lerp(float a, float b, float t) { return a + (b - a) * t; }

float approach(float cur, float target, float dt, float speed) {
    const float d = target - cur;
    const float step = speed * dt;
    if (std::fabs(d) <= step) return target;
    return cur + (d > 0 ? step : -step);
}

D2D1_COLOR_F rgba(uint32_t rgb, float a) {
    return ColorF(((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, a);
}

D2D1_COLOR_F rgba4(float r, float g, float b, float a) { return ColorF(r, g, b, a); }

UINT windowDpi(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    }
    if (fn && hwnd) {
        const UINT d = fn(hwnd);
        if (d) return d;
    }
    HDC dc = GetDC(hwnd ? hwnd : nullptr);
    const UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return d ? d : 96;
}

struct Solid {
    ComPtr<ID2D1SolidColorBrush> b;
    ID2D1SolidColorBrush* get(ID2D1RenderTarget* rt, const D2D1_COLOR_F& c) {
        if (b) b->SetColor(c);
        else rt->CreateSolidColorBrush(c, b.put());
        return b.get();
    }
};

void drawText(ID2D1RenderTarget* rt, IDWriteTextFormat* tf, const D2D1_RECT_F& rc, const wchar_t* text,
              ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
    if (!tf || !text) return;
    tf->SetTextAlignment(align);
    tf->SetParagraphAlignment(valign);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    rt->DrawTextW(text, (UINT32)wcslen(text), tf, rc, brush, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
}

void fillRound(ID2D1RenderTarget* rt, ID2D1Brush* br, float x, float y, float w, float h, float r) {
    rt->FillRoundedRectangle(RoundedRect(RectF(x, y, x + w, y + h), r, r), br);
}

void drawRound(ID2D1RenderTarget* rt, ID2D1Brush* br, float x, float y, float w, float h, float r,
               float stroke) {
    rt->DrawRoundedRectangle(RoundedRect(RectF(x, y, x + w, y + h), r, r), br, stroke);
}

void drawArc(ID2D1RenderTarget* rt, ID2D1Brush* br, float cx, float cy, float radius, float t01, float stroke) {
    t01 = clampf(t01, 0.0f, 1.0f);
    if (t01 < 0.004f) return;
    const float start = -3.14159265f * 0.5f;
    const float sweep = t01 * 3.14159265f * 2.0f;
    const int n = std::max(8, (int)(sweep / 0.10f));
    D2D1_POINT_2F prev = Point2F(cx + std::cos(start) * radius, cy + std::sin(start) * radius);
    for (int i = 1; i <= n; ++i) {
        const float a = start + sweep * (float)i / (float)n;
        D2D1_POINT_2F p = Point2F(cx + std::cos(a) * radius, cy + std::sin(a) * radius);
        rt->DrawLine(prev, p, br, stroke);
        prev = p;
    }
}

void fmtRate(wchar_t* buf, size_t n, float bps) {
    if (bps >= 1024.0f * 1024.0f)
        swprintf(buf, n, L"%.1f MB/s", bps / (1024.0f * 1024.0f));
    else if (bps >= 10.0f * 1024.0f)
        swprintf(buf, n, L"%.0f KB/s", bps / 1024.0f);
    else
        swprintf(buf, n, L"%.1f KB/s", bps / 1024.0f);
}

void fmtUptime(wchar_t* buf, size_t n, ULONGLONG ms) {
    const unsigned h = (unsigned)(ms / 3600000ull);
    const unsigned m = (unsigned)((ms / 60000ull) % 60ull);
    if (h >= 24)
        swprintf(buf, n, L"%ud %uh", h / 24, h % 24);
    else
        swprintf(buf, n, L"%uh %um", h, m);
}

void drawPreview(ID2D1RenderTarget* rt, Solid& solid, const AppSettings& s, float x, float y, float w,
                 float h, float time) {
    fillRound(rt, solid.get(rt, rgba4(0.02f, 0.03f, 0.05f, 0.95f)), x, y, w, h, 12.0f);
    rt->PushAxisAlignedClip(RectF(x + 1, y + 1, x + w - 1, y + h - 1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const std::string& fx = s.effect;
    if (fx == "starfield") {
        for (int i = 0; i < 40; ++i) {
            const float px = x + std::fmod(i * 47.3f + time * 18.0f, w);
            const float py = y + std::fmod(i * 31.9f, h);
            const float a = 0.25f + (i % 5) * 0.12f;
            rt->FillEllipse(D2D1::Ellipse(Point2F(px, py), 1.1f, 1.1f), solid.get(rt, rgba(s.starfield.color, a)));
        }
    } else if (fx == "aurora") {
        for (int i = 0; i < 6; ++i) {
            const float yy = y + h * (0.15f + i * 0.13f);
            const float phase = time * 0.4f + i * 0.7f;
            D2D1_POINT_2F prev = Point2F(x, yy);
            uint32_t col = (i & 1) ? s.aurora.colorB : s.aurora.colorA;
            for (int k = 1; k <= 16; ++k) {
                const float t = k / 16.0f;
                const float xx = x + t * w;
                const float yp = yy + std::sin(t * 4.0f + phase) * 10.0f;
                rt->DrawLine(prev, Point2F(xx, yp), solid.get(rt, rgba(col, 0.35f)), 3.0f);
                prev = Point2F(xx, yp);
            }
        }
    } else if (fx == "fireflies") {
        fillRound(rt, solid.get(rt, rgba4(0.03f, 0.02f, 0.05f, 0.95f)), x, y, w, h, 12.0f);
        rt->FillRectangle(RectF(x, y + h * 0.62f, x + w, y + h), solid.get(rt, rgba4(0.01f, 0.012f, 0.014f, 0.9f)));
        for (int i = 0; i < 7; ++i) {
            const float px = x + w * (0.15f + (i % 5) * 0.15f);
            const float py = y + h * (0.35f + (i % 3) * 0.16f + 0.04f * std::sin(time + i));
            const float a = 0.35f + (i % 3) * 0.2f;
            rt->FillEllipse(D2D1::Ellipse(Point2F(px, py), 3.5f, 3.5f), solid.get(rt, rgba(s.fireflies.color, a * 0.45f)));
            rt->FillEllipse(D2D1::Ellipse(Point2F(px, py), 1.3f, 1.3f), solid.get(rt, rgba(s.fireflies.color, a)));
        }
    } else if (fx == "snow") {
        const bool window = s.snow.backdrop == 2;
        fillRound(rt, solid.get(rt, window ? rgba4(0.25f, 0.12f, 0.05f, 0.95f) : rgba4(0.02f, 0.03f, 0.06f, 0.95f)),
                  x, y, w, h, 12.0f);
        if (s.snow.backdrop == 1) {
            rt->FillRectangle(RectF(x, y + h * 0.55f, x + w, y + h), solid.get(rt, rgba4(0.01f, 0.015f, 0.018f, 0.95f)));
        }
        for (int i = 0; i < 18; ++i) {
            const float px = x + std::fmod(i * 37.0f + time * (8.0f + (i % 4) * 3.0f), w);
            const float py = y + std::fmod(i * 19.0f + time * 14.0f, h);
            const float rad = (i % 5 == 0) ? 3.2f : 1.2f;
            rt->FillEllipse(D2D1::Ellipse(Point2F(px, py), rad, rad), solid.get(rt, rgba(s.snow.color, 0.75f)));
        }
    } else if (fx == "lava") {
        fillRound(rt, solid.get(rt, rgba(s.lava.liquidColor, 0.95f)), x, y, w, h, 12.0f);
        const uint32_t lc = s.lava.lavaColor;
        for (int i = 0; i < 5; ++i) {
            const float cx = x + w * (0.22f + (i % 3) * 0.28f);
            const float cy = y + h * (0.70f - (i % 2) * 0.22f + 0.04f * std::sin(time * 0.8f + i));
            const float rad = 14.0f + (i % 3) * 5.0f;
            rt->FillEllipse(D2D1::Ellipse(Point2F(cx, cy), rad, rad * 1.15f), solid.get(rt, rgba(lc, 0.85f)));
        }
    } else {
        const uint32_t col = (fx == "ape")   ? s.ape.color
                             : (fx == "myco") ? (s.myco.colorize ? 0xD4787A : s.myco.color)
                                              : s.matrix.color;
        const int cols = 28;
        const float cw = w / (float)cols;
        for (int c = 0; c < cols; ++c) {
            const float head = std::fmod(time * (7.0f + (c % 5)) + c * 1.7f, h + 30.0f) - 8.0f;
            for (int g = 0; g < 11; ++g) {
                const float gy = y + std::fmod(head - g * 7.0f + h, h);
                float a = 0.08f + 0.55f * (1.0f - g / 11.0f);
                if (fx == "ape" || fx == "myco") {
                    const float nx = (c + 0.5f) / cols;
                    const float ny = (gy - y) / h;
                    const float dx = nx - 0.50f;
                    const float dy = ny - (fx == "myco" ? 0.52f : 0.58f);
                    const float blob = clampf(1.0f - (dx * dx * 7.5f + dy * dy * 4.2f), 0.0f, 1.0f);
                    a *= lerp(0.45f, 1.35f, blob);
                }
                const float gh = (g == 0) ? 5.0f : 3.4f;
                rt->FillRectangle(RectF(x + c * cw + 1.5f, gy, x + c * cw + cw - 1.5f, gy + gh),
                                  solid.get(rt, rgba(col, clampf(a, 0.04f, 0.95f))));
            }
        }
    }
    rt->PopAxisAlignedClip();
    drawRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.10f)), x, y, w, h, 12.0f, 1.0f);
}

// Match tools/gen_icon.py CORNER_FRAC so the D2D clip agrees with the 256 px mask.
constexpr float kLogoCorner = 0.223f;

bool decodeLogoResource(IWICImagingFactory* wic, HINSTANCE inst, IWICBitmap** out) {
    if (!wic || !inst || !out) return false;
    HRSRC hrsrc = FindResourceW(inst, MAKEINTRESOURCEW(IDR_APPLOGO), RT_RCDATA);
    if (!hrsrc) return false;
    HGLOBAL hg = LoadResource(inst, hrsrc);
    if (!hg) return false;
    const void* data = LockResource(hg);
    const DWORD nbytes = SizeofResource(inst, hrsrc);
    if (!data || !nbytes) return false;

    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, nbytes);
    if (!copy) return false;
    void* locked = GlobalLock(copy);
    if (!locked) {
        GlobalFree(copy);
        return false;
    }
    std::memcpy(locked, data, nbytes);
    GlobalUnlock(copy);

    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, stream.put()))) {
        GlobalFree(copy);
        return false;
    }
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, dec.put()))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, frame.put()))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.put()))) return false;
    // Premultiplied BGRA so D2D acrylic compositing does not fringe.
    if (FAILED(conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                0.0, WICBitmapPaletteTypeMedianCut))) {
        return false;
    }
    return SUCCEEDED(wic->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, out));
}

bool scaleLogoFant(IWICImagingFactory* wic, IWICBitmap* src, UINT destPx, IWICBitmap** out) {
    if (!wic || !src || !out || destPx == 0) return false;
    UINT sw = 0, sh = 0;
    src->GetSize(&sw, &sh);
    IWICBitmapSource* chain = src;
    ComPtr<IWICBitmapScaler> scaler;
    if (sw != destPx || sh != destPx) {
        if (FAILED(wic->CreateBitmapScaler(scaler.put()))) return false;
        // Fant (WIC high-quality) downscale from the 256 px master to exact device pixels.
        HRESULT hr = scaler->Initialize(src, destPx, destPx, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return false;
        chain = scaler.get();
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.put()))) return false;
    if (FAILED(conv->Initialize(chain, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeMedianCut))) {
        return false;
    }
    return SUCCEEDED(wic->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, out));
}

}  // namespace

HWND DesktopWidget::sanitizeOwner(HWND owner) const {
    if (!owner || !IsWindow(owner)) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    if (pid != GetCurrentProcessId()) {
        LOG_WARN("Widget owner %p is cross-process — ignoring", (void*)owner);
        return nullptr;
    }
    const LONG_PTR style = GetWindowLongPtrW(owner, GWL_STYLE);
    if (style & WS_CHILD) {
        // A WS_CHILD owner makes CreateWindowEx(WS_POPUP) fail with 1400.
        HWND root = GetAncestor(owner, GA_ROOT);
        if (root && root != owner && IsWindow(root)) {
            DWORD rpid = 0;
            GetWindowThreadProcessId(root, &rpid);
            if (rpid == GetCurrentProcessId() && !(GetWindowLongPtrW(root, GWL_STYLE) & WS_CHILD)) {
                LOG_INFO("Widget owner %p is WS_CHILD; using root %p", (void*)owner, (void*)root);
                return root;
            }
        }
        LOG_WARN("Widget owner %p is WS_CHILD with no usable root — using no owner", (void*)owner);
        return nullptr;
    }
    return owner;
}

bool DesktopWidget::createTopLevelWindow(HWND owner) {
    struct Attempt {
        const char* name;
        DWORD ex;
        HWND owner;
    };
    const Attempt attempts[] = {
        {"tool+noactivate+owner", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, owner},
        {"tool+noactivate", WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, nullptr},
        {"tool+owner", WS_EX_TOOLWINDOW, owner},
        {"tool", WS_EX_TOOLWINDOW, nullptr},
        {"popup", 0, nullptr},
    };
    for (const auto& a : attempts) {
        if (a.owner && !IsWindow(a.owner)) continue;
        SetLastError(0);
        HWND hwnd = CreateWindowExW(a.ex, kClass, L"Stalgia", WS_POPUP, 0, 0, 8, 8, a.owner, nullptr,
                                    m_instance, this);
        if (hwnd) {
            m_hwnd = hwnd;
            m_owner = a.owner;
            LOG_INFO("Widget CreateWindowEx ok mode=%s hwnd=%p owner=%p ex=0x%08lX", a.name, (void*)hwnd,
                     (void*)a.owner, a.ex);
            return true;
        }
        LOG_WARN("Widget CreateWindowEx failed mode=%s owner=%p ex=0x%08lX err=%lu", a.name, (void*)a.owner,
                 a.ex, GetLastError());
    }
    return false;
}

bool DesktopWidget::applyDwmAcrylic() {
    if (!m_hwnd) return false;
    MARGINS margins{-1, -1, -1, -1};
    HRESULT hrExtend = DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWORD corners = DWMWCP_ROUND;
    HRESULT hrCorner = DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    DWORD backdrop = DWMSBT_TRANSIENTWINDOW;
    HRESULT hrBack = DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    LOG_INFO("Widget DWM extend=0x%08X corner=0x%08X acrylic=0x%08X", (unsigned)hrExtend, (unsigned)hrCorner,
             (unsigned)hrBack);
    return SUCCEEDED(hrBack);
}

bool DesktopWidget::enableLayeredFallback() {
    if (!m_hwnd) return false;
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    LOG_INFO("Widget using layered UpdateLayeredWindow fallback hwnd=%p", (void*)m_hwnd);
    return true;
}

bool DesktopWidget::ensureHwndTarget() {
    if (!m_hwnd || !m_d2d) return false;
    auto* d2d = static_cast<ID2D1Factory*>(m_d2d);
    if (m_hwndRt) {
        auto* rt = static_cast<ID2D1HwndRenderTarget*>(m_hwndRt);
        HRESULT hr = rt->Resize(D2D1::SizeU((UINT32)m_pixelW, (UINT32)m_pixelH));
        if (SUCCEEDED(hr)) return true;
        LOG_WARN("Widget HWND RT Resize failed 0x%08X — recreating", (unsigned)hr);
        releaseHwndTarget();
    }
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)m_dpi,
        (float)m_dpi);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps = D2D1::HwndRenderTargetProperties(
        m_hwnd, D2D1::SizeU((UINT32)std::max(1, m_pixelW), (UINT32)std::max(1, m_pixelH)),
        D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    ID2D1HwndRenderTarget* rt = nullptr;
    HRESULT hr = d2d->CreateHwndRenderTarget(props, hwndProps, &rt);
    if (FAILED(hr) || !rt) {
        LOG_WARN("CreateHwndRenderTarget failed 0x%08X", (unsigned)hr);
        return false;
    }
    m_hwndRt = rt;
    return true;
}

void DesktopWidget::releaseHwndTarget() {
    if (m_hwndRt) {
        static_cast<IUnknown*>(m_hwndRt)->Release();
        m_hwndRt = nullptr;
    }
}

bool DesktopWidget::create(HINSTANCE instance, SettingsPanel::Host* host, bool desktopLayer, HWND owner) {
    if (m_hwnd) return true;
    m_instance = instance;
    m_host = host;
    m_desktopLayer = desktopLayer;
    if (!initFactories()) {
        LOG_ERR("Desktop widget: Direct2D/DirectWrite init failed");
        return false;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = loadAppIcon(instance, largeIconPx(nullptr));
    wc.hIconSm = loadAppIcon(instance, smallIconPx(nullptr));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LOG_ERR("RegisterClassEx(StalgiaWidget) failed: %lu", GetLastError());
        return false;
    }
    m_classReg = true;

    owner = sanitizeOwner(owner);
    if (!createTopLevelWindow(owner)) {
        LOG_ERR("CreateWindowEx(StalgiaWidget) failed after fallbacks");
        return false;
    }

    updateDpi();
    if (m_host) {
        m_miniTarget = m_host->settings().widgetMini ? 1.0f : 0.0f;
        m_miniT = m_miniTarget;
        m_drawerTarget = 0;
        m_drawerT = 0;
    }

    applyWindowIcons(m_hwnd, instance);

    m_useDwm = applyDwmAcrylic() && ensureHwndTarget();
    if (!m_useDwm) {
        releaseHwndTarget();
        enableLayeredFallback();
    }

    m_sys.start(m_hwnd, kSampleMsg);
    syncSysSampling();

    computeSize();
    positionWindow(false);
    if (m_desktopLayer) {
        pinDesktopZ();
        SetTimer(m_hwnd, kTimerZ, 1000, nullptr);
    }
    paint();
    LOG_INFO("Desktop widget ready hwnd=%p owner=%p mode=%s dpi=%u scale=%.2f %dx%d desktopZ=%d",
             (void*)m_hwnd, (void*)m_owner, m_useDwm ? "dwm-acrylic" : "layered", m_dpi, m_scale, m_pixelW,
             m_pixelH, m_desktopLayer ? 1 : 0);
    return true;
}

void DesktopWidget::destroy() {
    m_sys.setPaused(true);
    m_sys.stop();
    stopAnimIfIdle();
    releaseHwndTarget();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    releaseFactories();
    m_visible = false;
    m_useDwm = false;
}

void DesktopWidget::show() {
    if (!m_hwnd && m_instance && m_host) create(m_instance, m_host, m_desktopLayer);
    if (!m_hwnd) return;
    m_visible = true;
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    if (m_desktopLayer) pinDesktopZ();
    syncSysSampling();
    paint();
    if (m_host) {
        m_host->settings().showWidget = true;
        m_host->applySettingsLive();
    }
    startAnim();
}

void DesktopWidget::hide() {
    m_visible = false;
    m_sys.setPaused(true);
    if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
    if (m_timerOn && m_hwnd) {
        KillTimer(m_hwnd, kTimerAnim);
        m_timerOn = false;
    }
    if (m_host) {
        m_host->settings().showWidget = false;
        m_host->applySettingsLive();
    }
}

void DesktopWidget::refresh() {
    if (m_hwnd && m_visible) paint();
}

bool DesktopWidget::visible() const { return m_hwnd && m_visible; }

bool DesktopWidget::wantsLiveThumb() const {
    return m_visible && m_hwnd && IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd) && m_miniT < 0.85f;
}

bool DesktopWidget::initFactories() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_ID2D1Factory, nullptr, &m_d2d);
    if (FAILED(hr) || !m_d2d) {
        LOG_ERR("D2D1CreateFactory 0x%08X", (unsigned)hr);
        return false;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, IID_IDWriteFactory, (IUnknown**)&m_dwrite);
    if (FAILED(hr) || !m_dwrite) {
        LOG_ERR("DWriteCreateFactory 0x%08X", (unsigned)hr);
        return false;
    }
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                          &m_wic);
    if (FAILED(hr) || !m_wic) {
        hr = CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                              &m_wic);
    }
    if (FAILED(hr) || !m_wic) {
        LOG_ERR("WIC factory 0x%08X", (unsigned)hr);
        return false;
    }
    return true;
}

void DesktopWidget::releaseLogoCache() {
    if (m_logoScaled) {
        static_cast<IUnknown*>(m_logoScaled)->Release();
        m_logoScaled = nullptr;
    }
    if (m_logoSrc) {
        static_cast<IUnknown*>(m_logoSrc)->Release();
        m_logoSrc = nullptr;
    }
    m_logoPx = 0;
}

void DesktopWidget::releaseFactories() {
    releaseLogoCache();
    if (m_d2d) {
        static_cast<IUnknown*>(m_d2d)->Release();
        m_d2d = nullptr;
    }
    if (m_dwrite) {
        static_cast<IUnknown*>(m_dwrite)->Release();
        m_dwrite = nullptr;
    }
    if (m_wic) {
        static_cast<IUnknown*>(m_wic)->Release();
        m_wic = nullptr;
    }
}

IWICBitmap* DesktopWidget::ensureLogoScaled(IWICImagingFactory* wic, UINT destPx) {
    if (!wic || !m_instance || destPx == 0) return nullptr;
    if (m_logoScaled && m_logoPx == (int)destPx) return static_cast<IWICBitmap*>(m_logoScaled);

    if (!m_logoSrc) {
        ComPtr<IWICBitmap> src;
        if (!decodeLogoResource(wic, m_instance, src.put()) || !src) return nullptr;
        m_logoSrc = src.detach();
    }

    ComPtr<IWICBitmap> scaled;
    if (!scaleLogoFant(wic, static_cast<IWICBitmap*>(m_logoSrc), destPx, scaled.put()) || !scaled) {
        return nullptr;
    }
    if (m_logoScaled) {
        static_cast<IUnknown*>(m_logoScaled)->Release();
        m_logoScaled = nullptr;
    }
    m_logoScaled = scaled.detach();
    m_logoPx = (int)destPx;
    return static_cast<IWICBitmap*>(m_logoScaled);
}

bool DesktopWidget::drawAppBadge(ID2D1RenderTarget* rt, IWICImagingFactory* wic, const DipRect& box) {
    if (!rt || !wic || box.w < 1.0f || box.h < 1.0f) return false;
    const UINT destPx = (UINT)std::max(1, (int)std::lround(box.w * m_scale));
    IWICBitmap* scaled = ensureLogoScaled(wic, destPx);
    if (!scaled) return false;

    // Tag the bitmap with the window DPI so dest DIP size maps 1:1 onto destPx.
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)m_dpi,
        (float)m_dpi);
    ComPtr<ID2D1Bitmap> logo;
    if (FAILED(rt->CreateBitmapFromWicBitmap(scaled, &props, logo.put())) || !logo) return false;

    const D2D1_RECT_F dest = RectF(box.x, box.y, box.x + box.w, box.y + box.h);
    D2D1_BITMAP_INTERPOLATION_MODE filter = D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    const D2D1_SIZE_F dipSize = logo->GetSize();
    if (std::fabs(dipSize.width - box.w) > 0.25f || std::fabs(dipSize.height - box.h) > 0.25f) {
        filter = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
    }

    const float rr = std::min(box.w, box.h) * kLogoCorner;
    auto* d2d = static_cast<ID2D1Factory*>(m_d2d);
    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    ComPtr<ID2D1Layer> layer;
    if (d2d && SUCCEEDED(d2d->CreateRoundedRectangleGeometry(RoundedRect(dest, rr, rr), geo.put())) &&
        SUCCEEDED(rt->CreateLayer(SizeF(box.w + 2.0f, box.h + 2.0f), layer.put()))) {
        D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(
            RectF(box.x - 1.0f, box.y - 1.0f, box.x + box.w + 1.0f, box.y + box.h + 1.0f), geo.get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        rt->PushLayer(lp, layer.get());
        rt->DrawBitmap(logo.get(), dest, 1.0f, filter);
        rt->PopLayer();
        return true;
    }
    rt->DrawBitmap(logo.get(), dest, 1.0f, filter);
    return true;
}

void DesktopWidget::updateDpi() {
    m_dpi = windowDpi(m_hwnd);
    m_scale = m_dpi / 96.0f;
}

void DesktopWidget::computeSize() {
    const Layout L = buildLayout();
    m_pixelW = std::max(8, (int)std::ceil((L.cardW + L.shadow * 2.0f) * m_scale));
    m_pixelH = std::max(8, (int)std::ceil((L.cardH + L.shadow * 2.0f) * m_scale));
}

DesktopWidget::Layout DesktopWidget::buildLayout() const {
    Layout L;
    L.shadow = m_useDwm ? 0.0f : kShadow;
    const float mini = m_miniT;
    const float drawer = m_drawerT * (1.0f - mini);
    const float bodyH = 16.0f + 44.0f + 10.0f + 96.0f + 12.0f + 58.0f + 12.0f + 48.0f + 12.0f + kSysH + 16.0f;
    L.cardW = lerp(kCardW, kMiniW, mini);
    L.cardH = lerp(bodyH, kMiniH, mini);
    const float x0 = L.shadow;
    const float y0 = L.shadow;
    L.card = {x0, y0, L.cardW, L.cardH};

    if (mini > 0.92f) {
        L.logo = {x0 + 8, y0 + 8, 28, 28};
        L.title = {x0 + 44, y0 + 4, 68, 36};
        L.miniCpu = {x0 + 120, y0 + 8, 36, 28};
        L.play = {x0 + L.cardW - 76, y0 + 6, 32, 32};
        L.miniBtn = {x0 + L.cardW - 40, y0 + 6, 32, 32};
        return L;
    }

    L.header = {x0 + kInset, y0 + 14, L.cardW - kInset * 2, 36};
    L.logo = {L.header.x, L.header.y, 32, 32};
    L.title = {L.header.x + 40, L.header.y - 2, 180, 36};
    L.miniBtn = {x0 + L.cardW - kInset - 30, L.header.y + 2, 28, 28};
    L.preview = {x0 + kInset, y0 + 58, L.cardW - kInset * 2, 96};

    const float ty = L.preview.y + L.preview.h + 12.0f;
    L.wallpaper = {x0 + kInset, ty, L.cardW - kInset * 2, 58.0f};
    L.wallThumb = {L.wallpaper.x + 8.0f, L.wallpaper.y + 11.0f, 64.0f, 36.0f};
    L.wallNext = {L.wallpaper.x + L.wallpaper.w - 36.0f, L.wallpaper.y + 13.0f, 28.0f, 32.0f};
    L.wallPrev = {L.wallNext.x - 32.0f, L.wallpaper.y + 13.0f, 28.0f, 32.0f};
    const float cy = ty + 58.0f + 12.0f;
    L.play = {x0 + kInset, cy, 40, 40};
    L.gear = {x0 + L.cardW - kInset - 40, cy, 40, 40};
    L.fps = {L.play.x + 48, cy, 78, 40};
    L.icons = {L.fps.x + 86, cy, 40, 40};
    L.sys = {x0 + kInset, cy + 48.0f, L.cardW - kInset * 2, kSysH};
    const float gw = 72.0f;
    const float gapG = (L.sys.w - gw * 3.0f) / 2.0f;
    for (int i = 0; i < 3; ++i) {
        L.gauges[i] = {L.sys.x + i * (gw + gapG), L.sys.y + 16.0f, gw, 74.0f};
    }
    L.spark = {L.sys.x + 8.0f, L.sys.y + 92.0f, L.sys.w - 16.0f, 16.0f};
    L.sysFoot = {L.sys.x, L.sys.y + 110.0f, L.sys.w, 34.0f};
    const float drawerX = x0 + kInset;
    const float drawerTop = L.sys.y + L.sys.h + 8.0f;
    const float panelW = L.cardW - kInset * 2.0f;
    const float contentX = drawerX + ui::kPad;
    const float contentW = ui::contentWidth(panelW);
    const std::string effectId = m_host ? m_host->settings().effect : std::string();
    const AppSettings* settings = m_host ? &m_host->settings() : nullptr;
    constexpr float kFont = 12.0f;

    float labelNeed = ui::kMinLabel;
    float valueNeed = ui::textWidth(L"1.00x", kFont);
    const int knobN = knobCount(effectId);
    for (int i = 0; i < knobN; ++i) {
        const Knob knob = knobAt(effectId, i);
        if (knob.kind != KnobKind::Slider || !knob.label) continue;
        labelNeed = std::max(labelNeed, ui::textWidth(knob.label, kFont));
        if (settings) valueNeed = std::max(valueNeed, ui::textWidth(knobReadout(*settings, effectId, knob.index).c_str(), kFont));
    }
    const ui::Columns cols = ui::columnsFor(contentW, labelNeed, valueNeed);

    // Flow in content coordinates, then shift by the drawer origin and scroll.
    // The gallery button is part of that flow, so the scroll height includes it
    // and the bottom pad — the last row can scroll clear of the panel edge.
    float rel = ui::kPad;
    L.galleryBtn = {contentX, rel, contentW, ui::kBtnH};
    rel += ui::kBtnH + ui::kGap;
    {
        float cx = 0;
        float cy = rel;
        auto place = [&](int index, const std::wstring& label) {
            const float w = ui::chipWidth(label.c_str(), kFont, contentW);
            const float lead = (index < 0 && cx > 0.0f) ? ui::kGap : 0.0f;
            if (cx > 0.0f && cx + lead + w > contentW + 0.5f) {
                cy += ui::kChipH + ui::kChipGap;
                cx = 0;
            } else {
                cx += lead;
            }
            Layout::Chip chip;
            chip.index = index;
            chip.label = label;
            chip.box = {contentX + cx, cy, w, ui::kChipH};
            L.chips.push_back(chip);
            cx += w + ui::kChipGap;
        };
        const int presets = presetCount(effectId);
        for (int i = 0; i < presets; ++i) place(i, widen(presetName(effectId, i)));
        place(-1, L"Reset");
        rel = cy + ui::kChipH;
    }

    const wchar_t* sectionName[] = {L"Motion", L"Look", L"Color", L"Advanced"};
    for (int sec = 0; sec < 4; ++sec) {
        bool any = false;
        bool rowStarted = false;
        for (int i = 0; i < knobN; ++i) {
            const Knob knob = knobAt(effectId, i);
            if ((int)knob.section != sec || knob.kind == KnobKind::Button) continue;
            if (!settings) continue;
            if (!any) {
                rel += ui::kSectionGap;
                Layout::Head hd;
                hd.label = sectionName[sec];
                hd.box = {contentX, rel, contentW, ui::kHeaderH};
                L.heads.push_back(hd);
                rel += ui::kHeaderH + ui::kHeaderGap;
                any = true;
            }
            if (rowStarted) rel += ui::kRowGap;
            Layout::Row row;
            row.index = knob.index;
            row.kind = (int)knob.kind;
            row.label = knob.label;
            row.box = {contentX, rel, contentW, ui::kRowH};
            row.labelBox = {contentX, rel, cols.label, ui::kRowH};
            if (knob.kind == KnobKind::Slider) {
                const float value = knobValue(*settings, effectId, knob.index);
                const float span = knob.hi - knob.lo;
                row.t = span > 0 ? clampf((value - knob.lo) / span, 0.0f, 1.0f) : 0;
                row.value = knobReadout(*settings, effectId, knob.index);
                row.track = {contentX + cols.label + ui::kGap, rel + (ui::kRowH - ui::kTrackH) * 0.5f, cols.track,
                             ui::kTrackH};
                row.valueBox = {contentX + contentW - cols.value, rel, cols.value, ui::kRowH};
            } else if (knob.kind == KnobKind::Toggle) {
                row.on = knobToggle(*settings, effectId, knob.index);
            } else if (knob.kind == KnobKind::Swatch) {
                row.rgb = knobColor(*settings, effectId, knob.index);
                row.track = {contentX + contentW - 28.0f, rel + (ui::kRowH - 20.0f) * 0.5f, 28.0f, 20.0f};
                row.labelBox.w = std::max(0.0f, row.track.x - ui::kGap - contentX);
            } else {
                continue;
            }
            L.rows.push_back(row);
            rel += ui::kRowH;
            rowStarted = true;
        }
    }
    rel += ui::kBottom;
    L.contentH = rel;
    const float scroll = clampf(m_drawerScroll, 0.0f, std::max(0.0f, L.contentH - kDrawerMax));
    const float shift = drawerTop - scroll;
    L.galleryBtn.y += shift;
    for (auto& c : L.chips) c.box.y += shift;
    for (auto& h : L.heads) h.box.y += shift;
    for (auto& r : L.rows) {
        r.box.y += shift;
        r.track.y += shift;
        r.labelBox.y += shift;
        r.valueBox.y += shift;
    }
    const float vis = std::min(L.contentH, kDrawerMax) * std::max(drawer, 0.0f);
    L.drawer = {drawerX, drawerTop, panelW, vis};
    const float maxS = std::max(0.0f, L.contentH - kDrawerMax);
    if (maxS > 1.0f && L.drawer.h > 40.0f) {
        const float trackH = L.drawer.h - ui::kGap * 2.0f;
        const float thumb = std::max(22.0f, trackH * (std::min(L.contentH, kDrawerMax) / L.contentH));
        const float travel = std::max(1.0f, trackH - thumb);
        L.scrollBar = {ui::thumbX(drawerX, panelW), drawerTop + ui::kGap + travel * (scroll / maxS), ui::kThumbW, thumb};
    }
    L.cardH = lerp(bodyH + L.drawer.h, kMiniH, mini);
    L.card.h = L.cardH;
    return L;
}

void DesktopWidget::positionWindow(bool keepScreenPos) {
    computeSize();
    int x = 48, y = 72;
    if (keepScreenPos && m_hwnd) {
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        x = wr.left;
        y = wr.top;
    } else if (m_host) {
        const int sx = m_host->settings().widgetX;
        const int sy = m_host->settings().widgetY;
        if (sx >= 0 && sy >= 0) {
            x = sx;
            y = sy;
        } else {
            POINT pt{0, 0};
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                x = mi.rcWork.right - m_pixelW - 28;
                y = mi.rcWork.top + 64;
            }
        }
    }
    SetWindowPos(m_hwnd, nullptr, x, y, m_pixelW, m_pixelH, SWP_NOACTIVATE | SWP_NOZORDER);
    if (m_desktopLayer) pinDesktopZ();
}

void DesktopWidget::pinDesktopZ() {
    if (!m_hwnd || m_pinningZ) return;
    m_pinningZ = true;
    // Never parent to Progman/DefView. Stay a top-level popup sitting just above
    // the desktop (Progman) and below ordinary windows.
    // GetParent() on an *owned* WS_POPUP returns the owner, not a parent — do not
    // SetParent(nullptr) in that case (it stripped the owner every second).
    const LONG_PTR style = GetWindowLongPtrW(m_hwnd, GWL_STYLE);
    if (style & WS_CHILD) {
        HWND parent = GetParent(m_hwnd);
        if (parent) {
            SetParent(m_hwnd, nullptr);
            LOG_INFO("Widget detached from parent, now top-level hwnd=%p", (void*)m_hwnd);
        }
    }
    HWND desk = FindWindowW(L"Progman", nullptr);
    if (!desk) desk = GetShellWindow();
    HWND insertAfter = HWND_BOTTOM;
    if (desk) {
        HWND prev = GetWindow(desk, GW_HWNDPREV);
        if (prev && prev != m_hwnd) insertAfter = prev;
    }
    SetWindowPos(m_hwnd, insertAfter, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (m_visible && !IsWindowVisible(m_hwnd)) {
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }
    m_pinningZ = false;
}

void DesktopWidget::startAnim() {
    if (!m_hwnd) return;
    if (!m_timerOn) {
        SetTimer(m_hwnd, kTimerAnim, 16, nullptr);
        m_timerOn = true;
        m_lastTick = GetTickCount64();
    }
}

void DesktopWidget::stopAnimIfIdle() {
    if (!m_timerOn || !m_hwnd) return;
    bool busy = m_draggingWin || m_dragSlider >= 0 || m_trackingLeave;
    busy = busy || std::fabs(m_drawerT - m_drawerTarget) > 0.004f;
    busy = busy || std::fabs(m_miniT - m_miniTarget) > 0.004f;
    busy = busy || std::fabs(m_dCpu - m_sysSnap.cpuPct) > 0.2f;
    busy = busy || std::fabs(m_dRam - m_sysSnap.ramPct) > 0.2f;
    busy = busy || std::fabs(m_dGpu - m_sysSnap.gpuPct) > 0.2f;
    if (m_hot.kind != HitKind::None) busy = true;
    for (float h : m_hover) {
        if (h > 0.01f) busy = true;
    }
    if (busy) return;
    KillTimer(m_hwnd, kTimerAnim);
    m_timerOn = false;
}

void DesktopWidget::tickAnim() {
    const ULONGLONG now = GetTickCount64();
    float dt = (m_lastTick ? (now - m_lastTick) : 16) / 1000.0f;
    m_lastTick = now;
    dt = clampf(dt, 0.001f, 0.05f);
    m_drawerT = approach(m_drawerT, m_drawerTarget, dt, 7.0f);
    m_miniT = approach(m_miniT, m_miniTarget, dt, 8.0f);
    m_dCpu = approach(m_dCpu, m_sysSnap.cpuPct, dt, 4.5f);
    m_dRam = approach(m_dRam, m_sysSnap.ramPct, dt, 4.5f);
    if (m_sysSnap.gpuValid) m_dGpu = approach(m_dGpu, m_sysSnap.gpuPct, dt, 4.5f);
    if (m_hot.kind != HitKind::None || m_trackingLeave) m_previewTime += dt;

    const int hotIdx = hoverSlot(m_hot);
    for (int i = 0; i < (int)(sizeof(m_hover) / sizeof(m_hover[0])); ++i) {
        const float target = (hotIdx == i) ? 1.0f : 0.0f;
        m_hover[i] = approach(m_hover[i], target, dt, 12.0f);
    }
    const int oldW = m_pixelW, oldH = m_pixelH;
    computeSize();
    if (oldW != m_pixelW || oldH != m_pixelH) {
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        SetWindowPos(m_hwnd, nullptr, wr.left, wr.top, m_pixelW, m_pixelH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    paint();
    stopAnimIfIdle();
}

void DesktopWidget::syncSysSampling() {
    const bool hidden = !m_visible;
    const bool mini = m_miniTarget > 0.5f;
    m_sys.setPaused(hidden);
    m_sys.setMiniMode(mini);
}

void DesktopWidget::onSysSample() {
    m_sysSnap = m_sys.snapshot();
    startAnim();
    if (!m_timerOn) paint();
}

uint32_t DesktopWidget::healthRgb(float pct, uint32_t accent) const {
    if (pct >= 90.0f) return 0xFF4D4D;
    if (pct >= 75.0f) return 0xFFB020;
    return accent;
}

uint32_t DesktopWidget::accentRgb() const {
    if (!m_host) return 0x00FF41;
    const std::string& id = m_host->settings().effect;
    if (id == "starfield") return m_host->settings().starfield.color;
    if (id == "aurora") return m_host->settings().aurora.colorA;
    if (id == "lava") return m_host->settings().lava.lavaColor;
    if (id == "fireflies") return m_host->settings().fireflies.color;
    if (id == "snow") return m_host->settings().snow.color;
    if (id == "ape") return 0xF2F6FF;
    if (id == "myco") return 0xD4787A;
    return m_host->settings().matrix.color;
}

int DesktopWidget::hoverSlot(Hit hit) const {
    const int k = (int)hit.kind;
    if (hit.kind == HitKind::None || k < 0 || k >= (int)HitKind::Drawer) return -1;
    if (k >= (int)(sizeof(m_hover) / sizeof(m_hover[0]))) return -1;
    return k;
}

namespace {
uint32_t nextSwatch(uint32_t c) {
    static const uint32_t kCols[] = {0x00FF41u, 0xFF6A14u, 0x7EB6FFu, 0xFF4AD2u, 0xFFE08Au, 0xFFFFFFu};
    for (int i = 0; i < 6; ++i)
        if (kCols[i] == c) return kCols[(i + 1) % 6];
    return kCols[0];
}
}  // namespace


void DesktopWidget::forceFullSampling() {
    m_miniTarget = 0.0f;
    m_miniT = 0.0f;
    syncSysSampling();
}

void DesktopWidget::setMini(bool mini) {
    m_miniTarget = mini ? 1.0f : 0.0f;
    if (m_host) {
        m_host->settings().widgetMini = mini;
        m_host->applySettingsLive();
    }
    syncSysSampling();
    startAnim();
}

void DesktopWidget::saveChrome() {
    if (!m_host || !m_hwnd) return;
    RECT wr{};
    GetWindowRect(m_hwnd, &wr);
    m_host->settings().widgetX = wr.left;
    m_host->settings().widgetY = wr.top;
    m_host->applySettingsLive();
}

DesktopWidget::Hit DesktopWidget::hitTest(float x, float y) const {
    const Layout L = buildLayout();
    Hit h;
    if (L.miniBtn.contains(x, y)) {
        h.kind = HitKind::MiniToggle;
        return h;
    }
    if (m_miniT > 0.85f) {
        if (L.play.contains(x, y)) h.kind = HitKind::Play;
        else if (L.card.contains(x, y)) h.kind = HitKind::Drag;
        return h;
    }
    if (L.play.contains(x, y)) h.kind = HitKind::Play;
    else if (L.gear.contains(x, y)) h.kind = HitKind::Gear;
    else if (L.fps.contains(x, y)) h.kind = HitKind::Fps;
    else if (L.icons.contains(x, y)) h.kind = HitKind::Icons;
    else if (L.wallPrev.contains(x, y)) h.kind = HitKind::Tile1;
    else if (L.wallNext.contains(x, y)) h.kind = HitKind::Tile2;
    else if (L.wallpaper.contains(x, y)) h.kind = HitKind::Tile0;
    if (h.kind != HitKind::None) return h;

    // The drawer sits inside the card. Test it first so a click edits a knob
    // instead of dragging the window.
    if (L.drawer.h > 12.0f && L.drawer.contains(x, y)) {
        if (L.scrollBar.h > 1.0f) {
            DipRect fat = L.scrollBar;
            fat.x -= 4.0f;
            fat.w += 8.0f;
            if (fat.contains(x, y)) {
                h.kind = HitKind::Scroll;
                return h;
            }
        }
        if (L.galleryBtn.contains(x, y)) {
            h.kind = HitKind::Gallery;
            return h;
        }
        for (const auto& c : L.chips) {
            if (c.box.contains(x, y)) {
                h.kind = HitKind::Preset;
                h.index = c.index;
                return h;
            }
        }
        for (const auto& r : L.rows) {
            if (!r.box.contains(x, y)) continue;
            if (r.kind == (int)KnobKind::Slider) h.kind = HitKind::Slider;
            else if (r.kind == (int)KnobKind::Toggle) h.kind = HitKind::Toggle;
            else if (r.kind == (int)KnobKind::Swatch) h.kind = HitKind::Swatch;
            else continue;
            h.index = r.index;
            return h;
        }
        h.kind = HitKind::Drawer;
        return h;
    }
    if (L.header.contains(x, y) || L.preview.contains(x, y) || L.card.contains(x, y)) h.kind = HitKind::Drag;
    return h;
}

void DesktopWidget::onClick(Hit hit, float x, float y) {
    (void)y;
    if (!m_host) return;
    if (hit.kind == HitKind::MiniToggle) {
        setMini(m_miniTarget < 0.5f);
        return;
    }
    if (hit.kind == HitKind::Play) {
        m_host->setUserPaused(!m_host->userPaused());
        paint();
        return;
    }
    if (hit.kind == HitKind::Gear) {
        m_drawerTarget = m_drawerTarget > 0.5f ? 0.0f : 1.0f;
        m_drawerScroll = 0;
        startAnim();
        return;
    }
    if (hit.kind == HitKind::Fps) {
        int fps = m_host->settings().fps;
        if (fps < 22) fps = 30;
        else if (fps < 45) fps = 60;
        else fps = 15;
        m_host->settings().fps = fps;
        m_host->applySettingsLive();
        paint();
        return;
    }
    if (hit.kind == HitKind::Icons) {
        m_host->setHideDesktopIcons(!m_host->settings().hideDesktopIcons);
        paint();
        return;
    }
    if (hit.kind == HitKind::Tile1) {
        m_host->cycleWallpaper(-1);
        paint();
        return;
    }
    if (hit.kind == HitKind::Tile2) {
        m_host->cycleWallpaper(1);
        paint();
        return;
    }
    if (hit.kind == HitKind::Tile0 || hit.kind == HitKind::Gallery) {
        m_host->openGallery();
        return;
    }
    if (hit.kind == HitKind::Preset) {
        if (hit.index < 0) resetEffect(m_host->settings(), m_host->settings().effect);
        else applyPreset(m_host->settings(), m_host->settings().effect, hit.index);
        m_host->applySettingsLive();
        paint();
        return;
    }
    if (hit.kind == HitKind::Toggle) {
        const std::string& id = m_host->settings().effect;
        const bool on = knobToggle(m_host->settings(), id, hit.index);
        setKnobToggle(m_host->settings(), id, hit.index, !on);
        m_host->applySettingsLive();
        paint();
        return;
    }
    if (hit.kind == HitKind::Swatch) {
        const std::string& id = m_host->settings().effect;
        const uint32_t cur = knobColor(m_host->settings(), id, hit.index);
        setKnobColor(m_host->settings(), id, hit.index, nextSwatch(cur));
        m_host->applySettingsLive();
        paint();
        return;
    }
    if (hit.kind == HitKind::Slider) {
        m_dragSlider = hit.index;
        onDragSlider(x);
        SetCapture(m_hwnd);
        startAnim();
    }
}

void DesktopWidget::onDragSlider(float x) {
    if (m_dragSlider < 0 || !m_host) return;
    const Layout L = buildLayout();
    const Layout::Row* row = nullptr;
    for (const auto& r : L.rows) {
        if (r.kind == (int)KnobKind::Slider && r.index == m_dragSlider) {
            row = &r;
            break;
        }
    }
    if (!row || row->track.w < 1.0f) return;
    const float t = (x - row->track.x) / row->track.w;
    setKnob(m_host->settings(), m_host->settings().effect, m_dragSlider, t);
    m_host->applySettingsLive();
}

bool DesktopWidget::presentLayer(const unsigned char* bgra, int w, int h) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (mem) DeleteDC(mem);
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }
    std::memcpy(bits, bgra, (size_t)w * (size_t)h * 4);
    HGDIOBJ old = SelectObject(mem, dib);
    POINT src{0, 0};
    SIZE sz{w, h};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    const BOOL ok = UpdateLayeredWindow(m_hwnd, screen, nullptr, &sz, mem, &src, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok != 0;
}

void DesktopWidget::paint() {
    if (!m_hwnd || !m_d2d || !m_dwrite || !m_host) return;
    updateDpi();
    computeSize();
    auto* d2d = static_cast<ID2D1Factory*>(m_d2d);
    auto* dwrite = static_cast<IDWriteFactory*>(m_dwrite);
    auto* wic = static_cast<IWICImagingFactory*>(m_wic);

    if (m_useDwm && !ensureHwndTarget()) {
        LOG_WARN("Widget HWND render target failed — falling back to layered");
        m_useDwm = false;
        enableLayeredFallback();
        computeSize();
        positionWindow(true);
    }

    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> wicRt;
    ID2D1RenderTarget* rt = nullptr;
    HRESULT hr = S_OK;

    if (m_useDwm) {
        rt = static_cast<ID2D1RenderTarget*>(m_hwndRt);
    } else {
        if (!m_wic) return;
        hr = wic->CreateBitmap((UINT)m_pixelW, (UINT)m_pixelH, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bmp.put());
        if (FAILED(hr)) return;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)m_dpi,
            (float)m_dpi);
        hr = d2d->CreateWicBitmapRenderTarget(bmp.get(), props, wicRt.put());
        if (FAILED(hr)) return;
        rt = wicRt.get();
    }
    if (!rt) return;

    ComPtr<IDWriteTextFormat> tfTitle, tfSub, tfSmall, tfTiny, tfRow;
    const wchar_t* family = L"Segoe UI Variable Display";
    auto mk = [&](IDWriteTextFormat** out, float size, DWRITE_FONT_WEIGHT w) {
        HRESULT r = dwrite->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
        if (FAILED(r)) {
            dwrite->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
        }
    };
    mk(tfTitle.put(), 16.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(tfSub.put(), 11.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    mk(tfSmall.put(), 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    mk(tfTiny.put(), 9.0f, DWRITE_FONT_WEIGHT_NORMAL);
    mk(tfRow.put(), 12.0f, DWRITE_FONT_WEIGHT_NORMAL);

    const Layout L = buildLayout();
    const uint32_t accent = accentRgb();
    const auto& settings = m_host->settings();
    Solid solid;

    rt->BeginDraw();
    rt->Clear(ColorF(0, 0, 0, 0));
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Soft drop shadow (layered fallback only — DWM draws its own).
    if (!m_useDwm) {
        for (int i = 8; i >= 1; --i) {
            const float grow = (float)i * 2.4f;
            const float a = 0.035f * (1.0f - i / 9.0f);
            fillRound(rt, solid.get(rt, rgba4(0, 0, 0, a)), L.card.x - grow, L.card.y - grow + 6.0f,
                      L.card.w + grow * 2, L.card.h + grow * 2, kRadius + grow * 0.4f);
        }
    }

    // Accent glow matching the current wallpaper.
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[3];
        gs[0].position = 0;
        gs[0].color = rgba(accent, 0.22f);
        gs[1].position = 0.45f;
        gs[1].color = rgba(accent, 0.06f);
        gs[2].position = 1;
        gs[2].color = rgba(accent, 0.0f);
        if (SUCCEEDED(rt->CreateGradientStopCollection(gs, 3, stops.put()))) {
            ComPtr<ID2D1RadialGradientBrush> glow;
            D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES rp{};
            rp.center = Point2F(L.card.x + L.card.w * 0.5f, L.card.y + 8.0f);
            rp.radiusX = L.card.w * 0.62f;
            rp.radiusY = 90.0f;
            if (SUCCEEDED(rt->CreateRadialGradientBrush(rp, stops.get(), glow.put()))) {
                fillRound(rt, glow.get(), L.card.x - 8, L.card.y - 8, L.card.w + 16, 70.0f, kRadius + 6);
            }
        }
    }

    // Acrylic card. DWM blurs the wallpaper; we add a dark tint so glyphs stay
    // readable over busy Matrix rain. Layered fallback is nearly opaque.
    const float glassA = m_useDwm ? 0.72f : 0.96f;
    fillRound(rt, solid.get(rt, rgba4(0.10f, 0.11f, 0.14f, glassA)), L.card.x, L.card.y, L.card.w, L.card.h,
              kRadius);
    {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2];
        gs[0].position = 0;
        gs[0].color = rgba4(1, 1, 1, 0.07f);
        gs[1].position = 1;
        gs[1].color = rgba4(1, 1, 1, 0.0f);
        if (SUCCEEDED(rt->CreateGradientStopCollection(gs, 2, stops.put()))) {
            ComPtr<ID2D1LinearGradientBrush> shine;
            D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp{};
            lp.startPoint = Point2F(L.card.x, L.card.y);
            lp.endPoint = Point2F(L.card.x, L.card.y + 90.0f);
            if (SUCCEEDED(rt->CreateLinearGradientBrush(lp, stops.get(), shine.put()))) {
                fillRound(rt, shine.get(), L.card.x, L.card.y, L.card.w, 90.0f, kRadius);
            }
        }
    }
    drawRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.22f)), L.card.x + 0.5f, L.card.y + 0.5f, L.card.w - 1.0f,
              L.card.h - 1.0f, kRadius - 0.5f, 1.0f);
    drawRound(rt, solid.get(rt, rgba(accent, 0.55f)), L.card.x, L.card.y, L.card.w, L.card.h, kRadius, 1.25f);

    auto btnBg = [&](const DipRect& r, float hot, bool on) {
        const float a = 0.10f + hot * 0.12f + (on ? 0.10f : 0);
        fillRound(rt, solid.get(rt, rgba4(1, 1, 1, a)), r.x, r.y, r.w, r.h, r.h * 0.5f);
        if (on) drawRound(rt, solid.get(rt, rgba(accent, 0.7f)), r.x, r.y, r.w, r.h, r.h * 0.5f, 1.0f);
    };

    // App-icon badge: Fant-scale the 256 px master to exact device pixels, then
    // blit 1:1 inside an antialiased rounded-rect clip so acrylic edges stay smooth.
    if (!drawAppBadge(rt, wic, L.logo)) {
        const float rr = std::min(L.logo.w, L.logo.h) * kLogoCorner;
        fillRound(rt, solid.get(rt, rgba(accent, 0.16f)), L.logo.x, L.logo.y, L.logo.w, L.logo.h, rr);
        drawText(rt, tfTitle.get(), RectF(L.logo.x, L.logo.y, L.logo.x + L.logo.w, L.logo.y + L.logo.h), L"S",
                 solid.get(rt, rgba(accent, 0.95f)), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    std::wstring subtitle;
    for (const auto& e : m_host->registry().list()) {
        if (e.id == settings.effect) {
            subtitle = widen(e.displayName);
            break;
        }
    }
    drawText(rt, tfTitle.get(), RectF(L.title.x, L.title.y, L.title.x + L.title.w, L.title.y + 20), L"Stalgia",
             solid.get(rt, rgba4(0.94f, 0.96f, 0.98f, 0.96f)));
    if (m_miniT < 0.8f && !subtitle.empty()) {
        drawText(rt, tfTiny.get(), RectF(L.title.x, L.title.y + 18, L.title.x + L.title.w, L.title.y + 34),
                 subtitle.c_str(), solid.get(rt, rgba(accent, 0.80f)));
    }

    // Mini / expand chevron
    btnBg(L.miniBtn, m_hover[(int)HitKind::MiniToggle], false);
    {
        const float cx = L.miniBtn.x + L.miniBtn.w * 0.5f;
        const float cy = L.miniBtn.y + L.miniBtn.h * 0.5f;
        auto* br = solid.get(rt, rgba4(1, 1, 1, 0.75f));
        if (m_miniT > 0.5f) {
            rt->DrawLine(Point2F(cx - 5, cy + 1), Point2F(cx, cy - 4), br, 1.4f);
            rt->DrawLine(Point2F(cx, cy - 4), Point2F(cx + 5, cy + 1), br, 1.4f);
        } else {
            rt->DrawLine(Point2F(cx - 5, cy - 1), Point2F(cx, cy + 4), br, 1.4f);
            rt->DrawLine(Point2F(cx, cy + 4), Point2F(cx + 5, cy - 1), br, 1.4f);
        }
    }

    if (m_miniT < 0.85f) {
        drawPreview(rt, solid, settings, L.preview.x, L.preview.y, L.preview.w, L.preview.h,
                    m_previewTime + 4.2f);

        {
            const DipRect& bar = L.wallpaper;
            const float hot = m_hover[(int)HitKind::Tile0];
            fillRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.05f + hot * 0.06f)), bar.x, bar.y, bar.w, bar.h, 12.0f);
            drawRound(rt, solid.get(rt, rgba(accent, 0.45f)), bar.x, bar.y, bar.w, bar.h, 12.0f, 1.0f);
            fillRound(rt, solid.get(rt, rgba4(0.02f, 0.03f, 0.05f, 0.9f)), L.wallThumb.x, L.wallThumb.y,
                      L.wallThumb.w, L.wallThumb.h, 6.0f);
            std::wstring wallName = subtitle.empty() ? L"Wallpaper" : subtitle;
            std::wstring wallGroup;
            for (const auto& e : m_host->registry().list()) {
                if (e.id == settings.effect) {
                    wallGroup = widen(e.group);
                    break;
                }
            }
            if (ThumbnailStage* thumbs = m_host->thumbs()) {
                const uint8_t* px = thumbs->pixels(settings.effect);
                if (px) {
                    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), (float)m_dpi,
                        (float)m_dpi);
                    ComPtr<ID2D1Bitmap> bmp;
                    if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(ThumbnailStage::kWidth, ThumbnailStage::kHeight), px,
                                                   ThumbnailStage::kWidth * 4, bp, bmp.put()))) {
                        rt->DrawBitmap(bmp.get(),
                                       RectF(L.wallThumb.x, L.wallThumb.y, L.wallThumb.x + L.wallThumb.w,
                                             L.wallThumb.y + L.wallThumb.h),
                                       1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    }
                }
            }
            const float textX = L.wallThumb.x + L.wallThumb.w + 10.0f;
            const float textR = L.wallPrev.x - 6.0f;
            drawText(rt, tfSmall.get(), RectF(textX, bar.y + 8.0f, textR, bar.y + 28.0f), wallName.c_str(),
                     solid.get(rt, rgba4(0.95f, 0.96f, 0.98f, 0.96f)));
            drawText(rt, tfTiny.get(), RectF(textX, bar.y + 28.0f, textR, bar.y + 48.0f), wallGroup.c_str(),
                     solid.get(rt, rgba(accent, 0.80f)));
            auto chevron = [&](const DipRect& r, bool right, HitKind kind) {
                const float h = m_hover[(int)kind];
                fillRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.06f + h * 0.10f)), r.x, r.y, r.w, r.h, 8.0f);
                auto* br = solid.get(rt, rgba4(1, 1, 1, 0.82f));
                const float cx = r.x + r.w * 0.5f;
                const float cy = r.y + r.h * 0.5f;
                if (right) {
                    rt->DrawLine(Point2F(cx - 3, cy - 5), Point2F(cx + 3, cy), br, 1.5f);
                    rt->DrawLine(Point2F(cx + 3, cy), Point2F(cx - 3, cy + 5), br, 1.5f);
                } else {
                    rt->DrawLine(Point2F(cx + 3, cy - 5), Point2F(cx - 3, cy), br, 1.5f);
                    rt->DrawLine(Point2F(cx - 3, cy), Point2F(cx + 3, cy + 5), br, 1.5f);
                }
            };
            chevron(L.wallPrev, false, HitKind::Tile1);
            chevron(L.wallNext, true, HitKind::Tile2);
        }

        const bool paused = m_host->userPaused();
        btnBg(L.play, m_hover[(int)HitKind::Play], false);
        {
            auto* br = solid.get(rt, rgba4(1, 1, 1, 0.92f));
            const float cx = L.play.x + L.play.w * 0.5f, cy = L.play.y + L.play.h * 0.5f;
            if (paused) {
                rt->DrawLine(Point2F(cx - 4, cy - 7), Point2F(cx - 4, cy + 7), br, 0.1f);
                ComPtr<ID2D1PathGeometry> geo;
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(d2d->CreatePathGeometry(geo.put())) && SUCCEEDED(geo->Open(sink.put()))) {
                    sink->BeginFigure(Point2F(cx - 5, cy - 8), D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(Point2F(cx + 8, cy));
                    sink->AddLine(Point2F(cx - 5, cy + 8));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    rt->FillGeometry(geo.get(), br);
                }
            } else {
                rt->FillRectangle(RectF(cx - 6, cy - 7, cx - 2, cy + 7), br);
                rt->FillRectangle(RectF(cx + 2, cy - 7, cx + 6, cy + 7), br);
            }
        }

        wchar_t fpsBuf[32];
        wsprintfW(fpsBuf, L"%d FPS", settings.fps);
        btnBg(L.fps, m_hover[(int)HitKind::Fps], false);
        drawText(rt, tfSmall.get(), RectF(L.fps.x, L.fps.y, L.fps.x + L.fps.w, L.fps.y + L.fps.h), fpsBuf,
                 solid.get(rt, rgba4(1, 1, 1, 0.82f)), DWRITE_TEXT_ALIGNMENT_CENTER);

        {
            const bool hidden = settings.hideDesktopIcons;
            btnBg(L.icons, m_hover[(int)HitKind::Icons], hidden);
            const float cx = L.icons.x + L.icons.w * 0.5f, cy = L.icons.y + L.icons.h * 0.5f;
            auto* br = solid.get(rt, rgba4(1, 1, 1, hidden ? 0.95f : 0.82f));
            const float s = 4.2f;
            const float gap = 3.2f;
            for (int r = 0; r < 2; ++r) {
                for (int c = 0; c < 2; ++c) {
                    const float x = cx - gap - s + c * (s * 2.0f + gap);
                    const float y = cy - gap - s + r * (s * 2.0f + gap);
                    fillRound(rt, br, x, y, s * 2.0f, s * 2.0f, 1.6f);
                }
            }
            if (hidden) {
                auto* slash = solid.get(rt, rgba4(1, 0.35f, 0.35f, 0.95f));
                rt->DrawLine(Point2F(cx - 11.0f, cy + 10.0f), Point2F(cx + 11.0f, cy - 10.0f), slash, 2.2f);
            }
        }

        btnBg(L.gear, m_hover[(int)HitKind::Gear], m_drawerTarget > 0.5f);
        {
            const float cx = L.gear.x + L.gear.w * 0.5f, cy = L.gear.y + L.gear.h * 0.5f;
            auto* br = solid.get(rt, rgba4(1, 1, 1, 0.85f));
            rt->DrawEllipse(D2D1::Ellipse(Point2F(cx, cy), 5.2f, 5.2f), br, 1.4f);
            for (int i = 0; i < 6; ++i) {
                const float a = i * 3.14159265f / 3.0f;
                rt->DrawLine(Point2F(cx + std::cos(a) * 7.2f, cy + std::sin(a) * 7.2f),
                             Point2F(cx + std::cos(a) * 10.0f, cy + std::sin(a) * 10.0f), br, 1.6f);
            }
        }

        // System health — three rings + sparkline + footer.
        {
            fillRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.038f)), L.sys.x, L.sys.y, L.sys.w, L.sys.h, 14.0f);
            drawText(rt, tfTiny.get(),
                     RectF(L.sys.x + 8, L.sys.y + 2, L.sys.x + L.sys.w * 0.5f, L.sys.y + 16), L"SYSTEM",
                     solid.get(rt, rgba4(1, 1, 1, 0.42f)), DWRITE_TEXT_ALIGNMENT_LEADING);
            drawText(rt, tfTiny.get(),
                     RectF(L.sys.x + L.sys.w * 0.5f, L.sys.y + 2, L.sys.x + L.sys.w - 8, L.sys.y + 16),
                     L"60s", solid.get(rt, rgba4(1, 1, 1, 0.28f)), DWRITE_TEXT_ALIGNMENT_TRAILING);

            struct G {
                const wchar_t* name;
                float pct;
                bool valid;
                wchar_t sub[40];
            } g[3];
            g[0].name = L"CPU";
            g[0].pct = m_dCpu;
            g[0].valid = m_sysSnap.cpuValid;
            if (m_sysSnap.cpuValid && m_sysSnap.cpuMhz > 1.0f)
                swprintf(g[0].sub, 40, L"%.2f GHz", m_sysSnap.cpuMhz / 1000.0f);
            else
                g[0].sub[0] = 0;
            g[1].name = L"RAM";
            g[1].pct = m_dRam;
            g[1].valid = m_sysSnap.ramValid && m_sysSnap.ramTotalGB > 0.05f;
            if (g[1].valid)
                swprintf(g[1].sub, 40, L"%.1f/%.0f GB", m_sysSnap.ramUsedGB, m_sysSnap.ramTotalGB);
            else
                g[1].sub[0] = 0;
            g[2].name = L"GPU";
            g[2].valid = m_sysSnap.gpuValid;
            g[2].pct = m_sysSnap.gpuValid ? m_dGpu : 0;
            if (m_sysSnap.vramValid)
                swprintf(g[2].sub, 40, L"%.1f/%.0f GB", m_sysSnap.vramUsedGB, m_sysSnap.vramTotalGB);
            else if (m_sysSnap.gpuTempValid)
                swprintf(g[2].sub, 40, L"%.0f°", m_sysSnap.gpuTempC);
            else
                g[2].sub[0] = 0;

            for (int i = 0; i < 3; ++i) {
                const DipRect& r = L.gauges[i];
                const float cx = r.x + r.w * 0.5f, cy = r.y + 26.0f;
                const uint32_t hc = g[i].valid ? healthRgb(g[i].pct, accent) : 0x8A93A3;
                rt->DrawEllipse(D2D1::Ellipse(Point2F(cx, cy), 22.0f, 22.0f),
                                solid.get(rt, rgba4(1, 1, 1, 0.10f)), 3.0f);
                if (g[i].valid)
                    drawArc(rt, solid.get(rt, rgba(hc, 0.95f)), cx, cy, 22.0f, g[i].pct / 100.0f, 3.2f);
                wchar_t pct[16];
                if (!g[i].valid)
                    wcscpy(pct, L"-");
                else if (g[i].pct >= 99.5f)
                    wcscpy(pct, L"100");
                else
                    swprintf(pct, 16, L"%.0f%%", g[i].pct);
                drawText(rt, tfSmall.get(), RectF(cx - 18, cy - 11, cx + 18, cy + 10), pct,
                         solid.get(rt, rgba(hc, 0.95f)), DWRITE_TEXT_ALIGNMENT_CENTER);
                drawText(rt, tfTiny.get(), RectF(r.x, r.y + 50, r.x + r.w, r.y + 62), g[i].name,
                         solid.get(rt, rgba4(1, 1, 1, 0.52f)), DWRITE_TEXT_ALIGNMENT_CENTER);
                if (g[i].sub[0]) {
                    drawText(rt, tfTiny.get(), RectF(r.x - 6, r.y + 61, r.x + r.w + 6, r.y + 73), g[i].sub,
                             solid.get(rt, rgba(hc, 0.72f)), DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }

            if (m_sysSnap.historyCount > 1) {
                const float x0 = L.spark.x, y0 = L.spark.y + 1, w = L.spark.w, h = L.spark.h - 2;
                rt->DrawLine(Point2F(x0, y0 + h), Point2F(x0 + w, y0 + h),
                             solid.get(rt, rgba4(1, 1, 1, 0.08f)), 1.0f);
                D2D1_POINT_2F prev{};
                const int n = m_sysSnap.historyCount;
                const uint32_t sc = healthRgb(m_dCpu, accent);
                for (int i = 0; i < n; ++i) {
                    const int idx = (m_sysSnap.historyHead + 60 - n + i) % 60;
                    const float t = n > 1 ? (float)i / (float)(n - 1) : 0;
                    const float v = clampf(m_sysSnap.cpuHistory[idx] / 100.0f, 0, 1);
                    D2D1_POINT_2F p = Point2F(x0 + t * w, y0 + h - v * h);
                    if (i) rt->DrawLine(prev, p, solid.get(rt, rgba(sc, 0.62f)), 1.35f);
                    prev = p;
                }
            }

            wchar_t line1[96], line2[96], up[24], dn[24], upn[24], diskPart[40];
            if (m_sysSnap.netValid) {
                fmtRate(dn, 24, m_sysSnap.netDownBps);
                fmtRate(upn, 24, m_sysSnap.netUpBps);
            } else {
                wcscpy(dn, L"-");
                wcscpy(upn, L"-");
            }
            fmtUptime(up, 24, m_sysSnap.uptimeMs);
            if (!m_sysSnap.diskIoValid)
                wcscpy(diskPart, L"-");
            else if (m_sysSnap.diskReadMBs + m_sysSnap.diskWriteMBs < 0.05f)
                wcscpy(diskPart, L"idle");
            else
                swprintf(diskPart, 40, L"%.1f↓  %.1f↑ MB/s", m_sysSnap.diskReadMBs, m_sysSnap.diskWriteMBs);
            if (m_sysSnap.diskValid && m_sysSnap.diskRoot[0])
                swprintf(line1, 96, L"%c:  %.0f GB free     ·     Disk  %s", m_sysSnap.diskRoot[0],
                         m_sysSnap.diskFreeGB, diskPart);
            else
                swprintf(line1, 96, L"Disk  %s", diskPart);
            if (m_sysSnap.gpuTempValid)
                swprintf(line2, 96, L"Net  %s↓  %s↑     ·     Up %s     ·     us %.1f%%  %.0f MB     GPU %.0f°",
                         dn, upn, up, m_sysSnap.selfCpuPct, m_sysSnap.selfRamMB, m_sysSnap.gpuTempC);
            else
                swprintf(line2, 96, L"Net  %s↓  %s↑     ·     Up %s     ·     us %.1f%%  %.0f MB", dn, upn, up,
                         m_sysSnap.selfCpuPct, m_sysSnap.selfRamMB);
            drawText(rt, tfTiny.get(),
                     RectF(L.sysFoot.x + 8, L.sysFoot.y, L.sysFoot.x + L.sysFoot.w - 8, L.sysFoot.y + 15),
                     line1, solid.get(rt, rgba4(1, 1, 1, 0.58f)));
            drawText(rt, tfTiny.get(),
                     RectF(L.sysFoot.x + 8, L.sysFoot.y + 15, L.sysFoot.x + L.sysFoot.w - 8,
                           L.sysFoot.y + 30),
                     line2, solid.get(rt, rgba4(1, 1, 1, 0.58f)));
        }

        if (m_drawerT > 0.02f) {
            rt->PushAxisAlignedClip(RectF(L.drawer.x, L.drawer.y, L.drawer.x + L.drawer.w,
                                          L.drawer.y + L.drawer.h),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            fillRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.04f)), L.drawer.x, L.drawer.y, L.drawer.w,
                      std::max(L.drawer.h, 4.0f), 12.0f);

            const float gHot = m_hover[(int)HitKind::Gallery];
            fillRound(rt, solid.get(rt, rgba(accent, 0.16f + gHot * 0.12f)), L.galleryBtn.x, L.galleryBtn.y,
                      L.galleryBtn.w, L.galleryBtn.h, 8.0f);
            drawText(rt, tfRow.get(),
                     RectF(L.galleryBtn.x, L.galleryBtn.y, L.galleryBtn.x + L.galleryBtn.w,
                           L.galleryBtn.y + L.galleryBtn.h),
                     L"All settings", solid.get(rt, rgba4(1, 1, 1, 0.92f)), DWRITE_TEXT_ALIGNMENT_CENTER);
            for (const auto& c : L.chips) {
                const bool reset = c.index < 0;
                fillRound(rt, solid.get(rt, reset ? rgba4(1, 1, 1, 0.05f) : rgba(accent, 0.22f)), c.box.x, c.box.y,
                          c.box.w, c.box.h, 8.0f);
                if (reset)
                    drawRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.38f)), c.box.x, c.box.y, c.box.w, c.box.h, 8.0f, 1.0f);
                drawText(rt, tfRow.get(), RectF(c.box.x, c.box.y, c.box.x + c.box.w, c.box.y + c.box.h),
                         c.label.c_str(), solid.get(rt, rgba4(1, 1, 1, reset ? 0.72f : 0.92f)),
                         DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            for (const auto& hd : L.heads) {
                drawText(rt, tfRow.get(), RectF(hd.box.x, hd.box.y, hd.box.x + hd.box.w, hd.box.y + hd.box.h),
                         hd.label, solid.get(rt, rgba(accent, 0.9f)));
            }
            for (const auto& r : L.rows) {
                if (r.kind == (int)KnobKind::Slider) {
                    drawText(rt, tfRow.get(),
                             RectF(r.labelBox.x, r.labelBox.y, r.labelBox.x + r.labelBox.w, r.labelBox.y + r.labelBox.h),
                             r.label, solid.get(rt, rgba4(1, 1, 1, 0.72f)));
                    drawText(rt, tfRow.get(),
                             RectF(r.valueBox.x, r.valueBox.y, r.valueBox.x + r.valueBox.w, r.valueBox.y + r.valueBox.h),
                             r.value.c_str(), solid.get(rt, rgba4(1, 1, 1, 0.84f)), DWRITE_TEXT_ALIGNMENT_TRAILING);
                    const float trackY = r.track.y + r.track.h * 0.5f;
                    fillRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.10f)), r.track.x, trackY - 2, r.track.w, 4, 2);
                    fillRound(rt, solid.get(rt, rgba(accent, 0.85f)), r.track.x, trackY - 2,
                              std::max(4.0f, r.track.w * clampf(r.t, 0, 1)), 4, 2);
                    const float kx = r.track.x + r.track.w * clampf(r.t, 0, 1);
                    rt->FillEllipse(D2D1::Ellipse(Point2F(kx, trackY), 6.0f, 6.0f),
                                    solid.get(rt, rgba4(0.95f, 0.96f, 0.98f, 0.95f)));
                } else if (r.kind == (int)KnobKind::Toggle) {
                    fillRound(rt, solid.get(rt, rgba(accent, r.on ? 0.28f : 0.08f)), r.box.x, r.box.y, r.box.w,
                              r.box.h, 8.0f);
                    drawText(rt, tfRow.get(),
                             RectF(r.box.x + ui::kGap, r.box.y, r.box.x + r.box.w - ui::kGap, r.box.y + r.box.h), r.label,
                             solid.get(rt, rgba4(1, 1, 1, r.on ? 0.92f : 0.62f)));
                } else if (r.kind == (int)KnobKind::Swatch) {
                    drawText(rt, tfRow.get(),
                             RectF(r.labelBox.x, r.labelBox.y, r.labelBox.x + r.labelBox.w, r.labelBox.y + r.labelBox.h),
                             r.label, solid.get(rt, rgba4(1, 1, 1, 0.78f)));
                    fillRound(rt, solid.get(rt, rgba(r.rgb, 1.0f)), r.track.x, r.track.y, r.track.w, r.track.h, 4.0f);
                }
            }
            if (L.scrollBar.h > 1.0f) {
                fillRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.28f)), L.scrollBar.x, L.scrollBar.y, L.scrollBar.w,
                          L.scrollBar.h, 2.0f);
            }
            rt->PopAxisAlignedClip();
        }
    } else {
        wchar_t cpuBuf[16];
        swprintf(cpuBuf, 16, L"%.0f%%", m_dCpu);
        fillRound(rt, solid.get(rt, rgba4(1, 1, 1, 0.06f)), L.miniCpu.x, L.miniCpu.y, L.miniCpu.w, L.miniCpu.h,
                  10.0f);
        drawText(rt, tfSmall.get(),
                 RectF(L.miniCpu.x, L.miniCpu.y, L.miniCpu.x + L.miniCpu.w, L.miniCpu.y + L.miniCpu.h), cpuBuf,
                 solid.get(rt, rgba(healthRgb(m_dCpu, accent), 0.95f)), DWRITE_TEXT_ALIGNMENT_CENTER);
        const bool paused = m_host->userPaused();
        btnBg(L.play, m_hover[(int)HitKind::Play], false);
        auto* br = solid.get(rt, rgba4(1, 1, 1, 0.92f));
        const float cx = L.play.x + L.play.w * 0.5f, cy = L.play.y + L.play.h * 0.5f;
        if (paused) {
            ComPtr<ID2D1PathGeometry> geo;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(d2d->CreatePathGeometry(geo.put())) && SUCCEEDED(geo->Open(sink.put()))) {
                sink->BeginFigure(Point2F(cx - 4, cy - 6), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(Point2F(cx + 6, cy));
                sink->AddLine(Point2F(cx - 4, cy + 6));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                rt->FillGeometry(geo.get(), br);
            }
        } else {
            rt->FillRectangle(RectF(cx - 5, cy - 6, cx - 2, cy + 6), br);
            rt->FillRectangle(RectF(cx + 2, cy - 6, cx + 5, cy + 6), br);
        }
    }

    hr = rt->EndDraw();
    if (FAILED(hr)) {
        if (hr == (HRESULT)D2DERR_RECREATE_TARGET) releaseHwndTarget();
        return;
    }
    if (m_useDwm) return;

    const UINT stride = (UINT)m_pixelW * 4;
    std::vector<unsigned char> pixels((size_t)stride * (size_t)m_pixelH);
    WICRect rc{0, 0, m_pixelW, m_pixelH};
    if (FAILED(bmp->CopyPixels(&rc, stride, (UINT)pixels.size(), pixels.data()))) return;
    presentLayer(pixels.data(), m_pixelW, m_pixelH);
}

LRESULT CALLBACK DesktopWidget::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* self = reinterpret_cast<DesktopWidget*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        // Must DefWindowProc the real HWND here. m_hwnd is still null until
        // CreateWindowEx returns, and DefWindowProc(NULL) is error 1400.
        if (self) self->m_hwnd = hwnd;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* self = reinterpret_cast<DesktopWidget*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->handle(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT DesktopWidget::handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_WINDOWPOSCHANGING: {
            auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
            if (!wp || m_pinningZ || m_draggingWin) break;
            if (m_visible && (wp->flags & SWP_HIDEWINDOW)) {
                wp->flags &= ~SWP_HIDEWINDOW;
                wp->flags |= SWP_SHOWWINDOW;
            }
            if (m_desktopLayer && !(wp->flags & SWP_NOZORDER)) {
                HWND desk = FindWindowW(L"Progman", nullptr);
                if (!desk) desk = GetShellWindow();
                HWND insertAfter = HWND_BOTTOM;
                if (desk) {
                    HWND prev = GetWindow(desk, GW_HWNDPREV);
                    if (prev && prev != hwnd) insertAfter = prev;
                    else if (prev == hwnd) {
                        wp->flags |= SWP_NOZORDER;
                        break;
                    }
                }
                wp->hwndInsertAfter = insertAfter;
            }
            break;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            if (m_useDwm) paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const float x = GET_X_LPARAM(lParam) / m_scale;
            const float y = GET_Y_LPARAM(lParam) / m_scale;
            if (!m_trackingLeave) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = m_hwnd;
                TrackMouseEvent(&tme);
                m_trackingLeave = true;
            }
            if (m_dragScroll) {
                const Layout L = buildLayout();
                const float maxS = std::max(0.0f, L.contentH - kDrawerMax);
                const float trackH = L.drawer.h - ui::kGap * 2.0f;
                const float travel = std::max(1.0f, trackH - L.scrollBar.h);
                const float t = clampf((y - m_scrollGrab - (L.drawer.y + ui::kGap)) / travel, 0.0f, 1.0f);
                m_drawerScroll = t * maxS;
                paint();
                return 0;
            }
            if (m_draggingWin) {
                POINT cur{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ClientToScreen(m_hwnd, &cur);
                const int nx = m_dragOrigX + (cur.x - m_dragScreen.x);
                const int ny = m_dragOrigY + (cur.y - m_dragScreen.y);
                SetWindowPos(m_hwnd, nullptr, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            if (m_dragSlider >= 0) {
                onDragSlider(x);
                paint();
                return 0;
            }
            const Hit h = hitTest(x, y);
            if (h != m_hot) {
                m_hot = h;
                startAnim();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            m_trackingLeave = false;
            m_hot = {};
            startAnim();
            return 0;
        case WM_LBUTTONDOWN: {
            const float x = GET_X_LPARAM(lParam) / m_scale;
            const float y = GET_Y_LPARAM(lParam) / m_scale;
            const Hit h = hitTest(x, y);
            m_pressed = h;
            if (h.kind == HitKind::Scroll) {
                const Layout L = buildLayout();
                m_dragScroll = true;
                m_scrollGrab = y - L.scrollBar.y;
                SetCapture(m_hwnd);
                return 0;
            }
            if (h.kind == HitKind::Drag) {
                m_draggingWin = true;
                RECT wr{};
                GetWindowRect(m_hwnd, &wr);
                m_dragOrigX = wr.left;
                m_dragOrigY = wr.top;
                m_dragScreen.x = GET_X_LPARAM(lParam);
                m_dragScreen.y = GET_Y_LPARAM(lParam);
                ClientToScreen(m_hwnd, &m_dragScreen);
                SetCapture(m_hwnd);
            } else {
                onClick(h, x, y);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (m_dragScroll) {
                m_dragScroll = false;
                m_pressed = {};
                ReleaseCapture();
                return 0;
            }
            if (m_draggingWin) {
                m_draggingWin = false;
                ReleaseCapture();
                saveChrome();
            }
            if (m_dragSlider >= 0) {
                m_dragSlider = -1;
                ReleaseCapture();
                if (m_host) m_host->flushSettings();
            }
            m_pressed = {};
            return 0;
        case WM_CAPTURECHANGED:
            if (m_dragSlider >= 0) {
                m_dragSlider = -1;
                if (m_host) m_host->flushSettings();
            }
            m_draggingWin = false;
            m_dragScroll = false;
            m_pressed = {};
            return 0;
        case WM_MOUSEWHEEL: {
            m_draggingWin = false;
            const float x = GET_X_LPARAM(lParam);
            const float y = GET_Y_LPARAM(lParam);
            POINT pt{ (int)x, (int)y };
            ScreenToClient(m_hwnd, &pt);
            const float dx = pt.x / m_scale;
            const float dy = pt.y / m_scale;
            const Layout L = buildLayout();
            if (L.drawer.contains(dx, dy) || (L.scrollBar.h > 1.0f && L.scrollBar.contains(dx, dy))) {
                const float step = ((short)HIWORD(wParam) > 0) ? -28.0f : 28.0f;
                const float maxS = std::max(0.0f, L.contentH - std::min(L.contentH, kDrawerMax));
                m_drawerScroll = clampf(m_drawerScroll + step, 0.0f, maxS);
                paint();
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const float x = GET_X_LPARAM(lParam) / m_scale;
            const float y = GET_Y_LPARAM(lParam) / m_scale;
            const Hit h = hitTest(x, y);
            if (h.kind == HitKind::Drag || h.kind == HitKind::MiniToggle || h.kind == HitKind::None)
                setMini(m_miniTarget < 0.5f);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kTimerAnim) tickAnim();
            else if (wParam == kTimerZ) pinDesktopZ();
            return 0;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            updateDpi();
            applyWindowIcons(m_hwnd, m_instance);
            paint();
            return 0;
        }
        case WM_DISPLAYCHANGE:
            updateDpi();
            applyWindowIcons(m_hwnd, m_instance);
            positionWindow(true);
            paint();
            return 0;
        case WM_DESTROY:
            releaseWindowIcons(hwnd);
            releaseHwndTarget();
            m_hwnd = nullptr;
            m_visible = false;
            return 0;
        default:
            if (msg == kSampleMsg) {
                onSysSample();
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool DesktopWidget::runSettingsSelfTest(std::string& report) {
    if (!m_hwnd || !m_host) {
        report = "WidgetSettings: FAIL (no widget)\n";
        return false;
    }
    const AppSettings saved = m_host->settings();
    const float savedDrawerT = m_drawerT, savedTarget = m_drawerTarget;
    const float savedMiniT = m_miniT, savedMiniTarget = m_miniTarget, savedScroll = m_drawerScroll;
    struct Restore {
        DesktopWidget* w;
        SettingsPanel::Host* host;
        AppSettings saved;
        float drawerT, drawerTarget, miniT, miniTarget, scroll;
        ~Restore() {
            w->m_drawerT = drawerT;
            w->m_drawerTarget = drawerTarget;
            w->m_miniT = miniT;
            w->m_miniTarget = miniTarget;
            w->m_drawerScroll = scroll;
            w->m_dragSlider = -1;
            if (host) {
                host->settings() = saved;
                host->switchEffect(saved.effect);
            }
            if (w->m_hwnd) w->positionWindow(true);
        }
    } restore{this, m_host, saved, savedDrawerT, savedTarget, savedMiniT, savedMiniTarget, savedScroll};

    m_miniT = m_miniTarget = 0;
    m_drawerT = m_drawerTarget = 1;
    m_drawerScroll = 0;
    updateDpi();
    if (m_scale < 0.5f) m_scale = 1.0f;
    positionWindow(true);

    auto fail = [&](const std::string& why) {
        report = "WidgetSettings: FAIL " + why + "\n";
        return false;
    };
    auto sendAt = [&](UINT msg, float dipX, float dipY, WPARAM wp) {
        const int px = (int)std::lround(dipX * m_scale);
        const int py = (int)std::lround(dipY * m_scale);
        SendMessageW(m_hwnd, msg, wp, MAKELPARAM(px, py));
    };
    auto bringInto = [&](float contentY) {
        const Layout probe = buildLayout();
        if (contentY < probe.drawer.y + 4.0f || contentY > probe.drawer.y + probe.drawer.h - 8.0f)
            m_drawerScroll = std::max(0.0f, m_drawerScroll + (contentY - (probe.drawer.y + 36.0f)));
    };

    auto auditDrawer = [&](const Layout& L) -> const char* {
        const float contentX = L.drawer.x + ui::kPad;
        const float contentW = ui::contentWidth(L.drawer.w);
        ui::R content{contentX, L.drawer.y + ui::kPad, contentW, std::max(0.0f, L.contentH - ui::kPad - ui::kBottom)};
        ui::R gutter{L.drawer.x + L.drawer.w - ui::kGutter, L.drawer.y, ui::kGutter, std::max(L.drawer.h, L.contentH)};
        std::vector<ui::R> items;
        auto add = [&](const DipRect& r) { items.push_back({r.x, r.y + m_drawerScroll, r.w, r.h}); };
        add(L.galleryBtn);
        for (const auto& c : L.chips) add(c.box);
        for (const auto& h : L.heads) add(h.box);
        for (const auto& r : L.rows) add(r.box);
        return ui::audit(items.data(), (int)items.size(), content, gutter);
    };

    const auto list = m_host->registry().list();
    int ok = 0;
    for (const auto& e : list) {
        m_host->switchEffect(e.id);
        m_drawerScroll = 0;
        Layout L = buildLayout();
        if (const char* why = auditDrawer(L)) return fail(e.id + std::string(" layout ") + why);
        for (const auto& c : L.chips) {
            if (c.box.x + c.box.w > L.drawer.x + L.drawer.w - ui::kGutter + 0.5f)
                return fail(e.id + " chip clipped");
        }
        if (L.galleryBtn.w < 40.0f || !L.drawer.contains(L.galleryBtn.x + 8.0f, L.galleryBtn.y + 8.0f))
            return fail(e.id + " All settings is not in the drawer");
        const Layout::Row* speed = nullptr;
        for (const auto& r : L.rows)
            if (r.kind == (int)KnobKind::Slider && r.index == 0) speed = &r;
        if (!speed) return fail(e.id + " missing speed");
        bringInto(speed->box.y + speed->box.h * 0.5f);
        L = buildLayout();
        speed = nullptr;
        for (const auto& r : L.rows)
            if (r.kind == (int)KnobKind::Slider && r.index == 0) speed = &r;
        if (!speed) return fail(e.id + " speed lost");
        const float y = speed->box.y + speed->box.h * 0.5f;
        const float x1 = speed->track.x + speed->track.w * 0.82f;
        if (!L.drawer.contains(x1, y)) return fail(e.id + " speed not hittable");
        const float before = knobValue(m_host->settings(), e.id, 0);
        sendAt(WM_LBUTTONDOWN, speed->track.x + speed->track.w * 0.12f, y, MK_LBUTTON);
        sendAt(WM_MOUSEMOVE, x1, y, MK_LBUTTON);
        sendAt(WM_LBUTTONUP, x1, y, 0);
        if (std::fabs(knobValue(m_host->settings(), e.id, 0) - before) < 0.02f)
            return fail(e.id + " speed did not change");

        L = buildLayout();
        int togIndex = -1;
        DipRect togBox{};
        for (const auto& r : L.rows)
            if (r.kind == (int)KnobKind::Toggle) {
                togIndex = r.index;
                togBox = r.box;
                break;
            }
        if (togIndex >= 0) {
            bringInto(togBox.y + togBox.h * 0.5f);
            L = buildLayout();
            togIndex = -1;
            for (const auto& r : L.rows)
                if (r.kind == (int)KnobKind::Toggle) {
                    togIndex = r.index;
                    togBox = r.box;
                    break;
                }
            if (togIndex < 0 || !L.drawer.contains(togBox.x + 6.0f, togBox.y + 4.0f))
                return fail(e.id + " toggle not hittable");
            const bool was = knobToggle(m_host->settings(), e.id, togIndex);
            sendAt(WM_LBUTTONDOWN, togBox.x + 10.0f, togBox.y + togBox.h * 0.5f, MK_LBUTTON);
            sendAt(WM_LBUTTONUP, togBox.x + 10.0f, togBox.y + togBox.h * 0.5f, 0);
            if (knobToggle(m_host->settings(), e.id, togIndex) == was) return fail(e.id + " toggle did not change");
        }

        L = buildLayout();
        int swIndex = -1;
        DipRect swBox{};
        for (const auto& r : L.rows)
            if (r.kind == (int)KnobKind::Swatch) {
                swIndex = r.index;
                swBox = r.track;
                break;
            }
        if (swIndex >= 0) {
            bringInto(swBox.y + swBox.h * 0.5f);
            L = buildLayout();
            swIndex = -1;
            for (const auto& r : L.rows)
                if (r.kind == (int)KnobKind::Swatch) {
                    swIndex = r.index;
                    swBox = r.track;
                    break;
                }
            if (swIndex < 0 || !L.drawer.contains(swBox.x + 2.0f, swBox.y + 2.0f))
                return fail(e.id + " color not hittable");
            const uint32_t was = knobColor(m_host->settings(), e.id, swIndex);
            sendAt(WM_LBUTTONDOWN, swBox.x + swBox.w * 0.5f, swBox.y + swBox.h * 0.5f, MK_LBUTTON);
            sendAt(WM_LBUTTONUP, swBox.x + swBox.w * 0.5f, swBox.y + swBox.h * 0.5f, 0);
            if (knobColor(m_host->settings(), e.id, swIndex) == was) return fail(e.id + " color did not change");
        }

        if (presetCount(e.id) > 1) {
            m_drawerScroll = 0;
            L = buildLayout();
            const Layout::Chip* chip = nullptr;
            for (const auto& c : L.chips)
                if (c.index == 1) chip = &c;
            if (!chip || !L.drawer.contains(chip->box.x + 4.0f, chip->box.y + 4.0f))
                return fail(e.id + " preset not hittable");
            const float spd = knobValue(m_host->settings(), e.id, 0);
            const uint32_t col = knobColor(m_host->settings(), e.id, 0);
            sendAt(WM_LBUTTONDOWN, chip->box.x + chip->box.w * 0.5f, chip->box.y + chip->box.h * 0.5f, MK_LBUTTON);
            sendAt(WM_LBUTTONUP, chip->box.x + chip->box.w * 0.5f, chip->box.y + chip->box.h * 0.5f, 0);
            if (std::fabs(knobValue(m_host->settings(), e.id, 0) - spd) < 0.02f &&
                knobColor(m_host->settings(), e.id, 0) == col)
                return fail(e.id + " preset did not change settings");
        }
        ++ok;
    }
    if (ok != (int)list.size()) return fail("incomplete");

    m_drawerT = m_drawerTarget = 1;
    m_drawerScroll = 0;
    m_draggingWin = false;
    bool overflow = false;
    for (const auto& e : list) {
        m_host->switchEffect(e.id);
        m_drawerScroll = 0;
        if (buildLayout().contentH > kDrawerMax + 8.0f) {
            overflow = true;
            break;
        }
    }
    if (!overflow) return fail("drawer does not overflow");
    Layout scrolled = buildLayout();
    if (scrolled.scrollBar.h < 8.0f) return fail("drawer scrollbar missing");
    RECT before{};
    GetWindowRect(m_hwnd, &before);
    POINT pt{};
    pt.x = (int)std::lround((scrolled.drawer.x + 24.0f) * m_scale);
    pt.y = (int)std::lround((scrolled.drawer.y + 36.0f) * m_scale);
    ClientToScreen(m_hwnd, &pt);
    SendMessageW(m_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)-WHEEL_DELTA), MAKELPARAM(pt.x, pt.y));
    RECT after{};
    GetWindowRect(m_hwnd, &after);
    if (after.left != before.left || after.top != before.top) return fail("wheel moved the window");
    if (m_draggingWin) return fail("wheel started a drag");
    if (m_drawerScroll < 20.0f) return fail("wheel did not scroll");
    scrolled = buildLayout();
    const Layout::Row* row = nullptr;
    for (const auto& r : scrolled.rows) {
        if (r.kind == (int)KnobKind::Slider &&
            scrolled.drawer.contains(r.track.x + 8.0f, r.box.y + r.box.h * 0.5f)) {
            row = &r;
            break;
        }
    }
    if (!row) return fail("no slider after scroll");
    const std::string id = m_host->settings().effect;
    const float beforeVal = knobValue(m_host->settings(), id, row->index);
    const float clickY = row->box.y + row->box.h * 0.5f;
    sendAt(WM_LBUTTONDOWN, row->track.x + row->track.w * 0.15f, clickY, MK_LBUTTON);
    sendAt(WM_LBUTTONUP, row->track.x + row->track.w * 0.15f, clickY, 0);
    if (m_draggingWin) return fail("slider click dragged the window");
    if (std::fabs(knobValue(m_host->settings(), id, row->index) - beforeVal) < 0.02f)
        return fail("slider miss after scroll");

    report = "WidgetSettings: PASS effects=" + std::to_string(ok) + "\n";
    return true;
}
