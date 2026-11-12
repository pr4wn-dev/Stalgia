#include "host/SettingsPanel.h"

#include "core/Config.h"
#include "core/Log.h"
#include "core/Util.h"
#include "effect/EffectRegistry.h"
#include "effect/Presets.h"
#include "resource.h"

#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

enum : int {
    IDC_STATUS = 3001,
    IDC_LIST = 3002,
    IDC_SHOW = 3003,
    IDC_HIDE = 3004,
    IDC_PAUSE = 3005,
    IDC_COLOR_A = 3006,
    IDC_COLOR_B = 3007,
    IDC_SL_SPEED = 3008,
    IDC_SL_DENSITY = 3009,
    IDC_SL_GLYPH = 3010,
    IDC_SL_TRAIL = 3011,
    IDC_SL_FPS = 3012,
    IDC_CHK_FS = 3013,
    IDC_CHK_COVERED = 3014,
    IDC_CHK_BATTERY = 3015,
    IDC_CHK_AUTO = 3016,
    IDC_OPENCFG = 3017,
    IDC_EXIT = 3018,
    IDC_SWATCH_A = 3019,
    IDC_SWATCH_B = 3020,
    IDC_SL_EXTRA1 = 3021,
    IDC_SL_EXTRA2 = 3022,
    IDC_CHK_EYE = 3023,
    IDC_CHK_BREATHE = 3024,
    IDC_CHK_COLOR = 3025,
    IDC_CHK_ICONS = 3026,
    IDC_GALLERY = 3027,
};

COLORREF& customColors() {
    static COLORREF c[16] = {
        RGB(0, 255, 65),  RGB(222, 255, 222), RGB(200, 220, 255), RGB(34, 255, 136),
        RGB(102, 68, 255), RGB(255, 80, 80),  RGB(255, 200, 80),  RGB(80, 200, 255),
        RGB(255, 255, 255), RGB(0, 0, 0),     RGB(40, 40, 40),    RGB(180, 180, 180),
        RGB(255, 0, 128), RGB(0, 180, 120),   RGB(120, 80, 255),  RGB(255, 160, 40),
    };
    return c[0];
}

void setTrack(HWND sl, int v) {
    if (sl) SendMessageW(sl, TBM_SETPOS, TRUE, v);
}

int trackPos(HWND sl) {
    return sl ? (int)SendMessageW(sl, TBM_GETPOS, 0, 0) : 0;
}

void setCheck(HWND w, bool on) {
    if (w) SendMessageW(w, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool checked(HWND w) {
    return w && SendMessageW(w, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setText(HWND w, const wchar_t* t) {
    if (w) SetWindowTextW(w, t);
}

bool sliderRange(const std::string& id, int index, float& lo, float& hi) {
    for (int i = 0; i < knobCount(id); ++i) {
        const Knob k = knobAt(id, i);
        if (k.kind == KnobKind::Slider && k.index == index) {
            lo = k.lo;
            hi = k.hi;
            return true;
        }
    }
    return false;
}

void showSlider(HWND sl, float value, float lo, float hi, bool integer) {
    if (!sl) return;
    if (integer) {
        SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELPARAM((int)std::lround(lo), (int)std::lround(hi)));
        setTrack(sl, (int)std::lround(value));
    } else {
        const int a = (int)std::lround(lo * 100.0f);
        const int b = (int)std::lround(hi * 100.0f);
        SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELPARAM(a, b));
        setTrack(sl, (int)std::lround(value * 100.0f));
    }
}

void writeSlider(AppSettings& s, const std::string& id, HWND sl, int index) {
    if (!sl || !IsWindowVisible(sl)) return;
    const int minV = (int)SendMessageW(sl, TBM_GETRANGEMIN, 0, 0);
    const int maxV = (int)SendMessageW(sl, TBM_GETRANGEMAX, 0, 0);
    const float t = maxV > minV ? ((float)trackPos(sl) - (float)minV) / (float)(maxV - minV) : 0.0f;
    setKnob(s, id, index, t);
}

LRESULT CALLBACK childWheelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEWHEEL) return SendMessageW(GetParent(hwnd), msg, wp, lp);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

BOOL CALLBACK hookChildWheel(HWND child, LPARAM) {
    SetWindowSubclass(child, childWheelProc, 1, 0);
    return TRUE;
}

}  // namespace

bool SettingsPanel::create(HINSTANCE instance, Host* host) {
    m_instance = instance;
    m_host = host;
    if (m_hwnd && IsWindow(m_hwnd)) {
        show();
        return true;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = loadAppIcon(instance, largeIconPx(nullptr));
    wc.hIconSm = loadAppIcon(instance, smallIconPx(nullptr));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    m_dpi = 96;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using GetDpiForSystemFn = UINT(WINAPI*)();
        auto fn = (GetDpiForSystemFn)(void*)GetProcAddress(user32, "GetDpiForSystem");
        if (fn) {
            UINT d = fn();
            if (d) m_dpi = (int)d;
        }
    }

    const int cw = dpiPx(520);
    const int ch = dpiPx(860);
    RECT wr{0, 0, cw, ch};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VSCROLL;
    AdjustWindowRectEx(&wr, style, FALSE, WS_EX_APPWINDOW);
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        const int maxH = std::max(240, (int)(work.bottom - work.top) - 16);
        if (wr.bottom - wr.top > maxH) wr.bottom = wr.top + maxH;
    }
    m_hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"Stalgia", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, instance, this);
    if (!m_hwnd) {
        LOG_ERR("CreateWindow StalgiaPanel failed: %lu", GetLastError());
        return false;
    }
    applyWindowIcons(m_hwnd, instance);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        m_font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (!m_font) m_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    createControls();
    layout();
    syncToUi();
    return true;
}

