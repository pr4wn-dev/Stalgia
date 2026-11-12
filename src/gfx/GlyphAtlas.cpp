#include "gfx/GlyphAtlas.h"

#include "core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <windows.h>

namespace {

struct GdiBitmap {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HBITMAP oldBmp = nullptr;
    void* bits = nullptr;
    int w = 0;
    int h = 0;

    ~GdiBitmap() { reset(); }

    void reset() {
        if (dc) {
            if (oldBmp) SelectObject(dc, oldBmp);
            DeleteDC(dc);
            dc = nullptr;
            oldBmp = nullptr;
        }
        if (bmp) {
            DeleteObject(bmp);
            bmp = nullptr;
        }
        bits = nullptr;
    }

    bool create(int width, int height) {
        reset();
        w = width;
        h = height;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) {
            reset();
            return false;
        }
        oldBmp = (HBITMAP)SelectObject(dc, bmp);
        std::memset(bits, 0, (size_t)width * height * 4);
        return true;
    }
};

bool fontHasKatakana(HDC dc) {
    wchar_t sample = 0xFF71;  // ｱ
    WORD idx = 0;
    if (GetGlyphIndicesW(dc, &sample, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR) {
        return false;
    }
    return idx != 0xFFFF;
}

HFONT makeFont(const wchar_t* face, int height) {
    return CreateFontW(height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       FIXED_PITCH | FF_MODERN, face);
}

}  // namespace

std::wstring GlyphAtlas::buildCharset() {
    // Film rain: halfwidth katakana first, then digits and a few symbols.
    // Latin capitals are deliberately omitted so hashing doesn't fill the
    // screen with A–Z. Mirrored kana are appended by the atlas builder.
    std::wstring s;
    for (wchar_t c = 0xFF66; c <= 0xFF9D; ++c) s.push_back(c);
    s += L"0123456789:*+-=.<>/|";
    return s;
}

HFONT GlyphAtlas::tryCreateFont(int height) {
    static const wchar_t* kFaces[] = {
        L"MS Gothic",
        L"Yu Gothic",
        L"Yu Gothic UI",
        L"Meiryo",
        L"Malgun Gothic",
        L"Microsoft YaHei",
        L"NSimSun",
        L"Consolas",
        L"Courier New",
        L"Segoe UI",
    };

    HDC screen = GetDC(nullptr);
    HFONT bestLatin = nullptr;
    for (const wchar_t* face : kFaces) {
        HFONT f = makeFont(face, height);
        if (!f) continue;
        HGDIOBJ old = SelectObject(screen, f);
        const bool kana = fontHasKatakana(screen);
        SelectObject(screen, old);
        if (kana) {
            if (bestLatin) DeleteObject(bestLatin);
            ReleaseDC(nullptr, screen);
            LOG_INFO("Glyph atlas font: %s (katakana available)", (const char*)"");
            // Log the face as UTF-8 via a narrow conversion in the caller would be nicer,
            // but avoid pulling Util into this loop — OutputDebug already has enough.
            return f;
        }
        if (!bestLatin) bestLatin = f;
        else DeleteObject(f);
    }
    ReleaseDC(nullptr, screen);
    if (bestLatin) {
        LOG_WARN("No CJK font found; atlas will use Latin glyphs only");
        return bestLatin;
    }
    return makeFont(L"Arial", height);
}