void SettingsPanel::destroy() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_font) {
        DeleteObject(m_font);
        m_font = nullptr;
    }
}

void SettingsPanel::show() {
    if (!m_hwnd) return;
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);
    refresh();
}

void SettingsPanel::refresh() {
    if (m_hwnd && IsWindow(m_hwnd)) syncToUi();
}

bool SettingsPanel::visible() const {
    return m_hwnd && IsWindowVisible(m_hwnd);
}

int SettingsPanel::dpiPx(int px) const {
    return MulDiv(px, m_dpi, 96);
}

HWND SettingsPanel::addStatic(const wchar_t* text, int id, DWORD extra) {
    HWND w = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | extra, 0, 0, 10, 10, m_hwnd,
                             (HMENU)(INT_PTR)id, m_instance, nullptr);
    if (m_font) SendMessageW(w, WM_SETFONT, (WPARAM)m_font, TRUE);
    return w;
}

HWND SettingsPanel::addBtn(const wchar_t* text, int id) {
    HWND w = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, m_hwnd,
                             (HMENU)(INT_PTR)id, m_instance, nullptr);
    if (m_font) SendMessageW(w, WM_SETFONT, (WPARAM)m_font, TRUE);
    return w;
}

HWND SettingsPanel::addCheck(const wchar_t* text, int id) {
    HWND w = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0,
                             10, 10, m_hwnd, (HMENU)(INT_PTR)id, m_instance, nullptr);
    if (m_font) SendMessageW(w, WM_SETFONT, (WPARAM)m_font, TRUE);
    return w;
}

HWND SettingsPanel::addTrack(int id, int minV, int maxV) {
    HWND w = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_NOTICKS, 0,
                             0, 10, 10, m_hwnd, (HMENU)(INT_PTR)id, m_instance, nullptr);
    SendMessageW(w, TBM_SETRANGE, TRUE, MAKELPARAM(minV, maxV));
    SendMessageW(w, TBM_SETPAGESIZE, 0, 1);
    return w;
}

void SettingsPanel::createControls() {
    m_lblTitle = addStatic(L"Stalgia", 0);
    m_status = addStatic(L"", IDC_STATUS);
    m_lblWallpaper = addStatic(L"Wallpaper", 0);
    m_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 0, 0, 10, 10,
                             m_hwnd, (HMENU)(INT_PTR)IDC_LIST, m_instance, nullptr);
    if (m_font) SendMessageW(m_list, WM_SETFONT, (WPARAM)m_font, TRUE);

    m_show = addBtn(L"Show wallpaper", IDC_SHOW);
    m_hide = addBtn(L"Hide wallpaper", IDC_HIDE);
    m_pause = addBtn(L"Pause", IDC_PAUSE);

    m_grpFx = addStatic(L"Effect settings", 0);
    m_lblColorA = addStatic(L"Color", 0);
    m_btnColorA = addBtn(L"Choose…", IDC_COLOR_A);
    m_swatchA = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, m_hwnd,
                                (HMENU)(INT_PTR)IDC_SWATCH_A, m_instance, nullptr);
    m_lblColorB = addStatic(L"Head / second color", 0);
    m_btnColorB = addBtn(L"Choose…", IDC_COLOR_B);
    m_swatchB = CreateWindowExW(WS_EX_STATICEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, m_hwnd,
                                (HMENU)(INT_PTR)IDC_SWATCH_B, m_instance, nullptr);

    m_lblSpeed = addStatic(L"Speed", 0);
    m_slSpeed = addTrack(IDC_SL_SPEED, 10, 300);
    m_valSpeed = addStatic(L"1.00", 0, SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX);
    m_lblDensity = addStatic(L"Density", 0);
    m_slDensity = addTrack(IDC_SL_DENSITY, 10, 200);
    m_valDensity = addStatic(L"1.00", 0, SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX);
    m_lblGlyph = addStatic(L"Glyph size", 0);
    m_slGlyph = addTrack(IDC_SL_GLYPH, 7, 40);
    m_valGlyph = addStatic(L"18", 0, SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX);
    m_lblTrail = addStatic(L"Trail length", 0);
    m_slTrail = addTrack(IDC_SL_TRAIL, 6, 60);
    m_valTrail = addStatic(L"22", 0, SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX);
    m_lblExtra1 = addStatic(L"Ape size", 0);
    m_slExtra1 = addTrack(IDC_SL_EXTRA1, 50, 95);
    m_valExtra1 = addStatic(L"0.68", 0, SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX);
    m_lblExtra2 = addStatic(L"Ape position", 0);
    m_slExtra2 = addTrack(IDC_SL_EXTRA2, 25, 75);
    m_valExtra2 = addStatic(L"0.50", 0, SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX);
    m_chkEye = addCheck(L"Eye glow", IDC_CHK_EYE);
    m_chkBreathe = addCheck(L"Breathing", IDC_CHK_BREATHE);
    m_chkColor = addCheck(L"Color", IDC_CHK_COLOR);

    m_lblPlayback = addStatic(L"Playback", 0);
    m_lblFps = addStatic(L"FPS", 0);
    m_slFps = addTrack(IDC_SL_FPS, 10, 60);
    m_valFps = addStatic(L"30", 0, SS_RIGHT | SS_CENTERIMAGE | SS_NOPREFIX);
    m_chkFs = addCheck(L"Pause on fullscreen", IDC_CHK_FS);
    m_chkCovered = addCheck(L"Pause when the desktop is covered", IDC_CHK_COVERED);
    m_chkBattery = addCheck(L"Pause on battery saver", IDC_CHK_BATTERY);
    m_chkAuto = addCheck(L"Start with Windows", IDC_CHK_AUTO);
    m_chkIcons = addCheck(L"Hide desktop icons", IDC_CHK_ICONS);

    m_btnGallery = addBtn(L"All settings", IDC_GALLERY);
    m_btnOpenCfg = addBtn(L"Open config file", IDC_OPENCFG);
    m_btnExit = addBtn(L"Exit", IDC_EXIT);
    m_note = addStatic(
        L"Closing this window leaves the wallpaper running in the tray.\n"
        L"Hide wallpaper tears the surface down so Win+D / Show Desktop behaves normally.",
        0, SS_NOPREFIX);
    EnumChildWindows(m_hwnd, hookChildWheel, 0);
}

bool SettingsPanel::controlShown(HWND w) const {
    return w && (GetWindowLongW(w, GWL_STYLE) & WS_VISIBLE);
}

int SettingsPanel::contentHeightPx() const { return std::max(1, m_contentPx); }

void SettingsPanel::updateScrollBar() {
    if (!m_hwnd) return;
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const int view = std::max(1, (int)rc.bottom);
    const int content = contentHeightPx();
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, content - 1);
    si.nPage = (UINT)view;
    si.nPos = m_scroll;
    SetScrollInfo(m_hwnd, SB_VERT, &si, TRUE);
    ShowScrollBar(m_hwnd, SB_VERT, content > view);
}

void SettingsPanel::layout() {
    if (!m_hwnd || m_inLayout) return;
    m_inLayout = true;

    auto textPx = [&](HWND w) {
        wchar_t buf[160]{};
        if (w) GetWindowTextW(w, buf, 160);
        if (!buf[0]) return dpiPx(72);
        HDC dc = GetDC(m_hwnd);
        HGDIOBJ old = m_font ? SelectObject(dc, m_font) : nullptr;
        SIZE sz{};
        GetTextExtentPoint32W(dc, buf, (int)wcslen(buf), &sz);
        if (old) SelectObject(dc, old);
        ReleaseDC(m_hwnd, dc);
        return (int)std::max(1L, sz.cx);
    };

    auto flow = [&](int clientW) {
        const int pad = dpiPx((int)ui::kPad);
        const int gap = dpiPx((int)ui::kGap);
        const int section = dpiPx((int)ui::kSectionGap);
        const int headerH = dpiPx((int)ui::kHeaderH);
        const int rowH = dpiPx((int)ui::kRowH);
        const int btnH = dpiPx((int)ui::kBtnH);
        const int inner = std::max(dpiPx(220), clientW - pad * 2);
        int labelW = dpiPx((int)ui::kMinLabel);
        HWND labels[] = {m_lblSpeed, m_lblDensity, m_lblGlyph, m_lblTrail, m_lblExtra1, m_lblExtra2, m_lblFps, m_lblColorA,
                         m_lblColorB};
        for (HWND w : labels)
            if (controlShown(w)) labelW = std::max(labelW, textPx(w) + dpiPx(8));
        const int valueW = std::max(dpiPx(72), textPx(m_valGlyph) + dpiPx(16));
        int trackW = inner - labelW - valueW - gap * 2;
        if (trackW < dpiPx((int)ui::kMinTrack)) {
            labelW = std::max(dpiPx(64), labelW - (dpiPx((int)ui::kMinTrack) - trackW));
            trackW = std::max(dpiPx(48), inner - labelW - valueW - gap * 2);
        }
        const int trackX = pad + labelW + gap;
        const int valueX = pad + inner - valueW;

        auto hide = [&](HWND w) {
            if (w) SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE);
        };
        auto put = [&](HWND w, int x, int y, int ww, int hh) {
            if (!w) return;
            if (!controlShown(w)) {
                hide(w);
                return;
            }
            SetWindowPos(w, nullptr, x, y - m_scroll, std::max(0, ww), std::max(0, hh), SWP_NOZORDER | SWP_NOACTIVATE);
        };
        int y = pad;
        const int statusW = std::min(dpiPx(280), inner / 2);
        put(m_lblTitle, pad, y, std::max(gap, inner - statusW - gap), dpiPx(24));
        put(m_status, pad + inner - statusW, y, statusW, dpiPx(24));
        y += dpiPx(24) + section;

        put(m_lblWallpaper, pad, y, inner, headerH);
        y += headerH + gap;
        put(m_list, pad, y, inner, dpiPx(96));
        y += dpiPx(96) + gap;
        const int btnGap = gap;
        const int btnW = std::max(dpiPx(80), (inner - btnGap * 2) / 3);
        put(m_show, pad, y, btnW, btnH);
        put(m_hide, pad + btnW + btnGap, y, btnW, btnH);
        put(m_pause, pad + (btnW + btnGap) * 2, y, inner - (btnW + btnGap) * 2, btnH);
        y += btnH + section;

        const int galleryW = std::min(dpiPx(156), std::max(dpiPx(120), inner / 3));
        put(m_grpFx, pad, y, std::max(gap, inner - galleryW - gap), btnH);
        put(m_btnGallery, pad + inner - galleryW, y, galleryW, btnH);
        y += btnH + gap;

        auto colorRow = [&](HWND label, HWND swatch, HWND button) {
            if (!controlShown(label)) {
                hide(label);
                hide(swatch);
                hide(button);
                return;
            }
            const int swH = dpiPx(22);
            const int swW = dpiPx(36);
            put(label, pad, y, labelW, rowH);
            put(swatch, trackX, y + (rowH - swH) / 2, swW, swH);
            put(button, trackX + swW + gap, y + (rowH - dpiPx(28)) / 2, dpiPx(104), dpiPx(28));
            y += rowH + gap;
        };
        colorRow(m_lblColorA, m_swatchA, m_btnColorA);
        colorRow(m_lblColorB, m_swatchB, m_btnColorB);

        auto sliderRow = [&](HWND label, HWND track, HWND value) {
            if (!controlShown(label)) {
                hide(label);
                hide(track);
                hide(value);
                return;
            }
            const int th = dpiPx(24);
            put(label, pad, y, labelW, rowH);
            put(track, trackX, y + (rowH - th) / 2, trackW, th);
            put(value, valueX, y, valueW, rowH);
            y += rowH + gap;
        };
        sliderRow(m_lblSpeed, m_slSpeed, m_valSpeed);
        sliderRow(m_lblDensity, m_slDensity, m_valDensity);
        sliderRow(m_lblGlyph, m_slGlyph, m_valGlyph);
        sliderRow(m_lblTrail, m_slTrail, m_valTrail);
        sliderRow(m_lblExtra1, m_slExtra1, m_valExtra1);
        sliderRow(m_lblExtra2, m_slExtra2, m_valExtra2);

        auto checkRow = [&](HWND w) {
            if (!controlShown(w)) {
                hide(w);
                return;
            }
            put(w, pad, y, inner, dpiPx(24));
            y += dpiPx(24) + gap;
        };
        checkRow(m_chkColor);
        checkRow(m_chkEye);
        checkRow(m_chkBreathe);

        if (y > pad) y += section - gap;
        put(m_lblPlayback, pad, y, inner, headerH);
        y += headerH + gap;
        sliderRow(m_lblFps, m_slFps, m_valFps);
        checkRow(m_chkFs);
        checkRow(m_chkCovered);
        checkRow(m_chkBattery);
        checkRow(m_chkAuto);
        checkRow(m_chkIcons);

        if (y > pad) y += section - gap;
        const int half = (inner - gap) / 2;
        put(m_btnOpenCfg, pad, y, half, btnH);
        put(m_btnExit, pad + half + gap, y, inner - half - gap, btnH);
        y += btnH + gap;
        put(m_note, pad, y, inner, dpiPx(52));
        y += dpiPx(52) + pad;
        return y;
    };

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    if (m_scroll < 0) m_scroll = 0;
    m_contentPx = flow((int)rc.right);
    updateScrollBar();
    RECT rc2{};
    GetClientRect(m_hwnd, &rc2);
    if (rc2.right != rc.right) m_contentPx = flow((int)rc2.right);
    const int view = std::max(1, (int)rc2.bottom);
    const int maxS = std::max(0, m_contentPx - view);
    if (m_scroll > maxS) {
        m_scroll = maxS;
        m_contentPx = flow((int)rc2.right);
    }
    updateScrollBar();
    m_inLayout = false;
}