bool GlyphAtlas::build(ID3D11Device* device, int cellSize, int glyphSet, const std::wstring& custom) {
    release();
    m_flipFrom = 100000;
    if (glyphSet == 2) {
        m_charset = L"01";
    } else if (glyphSet == 1 || (glyphSet == 3 && custom.empty())) {
        m_charset = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@#$%&*+-=<>";
    } else if (glyphSet == 3) {
        m_charset.clear();
        for (size_t i = 0; i < custom.size();) {
            const wchar_t c = custom[i];
            if (c >= 0xD800 && c <= 0xDFFF) {
                i += (c <= 0xDBFF && i + 1 < custom.size() && custom[i + 1] >= 0xDC00 && custom[i + 1] <= 0xDFFF) ? 2
                                                                                                                : 1;
                continue;
            }
            ++i;
            if (c <= 32) continue;
            bool seen = false;
            for (wchar_t e : m_charset)
                if (e == c) seen = true;
            if (!seen) m_charset.push_back(c);
            if ((int)m_charset.size() >= 48) break;
        }
        if (m_charset.empty()) m_charset = L"01";
    } else {
        m_charset = buildCharset();
        m_flipFrom = (int)m_charset.size();
        std::wstring kana;
        for (wchar_t c = 0xFF66; c <= 0xFF9D; ++c) kana.push_back(c);
        const int nFlip = std::min(24, (int)kana.size());
        for (int i = 0; i < nFlip; ++i) m_charset.push_back(kana[(size_t)((i * 2) % (int)kana.size())]);
    }
    m_cell = std::max(8, cellSize);
    const int count = (int)m_charset.size();
    m_cols = 16;
    m_rows = (count + m_cols - 1) / m_cols;
    m_texW = m_cols * m_cell;
    m_texH = m_rows * m_cell;

    // Draw at 2x and box-filter down so small kana stay smooth instead of GDI bitmaps.
    const int scale = 2;
    GdiBitmap gdi;
    if (!gdi.create(m_texW * scale, m_texH * scale)) {
        LOG_ERR("Failed to create GDI DIB for glyph atlas");
        return false;
    }

    HFONT font = tryCreateFont(std::max(7, m_cell * scale - 2));
    if (!font) {
        LOG_ERR("Failed to create atlas font");
        return false;
    }
    HGDIOBJ oldFont = SelectObject(gdi.dc, font);
    SetBkMode(gdi.dc, TRANSPARENT);
    SetTextColor(gdi.dc, RGB(255, 255, 255));
    SetTextAlign(gdi.dc, TA_TOP | TA_LEFT);
    SetGraphicsMode(gdi.dc, GM_ADVANCED);
    XFORM identity{1, 0, 0, 1, 0, 0};

    for (int i = 0; i < count; ++i) {
        const int cx = (i % m_cols) * m_cell * scale;
        const int cy = (i / m_cols) * m_cell * scale;
        wchar_t ch = m_charset[(size_t)i];
        RECT r{cx, cy, cx + m_cell * scale, cy + m_cell * scale};
        if (i >= m_flipFrom) {
            XFORM xf{-1.0f, 0, 0, 1.0f, (float)(2 * cx + m_cell * scale), 0};
            SetWorldTransform(gdi.dc, &xf);
        } else {
            SetWorldTransform(gdi.dc, &identity);
        }
        DrawTextW(gdi.dc, &ch, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    }
    SetWorldTransform(gdi.dc, &identity);

    SelectObject(gdi.dc, oldFont);
    DeleteObject(font);

    // Convert BGRA to R8 (coverage) — GDI writes BGRA, white glyphs on black.
    std::vector<unsigned char> r8((size_t)m_texW * m_texH);
    const unsigned char* src = static_cast<const unsigned char*>(gdi.bits);
    const int srcW = m_texW * scale;
    for (int y = 0; y < m_texH; ++y) {
        for (int x = 0; x < m_texW; ++x) {
            int cov = 0;
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    const unsigned char* p = src + ((size_t)(y * scale + sy) * srcW + (x * scale + sx)) * 4;
                    cov += std::max(p[0], std::max(p[1], p[2]));
                }
            }
            r8[(size_t)y * m_texW + x] = (unsigned char)(cov / (scale * scale));
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)m_texW;
    td.Height = (UINT)m_texH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = r8.data();
    sd.SysMemPitch = (UINT)m_texW;

    HRESULT hr = device->CreateTexture2D(&td, &sd, m_tex.put());
    if (FAILED(hr)) {
        LOG_ERR("CreateTexture2D atlas failed: 0x%08X", (unsigned)hr);
        return false;
    }
    hr = device->CreateShaderResourceView(m_tex.get(), nullptr, m_srv.put());
    if (FAILED(hr)) {
        LOG_ERR("Create SRV atlas failed: 0x%08X", (unsigned)hr);
        return false;
    }

    LOG_INFO("Glyph atlas %dx%d, %d glyphs, cell %d", m_texW, m_texH, count, m_cell);
    return true;
}

void GlyphAtlas::uvFor(int glyphIndex, float& u0, float& v0, float& u1, float& v1) const {
    if (m_charset.empty() || m_texW == 0 || m_texH == 0) {
        u0 = v0 = 0;
        u1 = v1 = 1;
        return;
    }
    const int i = ((glyphIndex % (int)m_charset.size()) + (int)m_charset.size()) % (int)m_charset.size();
    const int col = i % m_cols;
    const int row = i / m_cols;
    const float pad = 1.0f;
    u0 = (col * m_cell + pad) / (float)m_texW;
    v0 = (row * m_cell + pad) / (float)m_texH;
    u1 = ((col + 1) * m_cell - pad) / (float)m_texW;
    v1 = ((row + 1) * m_cell - pad) / (float)m_texH;
}

void GlyphAtlas::release() {
    m_srv.reset();
    m_tex.reset();
    m_charset.clear();
    m_flipFrom = m_cell = m_cols = m_rows = m_texW = m_texH = 0;
}