void SettingsPanel::updateValueLabels() {
    wchar_t buf[32];
    if (!m_host) return;
    const AppSettings& s = m_host->settings();
    const std::string& id = s.effect;
    auto put = [&](HWND label, int index) {
        if (label) setText(label, knobReadout(s, id, index).c_str());
    };
    put(m_valSpeed, 0);
    if (id == "lava") {
        put(m_valDensity, 1);
        put(m_valGlyph, 3);
        put(m_valTrail, 4);
    } else if (id == "ape" || id == "myco") {
        put(m_valDensity, 1);
        put(m_valGlyph, 3);
        put(m_valTrail, 8);
        put(m_valExtra1, 4);
        put(m_valExtra2, 5);
    } else if (id == "matrix") {
        put(m_valDensity, 1);
        put(m_valGlyph, 4);
        put(m_valTrail, 5);
    } else if (id == "starfield" || id == "fireflies" || id == "snow") {
        put(m_valDensity, 1);
        if (id == "snow") put(m_valExtra1, 2);
    }
    std::swprintf(buf, 32, L"%d", trackPos(m_slFps));
    setText(m_valFps, buf);
}

void SettingsPanel::updateColorButtons() {
    InvalidateRect(m_swatchA, nullptr, TRUE);
    InvalidateRect(m_swatchB, nullptr, TRUE);
}

void SettingsPanel::syncToUi() {
    if (!m_host || !m_hwnd) return;
    m_syncing = true;
    const AppSettings& s = m_host->settings();

    SendMessageW(m_list, LB_RESETCONTENT, 0, 0);
    int sel = 0;
    int i = 0;
    for (const auto& e : m_host->registry().list()) {
        std::wstring name(e.displayName.begin(), e.displayName.end());
        SendMessageW(m_list, LB_ADDSTRING, 0, (LPARAM)name.c_str());
        SendMessageW(m_list, LB_SETITEMDATA, i, (LPARAM)i);
        if (e.id == s.effect) sel = i;
        ++i;
    }
    SendMessageW(m_list, LB_SETCURSEL, sel, 0);

    const bool hidden = m_host->wallpaperHidden();
    const bool paused = m_host->userPaused();
    EnableWindow(m_show, hidden ? TRUE : FALSE);
    EnableWindow(m_hide, hidden ? FALSE : TRUE);
    setText(m_pause, paused ? L"Resume" : L"Pause");
    EnableWindow(m_pause, hidden ? FALSE : TRUE);

    wchar_t status[96];
    if (hidden) lstrcpynW(status, L"Hidden — static wallpaper showing", 96);
    else if (paused) lstrcpynW(status, L"Paused", 96);
    else lstrcpynW(status, L"Running", 96);
    setText(m_status, status);

    const bool matrix = s.effect == "matrix";
    const bool star = s.effect == "starfield";
    const bool aurora = s.effect == "aurora";
    const bool lava = s.effect == "lava";
    const bool ape = s.effect == "ape";
    const bool myco = s.effect == "myco";
    const bool reveal = ape || myco;
    const bool fire = s.effect == "fireflies";
    const bool snow = s.effect == "snow";

    ShowWindow(m_lblGlyph, (matrix || lava || reveal) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_slGlyph, (matrix || lava || reveal) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_valGlyph, (matrix || lava || reveal) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_lblTrail, (matrix || lava || reveal) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_slTrail, (matrix || lava || reveal) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_valTrail, (matrix || lava || reveal) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_lblDensity, (matrix || star || lava || reveal || fire || snow) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_slDensity, (matrix || star || lava || reveal || fire || snow) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_valDensity, (matrix || star || lava || reveal || fire || snow) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_lblColorB, (matrix || aurora || lava) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_btnColorB, (matrix || aurora || lava) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_swatchB, (matrix || aurora || lava) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_lblExtra1, (reveal || snow) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_slExtra1, (reveal || snow) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_valExtra1, (reveal || snow) ? SW_SHOW : SW_HIDE);
    ShowWindow(m_lblExtra2, reveal ? SW_SHOW : SW_HIDE);
    ShowWindow(m_slExtra2, reveal ? SW_SHOW : SW_HIDE);
    ShowWindow(m_valExtra2, reveal ? SW_SHOW : SW_HIDE);
    ShowWindow(m_chkEye, reveal ? SW_SHOW : SW_HIDE);
    ShowWindow(m_chkBreathe, reveal ? SW_SHOW : SW_HIDE);
    ShowWindow(m_chkColor, myco ? SW_SHOW : SW_HIDE);
    setText(m_lblColorA, lava ? L"Lava color" : reveal ? L"Code color" : L"Color");
    setText(m_lblColorB, matrix ? L"Head color" : lava ? L"Liquid tint" : L"Second color");
    setText(m_lblDensity, lava ? L"Blobs" : reveal ? L"Rain density" : L"Density");
    setText(m_lblExtra1, snow ? L"Wind" : ape ? L"Ape size" : myco ? L"Size" : L"Ape size");
    setText(m_lblGlyph, lava ? L"Glow" : L"Glyph size");
    setText(m_lblTrail, lava ? L"Blob size" : reveal ? L"Brightness" : L"Trail length");
    setText(m_lblExtra2, ape ? L"Ape position" : myco ? L"Position" : L"Ape position");
    setText(m_grpFx, matrix   ? L"Matrix settings"
                     : star   ? L"Starfield settings"
                     : aurora ? L"Aurora settings"
                     : lava   ? L"Lava lamp settings"
                     : myco   ? L"Slow Myco settings"
                     : ape    ? L"Code Ape settings"
                     : fire   ? L"Fireflies settings"
                     : snow   ? L"Snowfall settings"
                              : L"Wallpaper settings");

    auto bind = [&](HWND sl, int index, bool integer) {
        float lo = 0, hi = 1;
        if (!sliderRange(s.effect, index, lo, hi)) return;
        showSlider(sl, knobValue(s, s.effect, index), lo, hi, integer);
    };
    if (lava) {
        bind(m_slSpeed, 0, false);
        bind(m_slDensity, 1, true);
        bind(m_slGlyph, 3, false);
        bind(m_slTrail, 4, false);
    } else if (reveal) {
        bind(m_slSpeed, 0, false);
        bind(m_slDensity, 1, false);
        bind(m_slGlyph, 3, true);
        bind(m_slTrail, 8, false);
        bind(m_slExtra1, 4, false);
        bind(m_slExtra2, 5, false);
        if (myco) {
            setCheck(m_chkEye, knobToggle(s, s.effect, 1));
            setCheck(m_chkBreathe, knobToggle(s, s.effect, 0));
            setCheck(m_chkColor, knobToggle(s, s.effect, 2));
        } else {
            setCheck(m_chkEye, knobToggle(s, s.effect, 0));
            setCheck(m_chkBreathe, knobToggle(s, s.effect, 1));
        }
    } else if (matrix) {
        bind(m_slSpeed, 0, false);
        bind(m_slDensity, 1, false);
        bind(m_slGlyph, 4, true);
        bind(m_slTrail, 5, false);
    } else if (star || fire) {
        bind(m_slSpeed, 0, false);
        bind(m_slDensity, 1, false);
    } else if (snow) {
        bind(m_slSpeed, 0, false);
        bind(m_slDensity, 1, false);
        bind(m_slExtra1, 2, false);
    } else {
        bind(m_slSpeed, 0, false);
    }
    layout();
    setTrack(m_slFps, Config::clampFps(s.fps));
    setCheck(m_chkFs, s.pauseOnFullscreen);
    setCheck(m_chkCovered, s.pauseOnCovered);
    setCheck(m_chkBattery, s.pauseOnBatterySaver);
    setCheck(m_chkAuto, s.startWithWindows);
    setCheck(m_chkIcons, s.hideDesktopIcons);
    updateValueLabels();
    updateColorButtons();
    m_syncing = false;
}

void SettingsPanel::applyFromUi() {
    if (!m_host || m_syncing) return;
    AppSettings& s = m_host->settings();
    const std::string& id = s.effect;
    if (id == "matrix") {
        writeSlider(s, id, m_slSpeed, 0);
        writeSlider(s, id, m_slDensity, 1);
        writeSlider(s, id, m_slGlyph, 4);
        writeSlider(s, id, m_slTrail, 5);
    } else if (id == "starfield") {
        writeSlider(s, id, m_slSpeed, 0);
        writeSlider(s, id, m_slDensity, 1);
    } else if (id == "aurora") {
        writeSlider(s, id, m_slSpeed, 0);
    } else if (id == "fireflies") {
        writeSlider(s, id, m_slSpeed, 0);
        writeSlider(s, id, m_slDensity, 1);
    } else if (id == "snow") {
        writeSlider(s, id, m_slSpeed, 0);
        writeSlider(s, id, m_slDensity, 1);
        writeSlider(s, id, m_slExtra1, 2);
    } else if (id == "lava") {
        writeSlider(s, id, m_slSpeed, 0);
        writeSlider(s, id, m_slDensity, 1);
        writeSlider(s, id, m_slGlyph, 3);
        writeSlider(s, id, m_slTrail, 4);
    } else if (id == "ape" || id == "myco") {
        writeSlider(s, id, m_slSpeed, 0);
        writeSlider(s, id, m_slDensity, 1);
        writeSlider(s, id, m_slGlyph, 3);
        writeSlider(s, id, m_slTrail, 8);
        writeSlider(s, id, m_slExtra1, 4);
        writeSlider(s, id, m_slExtra2, 5);
        if (id == "myco") {
            setKnobToggle(s, id, 1, checked(m_chkEye));
            setKnobToggle(s, id, 0, checked(m_chkBreathe));
            setKnobToggle(s, id, 2, checked(m_chkColor));
        } else {
            setKnobToggle(s, id, 0, checked(m_chkEye));
            setKnobToggle(s, id, 1, checked(m_chkBreathe));
        }
    }
    s.fps = Config::clampFps(trackPos(m_slFps));
    s.pauseOnFullscreen = checked(m_chkFs);
    s.pauseOnCovered = checked(m_chkCovered);
    s.pauseOnBatterySaver = checked(m_chkBattery);
    s.startWithWindows = checked(m_chkAuto);
    s.hideDesktopIcons = checked(m_chkIcons);
    updateValueLabels();
    m_host->applySettingsLive();
}

void SettingsPanel::pickColor(int which) {
    if (!m_host) return;
    AppSettings& s = m_host->settings();
    uint32_t* target = nullptr;
    if (which == 0) {
        if (s.effect == "matrix") target = &s.matrix.color;
        else if (s.effect == "starfield") target = &s.starfield.color;
        else if (s.effect == "lava") target = &s.lava.lavaColor;
        else if (s.effect == "ape") target = &s.ape.color;
        else if (s.effect == "myco") target = &s.myco.color;
        else if (s.effect == "fireflies") target = &s.fireflies.color;
        else if (s.effect == "snow") target = &s.snow.color;
        else target = &s.aurora.colorA;
    } else {
        if (s.effect == "matrix") target = &s.matrix.headColor;
        else if (s.effect == "lava") target = &s.lava.liquidColor;
        else target = &s.aurora.colorB;
    }
    if (!target) return;

    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = m_hwnd;
    cc.lpCustColors = &customColors();
    cc.rgbResult = colorFromHex(*target);
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    const uint32_t rgb = ((uint32_t)GetRValue(cc.rgbResult) << 16) | ((uint32_t)GetGValue(cc.rgbResult) << 8) |
                         (uint32_t)GetBValue(cc.rgbResult);
    setKnobColor(s, s.effect, which, rgb);
    updateColorButtons();
    m_host->applySettingsLive();
}

LRESULT CALLBACK SettingsPanel::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsPanel* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsPanel*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<SettingsPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handle(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SettingsPanel::handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_HSCROLL:
            if (!m_syncing) applyFromUi();
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            if (id == IDC_LIST && code == LBN_SELCHANGE && m_host) {
                const int sel = (int)SendMessageW(m_list, LB_GETCURSEL, 0, 0);
                const auto list = m_host->registry().list();
                if (sel >= 0 && sel < (int)list.size()) {
                    m_host->switchEffect(list[(size_t)sel].id);
                    syncToUi();
                }
                return 0;
            }
            if (code == BN_CLICKED || code == 0) {
                switch (id) {
                    case IDC_SHOW:
                        if (m_host) m_host->setWallpaperHidden(false);
                        syncToUi();
                        return 0;
                    case IDC_HIDE:
                        if (m_host) m_host->setWallpaperHidden(true);
                        syncToUi();
                        return 0;
                    case IDC_PAUSE:
                        if (m_host) m_host->setUserPaused(!m_host->userPaused());
                        syncToUi();
                        return 0;
                    case IDC_COLOR_A:
                        pickColor(0);
                        return 0;
                    case IDC_COLOR_B:
                        pickColor(1);
                        return 0;
                    case IDC_CHK_FS:
                    case IDC_CHK_COVERED:
                    case IDC_CHK_BATTERY:
                    case IDC_CHK_AUTO:
                    case IDC_CHK_ICONS:
                    case IDC_CHK_EYE:
                    case IDC_CHK_BREATHE:
                    case IDC_CHK_COLOR:
                        applyFromUi();
                        return 0;
                    case IDC_GALLERY:
                        if (m_host) m_host->openGallery();
                        return 0;
                    case IDC_OPENCFG:
                        if (m_host) m_host->openConfigFile();
                        return 0;
                    case IDC_EXIT:
                        if (m_host) m_host->exitApp();
                        return 0;
                }
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HWND ctl = (HWND)lParam;
            HDC hdc = (HDC)wParam;
            if (m_host && (ctl == m_swatchA || ctl == m_swatchB)) {
                const AppSettings& s = m_host->settings();
                uint32_t rgb = 0;
                if (ctl == m_swatchA) {
                    if (s.effect == "matrix") rgb = s.matrix.color;
                    else if (s.effect == "starfield") rgb = s.starfield.color;
                    else if (s.effect == "lava") rgb = s.lava.lavaColor;
                    else if (s.effect == "ape") rgb = s.ape.color;
                    else if (s.effect == "myco") rgb = s.myco.color;
                    else if (s.effect == "fireflies") rgb = s.fireflies.color;
                    else if (s.effect == "snow") rgb = s.snow.color;
                    else rgb = s.aurora.colorA;
                } else {
                    if (s.effect == "matrix") rgb = s.matrix.headColor;
                    else if (s.effect == "lava") rgb = s.lava.liquidColor;
                    else rgb = s.aurora.colorB;
                }
                SetBkColor(hdc, colorFromHex(rgb));
                static HBRUSH brushes[2]{};
                HBRUSH& br = brushes[ctl == m_swatchA ? 0 : 1];
                if (br) DeleteObject(br);
                br = CreateSolidBrush(colorFromHex(rgb));
                return (LRESULT)br;
            }
            break;
        }
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            m_dpi = (int)HIWORD(wParam);
            if (m_dpi <= 0) m_dpi = 96;
            applyWindowIcons(m_hwnd, m_instance);
            if (suggested) {
                SetWindowPos(m_hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            layout();
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            m_scroll -= (delta / WHEEL_DELTA) * dpiPx(48);
            layout();
            return 0;
        }
        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(m_hwnd, SB_VERT, &si);
            int pos = si.nPos;
            switch (LOWORD(wParam)) {
            case SB_TOP: pos = si.nMin; break;
            case SB_BOTTOM: pos = si.nMax; break;
            case SB_LINEUP: pos -= dpiPx(24); break;
            case SB_LINEDOWN: pos += dpiPx(24); break;
            case SB_PAGEUP: pos -= (int)si.nPage; break;
            case SB_PAGEDOWN: pos += (int)si.nPage; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION: pos = (short)HIWORD(wParam); break;
            default: break;
            }
            m_scroll = pos;
            layout();
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(m_hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            releaseWindowIcons(m_hwnd);
            if (m_hwnd == (HWND)0 || GetWindowLongPtrW(m_hwnd, GWLP_USERDATA)) {
                SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            }
            m_hwnd = nullptr;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

bool SettingsPanel::runScrollSelfTest(std::string& report) {
    if (!m_hwnd || !m_host) {
        report = "SettingsScroll: FAIL (no window)\n";
        return false;
    }
    RECT wr{};
    GetWindowRect(m_hwnd, &wr);
    const int savedScroll = m_scroll;
    struct Restore {
        SettingsPanel* panel;
        RECT wr;
        int scroll;
        ~Restore() {
            panel->m_scroll = scroll;
            SetWindowPos(panel->m_hwnd, nullptr, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            panel->layout();
        }
    } restore{this, wr, savedScroll};

    m_scroll = 0;
    layout();
    {
        RECT client{};
        GetClientRect(m_hwnd, &client);
        const int inset = dpiPx((int)ui::kPad);
        std::vector<RECT> seen;
        HWND all[] = {m_lblTitle,    m_status,     m_lblWallpaper, m_list,       m_show,       m_hide,
                      m_pause,       m_grpFx,      m_btnGallery,   m_lblColorA,  m_swatchA,    m_btnColorA,
                      m_lblColorB,   m_swatchB,    m_btnColorB,    m_lblSpeed,   m_slSpeed,    m_valSpeed,
                      m_lblDensity,  m_slDensity,  m_valDensity,   m_lblGlyph,   m_slGlyph,    m_valGlyph,
                      m_lblTrail,    m_slTrail,    m_valTrail,     m_lblExtra1,  m_slExtra1,   m_valExtra1,
                      m_lblExtra2,   m_slExtra2,   m_valExtra2,    m_chkEye,     m_chkBreathe, m_chkColor,
                      m_lblPlayback, m_lblFps,     m_slFps,        m_valFps,     m_chkFs,      m_chkCovered,
                      m_chkBattery,  m_chkAuto,    m_chkIcons,     m_btnOpenCfg, m_btnExit,    m_note};
        int maxBottom = 0;
        for (HWND w : all) {
            if (!controlShown(w)) continue;
            RECT wrc{};
            if (!GetWindowRect(w, &wrc)) continue;
            POINT a{wrc.left, wrc.top};
            POINT b{wrc.right, wrc.bottom};
            ScreenToClient(m_hwnd, &a);
            ScreenToClient(m_hwnd, &b);
            if (b.x - a.x < 2 || b.y - a.y < 2) continue;
            if (a.x < inset - 2) {
                report = "SettingsScroll: FAIL left inset\n";
                return false;
            }
            if (b.x > client.right - inset + 2) {
                report = "SettingsScroll: FAIL right inset\n";
                return false;
            }
            if (a.y + m_scroll < inset - 2) {
                report = "SettingsScroll: FAIL top inset\n";
                return false;
            }
            maxBottom = std::max(maxBottom, (int)b.y + m_scroll);
            for (const RECT& o : seen) {
                if (a.x < o.right - 1 && o.left < b.x - 1 && a.y < o.bottom - 1 && o.top < b.y - 1) {
                    report = "SettingsScroll: FAIL controls overlap\n";
                    return false;
                }
            }
            seen.push_back(RECT{a.x, a.y, b.x, b.y});
        }
        if (maxBottom > contentHeightPx() - inset + 3) {
            report = "SettingsScroll: FAIL bottom padding\n";
            return false;
        }
    }

    RECT cr{};
    GetClientRect(m_hwnd, &cr);
    const int frameH = (wr.bottom - wr.top) - cr.bottom;
    SetWindowPos(m_hwnd, nullptr, wr.left, wr.top, wr.right - wr.left, frameH + 280, SWP_NOZORDER | SWP_NOACTIVATE);
    m_scroll = 0;
    layout();
    GetClientRect(m_hwnd, &cr);
    if (contentHeightPx() <= cr.bottom + 4) {
        report = "SettingsScroll: FAIL content still fits\n";
        return false;
    }
    RECT speed0{};
    GetWindowRect(m_slSpeed, &speed0);
    const int speedY = speed0.top;
    m_scroll = 64;
    layout();
    RECT speed1{};
    GetWindowRect(m_slSpeed, &speed1);
    if (std::abs((speedY - 64) - speed1.top) > 2) {
        report = "SettingsScroll: FAIL offset\n";
        return false;
    }
    POINT pt{(speed0.left + speed0.right) / 2, speedY + std::max(1L, (speed0.bottom - speed0.top) / 2)};
    if (WindowFromPoint(pt) == m_slSpeed) {
        report = "SettingsScroll: FAIL hit ignored scroll\n";
        return false;
    }
    RECT win0{};
    GetWindowRect(m_hwnd, &win0);
    const int sc = m_scroll;
    SendMessageW(m_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(win0.left + 30, win0.top + 80));
    RECT win1{};
    GetWindowRect(m_hwnd, &win1);
    if (win1.left != win0.left || win1.top != win0.top) {
        report = "SettingsScroll: FAIL wheel moved the window\n";
        return false;
    }
    if (m_scroll >= sc) {
        report = "SettingsScroll: FAIL wheel\n";
        return false;
    }
    report = "SettingsScroll: PASS\n";
    return true;
}
