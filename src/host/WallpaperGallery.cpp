#include "host/WallpaperGallery.h"
#include "host/UiMetrics.h"

#include "core/ComPtr.h"
#include "effect/Presets.h"
#include "core/Config.h"
#include "core/Log.h"
#include "core/Util.h"
#include "effect/EffectRegistry.h"
#include "host/ThumbnailStage.h"

#include <algorithm>
#include <cmath>
#include <commdlg.h>
#include <cstdio>
#include <cstring>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <windowsx.h>

#ifdef DrawText
#undef DrawText
#endif
#ifdef DrawTextW
#undef DrawTextW
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace {

constexpr float kRail = 176.0f;
constexpr float kSide = 320.0f;
constexpr float kGap = 12.0f;
constexpr float kPad = ui::kPad;
constexpr float kMinCard = 200.0f;

float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

UINT galleryDpi(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = (Fn)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn && hwnd) {
        const UINT d = fn(hwnd);
        if (d) return d;
    }
    HDC dc = GetDC(hwnd ? hwnd : nullptr);
    const UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return d ? d : 96;
}

D2D1_COLOR_F rgb(uint32_t c, float a) {
    return D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a);
}

int columnCount(float innerW) {
    if (innerW < 40.0f) return 1;
    return std::max(1, (int)std::floor((innerW + kGap) / (kMinCard + kGap)));
}

float wrapHeight(IDWriteFactory* dw, const wchar_t* text, float width, float size) {
    if (!text || !text[0]) return 0;
    if (!dw) return size + 4.0f;
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", fmt.put())) ||
        !fmt) {
        return size + 4.0f;
    }
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(text, (UINT32)wcslen(text), fmt.get(), std::max(20.0f, width), 600.0f,
                                    layout.put())) ||
        !layout) {
        return size + 4.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return std::max(size, metrics.height + 2.0f);
}

}  // namespace

struct GalleryUi {
    enum Kind { HitNone = 0, HitGroup, HitCard, HitStar, HitSlider, HitSwatch, HitToggle, HitChip, HitButton, HitScroll };

    struct Rect {
        float x = 0, y = 0, w = 0, h = 0;
        bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
        D2D1_RECT_F box() const { return D2D1::RectF(x, y, x + w, y + h); }
    };

    struct Card {
        std::string id;
        std::wstring name, blurb;
        Rect body, thumb, star;
        bool favorite = false;
    };
    struct Slider {
        Rect row, track, labelBox, valueBox;
        int index = 0;
        float t = 0;
        const wchar_t* label = L"";
        std::wstring value;
    };
    struct Chip {
        Rect box;
        int preset = -1;
        std::wstring label;
    };
    struct Header {
        Rect row;
        const wchar_t* label = L"";
    };
    struct Button {
        Rect box;
        int index = 0;
        std::wstring label;
    };
    struct Swatch {
        Rect row, box;
        int which = 0;
        uint32_t rgb = 0;
        const wchar_t* label = L"";
    };
    struct Toggle {
        Rect row;
        int index = 0;
        bool on = false;
        const wchar_t* label = L"";
    };
    struct GroupRow {
        Rect row;
        std::wstring name;
    };
    struct Layout {
        float W = 0, H = 0;
        int columns = 1;
        float innerW = 0;
        Rect rail, grid, side;
        std::vector<GroupRow> groups;
        std::vector<Card> cards;
        std::wstring title, desc;
        Rect titleRect, descRect;
        float contentX = 0, contentW = 0;
        std::vector<Chip> chips;
        std::vector<Header> headers;
        std::vector<Slider> sliders;
        std::vector<Swatch> swatches;
        std::vector<Toggle> toggles;
        std::vector<Button> buttons;
        float gridContent = 0;
        float sideContent = 0;
        Rect gridThumb, sideThumb;
        bool emptyGroup = false;
        std::string panelId;
    };
    struct Hit {
        int kind = HitNone;
        int index = -1;
        std::string id;
    };

    static std::vector<std::string> groupNames(const WallpaperGallery& g) {
        std::vector<std::string> names;
        names.emplace_back("Favorites");
        if (g.m_host)
            for (const auto& name : g.m_host->registry().groups()) names.push_back(name);
        return names;
    }

    static std::vector<EffectInfo> cardsIn(const WallpaperGallery& g, int group) {
        std::vector<EffectInfo> out;
        if (!g.m_host) return out;
        const auto names = groupNames(g);
        if (group < 0 || group >= (int)names.size()) return out;
        const std::string& name = names[(size_t)group];
        for (const auto& e : g.m_host->registry().list()) {
            if (name == "Favorites") {
                if (g.m_host->isFavorite(e.id)) out.push_back(e);
            } else if (e.group == name) {
                out.push_back(e);
            }
        }
        return out;
    }

    static Layout build(const WallpaperGallery& g, float W, float H) {
        Layout L;
        L.W = std::max(1.0f, W);
        L.H = std::max(1.0f, H);
        L.rail = {0, 0, std::min(kRail, L.W), L.H};
        L.side = {std::max(0.0f, L.W - kSide), 0, std::min(kSide, L.W), L.H};
        L.grid = {L.rail.w, 0, std::max(0.0f, L.side.x - L.rail.w), L.H};
        const float innerX = L.grid.x + kPad;
        L.innerW = std::max(40.0f, L.grid.w - ui::kPad - ui::kGutter);
        L.columns = columnCount(L.innerW);
        const float cardW = (L.innerW - kGap * (float)(L.columns - 1)) / (float)L.columns;
        const float thumbH = std::min(150.0f, std::max(72.0f, (cardW - 20.0f) * 9.0f / 16.0f));
        const float cardH = thumbH + 64.0f;

        const auto names = groupNames(g);
        for (int i = 0; i < (int)names.size(); ++i) {
            GroupRow row;
            row.row = {ui::kPad, 56.0f + (float)i * 44.0f, kRail - ui::kPad * 2.0f, 36.0f};
            row.name = widen(names[(size_t)i]);
            L.groups.push_back(row);
        }

        const int group = (g.m_group >= 0 && g.m_group < (int)names.size()) ? g.m_group : 0;
        const auto shown = cardsIn(g, group);
        L.emptyGroup = shown.empty();
        L.panelId = g.m_panelId;
        const int rows = shown.empty() ? 0 : ((int)shown.size() + L.columns - 1) / L.columns;
        L.gridContent = rows > 0 ? (float)rows * (cardH + kGap) - kGap : 0;
        for (int i = 0; i < (int)shown.size(); ++i) {
            Card c;
            c.id = shown[(size_t)i].id;
            c.name = widen(shown[(size_t)i].displayName);
            c.blurb = widen(shown[(size_t)i].description);
            c.favorite = g.m_host && g.m_host->isFavorite(c.id);
            const int col = i % L.columns;
            const int row = i / L.columns;
            const float x = innerX + (float)col * (cardW + kGap);
            const float y = L.grid.y + kPad + (float)row * (cardH + kGap) - g.m_gridScroll;
            c.body = {x, y, cardW, cardH};
            c.thumb = {x + 10.0f, y + 10.0f, cardW - 20.0f, thumbH};
            c.star = {x + cardW - 34.0f, y + 14.0f, 24.0f, 24.0f};
            L.cards.push_back(c);
        }

        const float sx = L.side.x + ui::kPad;
        const float sw = ui::contentWidth(L.side.w);
        L.contentX = sx;
        L.contentW = sw;
        float y = L.side.y + ui::kPad - g.m_sideScroll;
        const auto* info = (g.m_host && !g.m_panelId.empty()) ? g.m_host->registry().find(g.m_panelId) : nullptr;
        L.title = info ? widen(info->displayName) : L"";
        L.desc = info ? widen(info->description) : L"";
        auto* dw = static_cast<IDWriteFactory*>(g.m_dwrite);
        L.titleRect = {sx, y, sw, 28.0f};
        y += 28.0f + ui::kGap;
        const float descH = L.desc.empty() ? 0 : wrapHeight(dw, L.desc.c_str(), sw, 13.0f);
        L.descRect = {sx, y, sw, descH};
        if (descH > 0) y += descH + ui::kGap;

        constexpr float kFont = 12.0f;
        ui::Columns cols = ui::columnsFor(sw, ui::kMinLabel, ui::textWidth(L"1.00x", kFont));
        if (g.m_host && !g.m_panelId.empty()) {
            const std::string& id = g.m_panelId;
            const auto& settings = g.m_host->settings();
            float labelNeed = ui::kMinLabel;
            float valueNeed = ui::textWidth(L"1.00x", kFont);
            const int n = knobCount(id);
            for (int i = 0; i < n; ++i) {
                const Knob knob = knobAt(id, i);
                if (knob.kind != KnobKind::Slider || !knob.label) continue;
                labelNeed = std::max(labelNeed, ui::textWidth(knob.label, kFont));
                valueNeed = std::max(valueNeed, ui::textWidth(knobReadout(settings, id, knob.index).c_str(), kFont));
            }
            cols = ui::columnsFor(sw, labelNeed, valueNeed);

            float cx = 0;
            float cy = y;
            auto placeChip = [&](int preset, const std::wstring& label) {
                const float w = ui::chipWidth(label.c_str(), kFont, sw);
                const float lead = (preset < 0 && cx > 0.0f) ? ui::kGap : 0.0f;
                if (cx > 0.0f && cx + lead + w > sw + 0.5f) {
                    cx = 0;
                    cy += ui::kChipH + ui::kChipGap;
                } else {
                    cx += lead;
                }
                Chip chip;
                chip.preset = preset;
                chip.label = label;
                chip.box = {sx + cx, cy, w, ui::kChipH};
                L.chips.push_back(chip);
                cx += w + ui::kChipGap;
            };
            const int presets = presetCount(id);
            for (int i = 0; i < presets; ++i) placeChip(i, widen(presetName(id, i)));
            placeChip(-1, L"Reset to default");
            y = cy + ui::kChipH;

            const wchar_t* sectionName[] = {L"Motion", L"Look", L"Color", L"Advanced"};
            for (int sec = 0; sec < 4; ++sec) {
            bool any = false;
            bool rowStarted = false;
            for (int i = 0; i < n; ++i) {
                const Knob knob = knobAt(id, i);
                if ((int)knob.section != sec) continue;
                if (!any) {
                    y += ui::kSectionGap;
                    Header hd;
                    hd.label = sectionName[sec];
                    hd.row = {sx, y, sw, ui::kHeaderH};
                    L.headers.push_back(hd);
                    y += ui::kHeaderH + ui::kHeaderGap;
                    any = true;
                }
                if (rowStarted) y += ui::kRowGap;
                if (knob.kind == KnobKind::Slider) {
                    const float value = knobValue(settings, id, knob.index);
                    const float span = knob.hi - knob.lo;
                    Slider s;
                    s.index = knob.index;
                    s.label = knob.label;
                    s.row = {sx, y, sw, ui::kRowH};
                    s.labelBox = {sx, y, cols.label, ui::kRowH};
                    s.track = {sx + cols.label + ui::kGap, y + (ui::kRowH - ui::kTrackH) * 0.5f, cols.track, ui::kTrackH};
                    s.valueBox = {sx + sw - cols.value, y, cols.value, ui::kRowH};
                    s.t = span > 0 ? clampf((value - knob.lo) / span, 0.0f, 1.0f) : 0;
                    s.value = knobReadout(settings, id, knob.index);
                    L.sliders.push_back(s);
                    y += ui::kRowH;
                } else if (knob.kind == KnobKind::Swatch) {
                    Swatch swatch;
                    swatch.label = knob.label;
                    swatch.which = knob.index;
                    swatch.rgb = knobColor(settings, id, knob.index);
                    swatch.row = {sx, y, sw, ui::kRowH};
                    swatch.box = {sx + sw - 28.0f, y + (ui::kRowH - 20.0f) * 0.5f, 28.0f, 20.0f};
                    L.swatches.push_back(swatch);
                    y += ui::kRowH;
                } else if (knob.kind == KnobKind::Toggle) {
                    Toggle t;
                    t.label = knob.label;
                    t.index = knob.index;
                    t.on = knobToggle(settings, id, knob.index);
                    t.row = {sx, y, sw, ui::kRowH};
                    L.toggles.push_back(t);
                    y += ui::kRowH;
                } else if (knob.kind == KnobKind::Button) {
                    Button b;
                    b.index = knob.index;
                    b.label = knob.label;
                    const float bw = std::min(sw, std::max(128.0f, ui::textWidth(b.label.c_str(), kFont) + ui::kChipPadX * 2.0f));
                    b.box = {sx, y, bw, ui::kBtnH};
                    L.buttons.push_back(b);
                    y += ui::kBtnH;
                } else {
                    continue;
                }
                rowStarted = true;
            }
            }
        }
        y += ui::kBottom + g.m_sideScroll;
        L.sideContent = std::max(0.0f, y - L.side.y);
        auto thumb = [](const Rect& view, float maxScroll, float scroll) {
            Rect t;
            if (maxScroll < 1.0f || view.h < 48.0f) return t;
            const float track = view.h - ui::kGap * 2.0f;
            const float th = std::max(28.0f, track * (view.h / (view.h + maxScroll)));
            const float travel = std::max(1.0f, track - th);
            t = {ui::thumbX(view.x, view.w), view.y + ui::kGap + travel * (scroll / maxScroll), ui::kThumbW, th};
            return t;
        };
        const float maxG = std::max(0.0f, L.gridContent - std::max(0.0f, L.grid.h - kPad * 2.0f));
        const float maxS = std::max(0.0f, L.sideContent - L.side.h);
        L.gridThumb = thumb(L.grid, maxG, g.m_gridScroll);
        L.sideThumb = thumb(L.side, maxS, g.m_sideScroll);
        return L;
    }

    static Hit hit(const Layout& L, float x, float y) {
        Hit h;
        auto onThumb = [&](const Rect& t, int which) {
            Rect fat = t;
            fat.x -= 4.0f;
            fat.w += 8.0f;
            if (t.h > 1.0f && fat.contains(x, y)) {
                h.kind = HitScroll;
                h.index = which;
                return true;
            }
            return false;
        };
        if (onThumb(L.gridThumb, 0) || onThumb(L.sideThumb, 1)) return h;
        if (L.grid.contains(x, y)) {
            for (int i = (int)L.cards.size() - 1; i >= 0; --i) {
                const auto& c = L.cards[(size_t)i];
                if (c.star.contains(x, y)) {
                    h.kind = HitStar;
                    h.index = i;
                    h.id = c.id;
                    return h;
                }
                if (c.body.contains(x, y)) {
                    h.kind = HitCard;
                    h.index = i;
                    h.id = c.id;
                    return h;
                }
            }
        }
        if (L.side.contains(x, y)) {
            for (int i = 0; i < (int)L.chips.size(); ++i) {
                if (L.chips[(size_t)i].box.contains(x, y)) {
                    h.kind = HitChip;
                    h.index = L.chips[(size_t)i].preset;
                    return h;
                }
            }
            for (const auto& b : L.buttons) {
                if (b.box.contains(x, y)) {
                    h.kind = HitButton;
                    h.index = b.index;
                    return h;
                }
            }
            for (const auto& s : L.sliders) {
                if (s.row.contains(x, y)) {
                    h.kind = HitSlider;
                    h.index = s.index;
                    return h;
                }
            }
            for (const auto& s : L.swatches) {
                if (s.row.contains(x, y)) {
                    h.kind = HitSwatch;
                    h.index = s.which;
                    return h;
                }
            }
            for (const auto& t : L.toggles) {
                if (t.row.contains(x, y)) {
                    h.kind = HitToggle;
                    h.index = t.index;
                    return h;
                }
            }
        }
        if (L.rail.contains(x, y)) {
            for (int i = 0; i < (int)L.groups.size(); ++i) {
                if (L.groups[(size_t)i].row.contains(x, y)) {
                    h.kind = HitGroup;
                    h.index = i;
                    return h;
                }
            }
        }
        return h;
    }

    static void activate(WallpaperGallery& g, const Hit& h) {
        if (h.kind == HitGroup) {
            g.m_group = h.index;
            g.m_gridScroll = 0;
            g.m_sideScroll = 0;
            const auto shown = cardsIn(g, h.index);
            bool found = false;
            for (const auto& e : shown)
                if (e.id == g.m_panelId) found = true;
            if (!found) g.m_panelId = shown.empty() ? std::string() : shown.front().id;
            return;
        }
        if (h.kind == HitCard) {
            g.m_panelId = h.id;
            g.m_sideScroll = 0;
            if (g.m_host) g.m_host->switchEffect(h.id);
            return;
        }
        if (h.kind == HitStar) {
            if (g.m_host) g.m_host->toggleFavorite(h.id);
            return;
        }
        if (h.kind == HitChip) {
            if (!g.m_host || g.m_panelId.empty()) return;
            g.m_preset = h.index;
            if (h.index < 0) resetEffect(g.m_host->settings(), g.m_panelId);
            else applyPreset(g.m_host->settings(), g.m_panelId, h.index);
            g.m_host->applySettingsLive();
            return;
        }
        if (h.kind == HitButton) {
            g.m_preset = -1;
            if (g.m_panelId == "matrix") g.editGlyphs();
            else g.pickImage();
            return;
        }
        if (h.kind == HitSwatch) g.pickColor(h.index);
        else if (h.kind == HitToggle) g.toggleAt(h.index);
    }

    static void clampScroll(WallpaperGallery& g, float W, float H) {
        const Layout L = build(g, W, H);
        const float maxG = std::max(0.0f, L.gridContent - std::max(0.0f, L.grid.h - kPad * 2.0f));
        const float maxS = std::max(0.0f, L.sideContent - L.side.h);
        g.m_gridScroll = clampf(g.m_gridScroll, 0.0f, maxG);
        g.m_sideScroll = clampf(g.m_sideScroll, 0.0f, maxS);
    }

    static bool selfTest(WallpaperGallery& g, std::string& report) {
        if (!g.m_host) {
            report = "GalleryUI: FAIL (no host)\n";
            return false;
        }
        AppSettings saved = g.m_host->settings();
        const int savedGroup = g.m_group;
        const std::string savedPanel = g.m_panelId;
        const float savedGrid = g.m_gridScroll;
        const float savedSide = g.m_sideScroll;
        struct Restore {
            WallpaperGallery& g;
            SettingsPanel::Host* host;
            AppSettings saved;
            int group;
            std::string panel;
            float grid, side;
            ~Restore() {
                g.m_group = group;
                g.m_panelId = panel;
                g.m_gridScroll = grid;
                g.m_sideScroll = side;
                g.m_dragSlider = -1;
                g.m_hotKind = 0;
                g.m_downKind = 0;
                if (!host) return;
                host->settings() = saved;
                host->switchEffect(saved.effect);
            }
        } restore{g, g.m_host, saved, savedGroup, savedPanel, savedGrid, savedSide};

        auto fail = [&](const std::string& why) {
            report = "GalleryUI: FAIL " + why + "\n";
            return false;
        };

        const UINT savedDpi = g.m_dpi ? g.m_dpi : 96;
        for (UINT dpi : {96u, 120u, 144u}) {
            g.m_dpi = dpi;
            const float px = 320.0f * ((float)dpi / 96.0f);
            if (std::fabs(g.pxToDip(px) - 320.0f) > 0.6f) return fail("DIP conversion at " + std::to_string(dpi));
        }
        g.m_dpi = savedDpi;

        g.m_host->settings().favorites.clear();
        struct Size {
            float w, h;
            const char* name;
        };
        const Size sizes[] = {
            {1024.0f * 96.0f / 120.0f, 620.0f * 96.0f / 120.0f, "125%-1024"},
            {1400.0f, 860.0f, "100%-1400"},
            {1920.0f * 96.0f / 144.0f, 1000.0f * 96.0f / 144.0f, "150%-1920"},
            {860.0f, 560.0f, "min"},
        };

        int cardsHit = 0;
        bool cardActivated = false;
        for (const Size& sz : sizes) {
            g.m_gridScroll = 0;
            g.m_sideScroll = 0;
            Layout L = build(g, sz.w, sz.h);
            if (L.side.x + L.side.w > sz.w + 0.5f) return fail(std::string(sz.name) + " side panel past window");
            if (std::fabs((L.side.x + L.side.w) - sz.w) > 1.0f) return fail(std::string(sz.name) + " side panel not flush");
            if (L.columns != columnCount(L.innerW)) return fail(std::string(sz.name) + " column count");
            if (L.columns == 1 && !L.cards.empty() && L.cards[0].body.w < L.innerW * 0.90f)
                return fail(std::string(sz.name) + " single column leaves a gap");
            if (L.columns >= 2 && L.cards.size() >= 2 && L.cards[1].body.x <= L.cards[0].body.x + 1.0f)
                return fail(std::string(sz.name) + " cards did not reflow");
            if (std::string(sz.name) == "125%-1024" && L.columns != 1)
                return fail("125%-1024 expected one stretched column");
            if (std::string(sz.name) == "100%-1400" && L.columns < 3)
                return fail("100%-1400 expected at least 3 columns");

            for (int gi = 0; gi < (int)L.groups.size(); ++gi) {
                g.m_gridScroll = 0;
                const Rect& row = L.groups[(size_t)gi].row;
                const float hx = row.x + row.w * 0.5f;
                const float hy = row.y + row.h * 0.5f;
                const Hit got = hit(L, hx, hy);
                if (got.kind != HitGroup || got.index != gi)
                    return fail(std::string(sz.name) + " group " + std::to_string(gi) + " hit " +
                                std::to_string(got.kind));
                activate(g, got);
                const auto shown = cardsIn(g, gi);
                if (shown.empty()) {
                    if (!g.m_panelId.empty()) return fail(std::string(sz.name) + " empty group still has a panel");
                    Layout empty = build(g, sz.w, sz.h);
                    if (!empty.emptyGroup) return fail(std::string(sz.name) + " missing empty state");
                    continue;
                }
                bool panelInGroup = false;
                for (const auto& e : shown)
                    if (e.id == g.m_panelId) panelInGroup = true;
                if (!panelInGroup) return fail(std::string(sz.name) + " panel is not in group " + std::to_string(gi));

                Layout placed = build(g, sz.w, sz.h);
                if (placed.columns == 1 && !placed.cards.empty() && placed.cards[0].body.w < placed.innerW * 0.90f)
                    return fail(std::string(sz.name) + " single column leaves a gap");
                if (placed.columns >= 2 && placed.cards.size() >= 2 &&
                    placed.cards[1].body.x <= placed.cards[0].body.x + 1.0f)
                    return fail(std::string(sz.name) + " cards did not reflow");

                Layout G = build(g, sz.w, sz.h);
                for (const auto& ctl : G.sliders) {
                    if (ctl.track.x < G.side.x + 8.0f || ctl.track.x + ctl.track.w > G.side.x + G.side.w - 8.0f)
                        return fail(std::string(sz.name) + " slider outside panel");
                }
                for (const auto& ctl : G.swatches) {
                    if (ctl.box.w < 20.0f || ctl.box.x + ctl.box.w > G.side.x + G.side.w - 8.0f)
                        return fail(std::string(sz.name) + " swatch outside panel");
                }
                if (G.descRect.w > 1.0f && G.descRect.x + G.descRect.w > G.side.x + G.side.w - 8.0f)
                    return fail(std::string(sz.name) + " description wider than panel");
                {
                    const float scroll = g.m_sideScroll;
                    ui::R content{G.contentX, G.side.y + ui::kPad, G.contentW,
                                  std::max(0.0f, G.sideContent - ui::kPad - ui::kBottom)};
                    ui::R gutter{G.side.x + G.side.w - ui::kGutter, G.side.y, ui::kGutter,
                                 std::max(G.side.h, G.sideContent)};
                    std::vector<ui::R> items;
                    auto add = [&](const Rect& r) {
                        if (r.h > 1.0f) items.push_back({r.x, r.y + scroll, r.w, r.h});
                    };
                    add(G.titleRect);
                    add(G.descRect);
                    for (const auto& c : G.chips) add(c.box);
                    for (const auto& h : G.headers) add(h.row);
                    for (const auto& s : G.sliders) add(s.row);
                    for (const auto& s : G.swatches) add(s.row);
                    for (const auto& t : G.toggles) add(t.row);
                    for (const auto& b : G.buttons) add(b.box);
                    if (const char* why = ui::audit(items.data(), (int)items.size(), content, gutter))
                        return fail(std::string(sz.name) + " layout " + why);
                    for (const auto& c : G.chips) {
                        if (c.box.x + c.box.w > G.contentX + G.contentW + 0.5f)
                            return fail(std::string(sz.name) + " chip clipped");
                    }
                    if (!G.cards.empty()) {
                        ui::R gridContent{G.grid.x + ui::kPad, G.grid.y + ui::kPad, G.innerW,
                                          std::max(1.0f, G.gridContent)};
                        ui::R gridGutter{G.grid.x + G.grid.w - ui::kGutter, G.grid.y, ui::kGutter, G.grid.h};
                        std::vector<ui::R> cards;
                        cards.reserve(G.cards.size());
                        for (const auto& c : G.cards)
                            cards.push_back({c.body.x, c.body.y + g.m_gridScroll, c.body.w, c.body.h});
                        if (const char* why = ui::audit(cards.data(), (int)cards.size(), gridContent, gridGutter))
                            return fail(std::string(sz.name) + " grid " + why);
                    }
                }

                for (int ci = 0; ci < (int)shown.size(); ++ci) {
                    bool found = false;
                    for (int attempt = 0; attempt < 4 && !found; ++attempt) {
                        G = build(g, sz.w, sz.h);
                        if (ci >= (int)G.cards.size()) return fail(std::string(sz.name) + " missing card");
                        const Card& c = G.cards[(size_t)ci];
                        const float cx = c.body.x + 18.0f;
                        const float cy = c.thumb.y + c.thumb.h + 16.0f;
                        if (!G.grid.contains(cx, cy) || !G.grid.contains(c.star.x + 4.0f, c.star.y + 4.0f)) {
                            const float absTop = c.body.y + g.m_gridScroll;
                            g.m_gridScroll = std::max(0.0f, absTop - G.grid.y - 4.0f);
                            continue;
                        }
                        const Hit ch = hit(G, cx, cy);
                        if (ch.kind != HitCard || ch.id != shown[(size_t)ci].id)
                            return fail(std::string(sz.name) + " card " + shown[(size_t)ci].id);
                        const Hit sh = hit(G, c.star.x + c.star.w * 0.5f, c.star.y + c.star.h * 0.5f);
                        if (sh.kind != HitStar || sh.id != c.id)
                            return fail(std::string(sz.name) + " star " + c.id);
                        if (!cardActivated) {
                            activate(g, ch);
                            if (g.m_panelId != c.id || g.m_host->settings().effect != c.id)
                                return fail(std::string(sz.name) + " panel did not follow " + c.id);
                            ++cardsHit;
                        }
                        found = true;
                    }
                    if (!found) return fail(std::string(sz.name) + " could not reach " + shown[(size_t)ci].id);
                }
            }
            cardActivated = true;
        }

        g.m_host->settings().favorites.clear();
        g.m_gridScroll = 0;
        Hit fav{};
        fav.kind = HitGroup;
        fav.index = 0;
        activate(g, fav);
        Layout empty = build(g, 1100, 720);
        if (!empty.emptyGroup || !g.m_panelId.empty()) return fail("Favorites empty state");

        Hit retro{};
        retro.kind = HitGroup;
        retro.index = (int)groupNames(g).size() - 1;
        activate(g, retro);
        Layout lavaL = build(g, 1100, 720);
        const Card* lava = nullptr;
        for (const auto& c : lavaL.cards)
            if (c.id == "lava") lava = &c;
        if (!lava) return fail("Lava card missing");
        Hit lavaHit;
        lavaHit.kind = HitCard;
        lavaHit.id = "lava";
        activate(g, lavaHit);
        g.m_sideScroll = 0;
        bool swatchesHit = false;
        for (int attempt = 0; attempt < 8 && !swatchesHit; ++attempt) {
            lavaL = build(g, 1100, 720);
            if (lavaL.swatches.size() != 2) return fail("Lava color swatches");
            swatchesHit = true;
            for (const auto& sw : lavaL.swatches) {
                const float hx = sw.box.x + sw.box.w * 0.5f;
                const float hy = sw.box.y + sw.box.h * 0.5f;
                if (!lavaL.side.contains(hx, hy) || hit(lavaL, hx, hy).kind != HitSwatch) swatchesHit = false;
            }
            if (!swatchesHit) g.m_sideScroll += 64.0f;
        }
        if (!swatchesHit) return fail("Lava swatch hit");
        if (lavaL.sliders.empty()) return fail("Lava sliders");
        g.setSlider(0, 0.25f);
        const float expect = 0.1f + 0.25f * (2.5f - 0.1f);
        if (std::fabs(g.m_host->settings().lava.speed - expect) > 0.05f)
            return fail("slider did not change Lava speed");

        g.m_gridScroll = 0;
        Layout scrolled = build(g, 860, 420);
        const float y0 = scrolled.cards.empty() ? 0 : scrolled.cards[0].body.y;
        g.m_gridScroll = 48;
        scrolled = build(g, 860, 420);
        if (!scrolled.cards.empty() && std::fabs((y0 - 48.0f) - scrolled.cards[0].body.y) > 0.5f)
            return fail("grid scroll");
        const float sy0 = scrolled.sliders.empty() ? 0 : scrolled.sliders[0].row.y;
        g.m_sideScroll = 36;
        scrolled = build(g, 860, 420);
        if (!scrolled.sliders.empty() && std::fabs((sy0 - 36.0f) - scrolled.sliders[0].row.y) > 0.5f)
            return fail("settings scroll");

        auto gridMax = [](const Layout& L) {
            return std::max(0.0f, L.gridContent - std::max(0.0f, L.grid.h - kPad * 2.0f));
        };
        auto sideMax = [](const Layout& L) {
            return std::max(0.0f, L.sideContent - L.side.h);
        };
        // Retro is one card, so an 860x420 grid does not overflow. A scrollbar there is a bug.
        if (gridMax(scrolled) < 1.0f && scrolled.gridThumb.h > 1.0f) return fail("grid thumb without overflow");
        if (sideMax(scrolled) < 1.0f && scrolled.sideThumb.h > 1.0f) return fail("side thumb without overflow");

        const auto names = groupNames(g);
        int matrixGroup = -1;
        for (int i = 0; i < (int)names.size(); ++i)
            if (names[(size_t)i] == "Matrix Rain") matrixGroup = i;
        if (matrixGroup < 0) return fail("Matrix Rain group missing");
        Hit mx{};
        mx.kind = HitGroup;
        mx.index = matrixGroup;
        activate(g, mx);
        Hit mxCard{};
        mxCard.kind = HitCard;
        mxCard.id = "matrix";
        activate(g, mxCard);
        g.m_gridScroll = 0;
        g.m_sideScroll = 0;
        Layout overflow = build(g, 900, 360);
        if (gridMax(overflow) < 24.0f || sideMax(overflow) < 24.0f) return fail("expected overflow");
        if (overflow.gridThumb.h < 20.0f || overflow.sideThumb.h < 20.0f) return fail("thumb missing on overflow");
        const float thumbY = overflow.gridThumb.y;
        g.m_gridScroll = std::min(48.0f, gridMax(overflow));
        Layout moved = build(g, 900, 360);
        if (moved.gridThumb.y < thumbY + 1.0f) return fail("thumb did not follow scroll");
        if (!moved.cards.empty()) {
            const auto& card = moved.cards[0];
            const float hy = card.body.y + std::min(24.0f, card.body.h * 0.5f);
            if (moved.grid.contains(card.body.x + 20.0f, hy)) {
                const Hit scrolledHit = hit(moved, card.body.x + card.body.w * 0.5f, hy);
                if (scrolledHit.kind != HitCard || scrolledHit.id != card.id) return fail("scroll hit");
            }
        }

        g.m_gridScroll = 200.0f;
        g.m_sideScroll = 200.0f;
        clampScroll(g, 1600, 4000);
        Layout fit = build(g, 1600, 4000);
        if (gridMax(fit) > 1.0f || sideMax(fit) > 1.0f) return fail("tall window still overflows");
        if (g.m_gridScroll > 0.5f || g.m_sideScroll > 0.5f) return fail("scroll not clamped");
        if (fit.gridThumb.h > 1.0f || fit.sideThumb.h > 1.0f) return fail("scrollbar without overflow");

        if (g.m_hwnd) {
            float dipW = 1, dipH = 1;
            g.clientDipSize(dipW, dipH);
            g.m_gridScroll = 0;
            g.m_sideScroll = 0;
            Layout live = build(g, dipW, dipH);
            RECT before{};
            GetWindowRect(g.m_hwnd, &before);
            const float dpi = g.m_dpi ? (float)g.m_dpi : 96.0f;
            auto sendAt = [&](UINT msg, float dipX, float dipY, WPARAM wp) {
                const int px = (int)std::lround(dipX * dpi / 96.0f);
                const int py = (int)std::lround(dipY * dpi / 96.0f);
                SendMessageW(g.m_hwnd, msg, wp, MAKELPARAM(px, py));
            };
            auto sendWheel = [&](float dipX, float dipY) {
                POINT p{(int)std::lround(dipX * dpi / 96.0f), (int)std::lround(dipY * dpi / 96.0f)};
                ClientToScreen(g.m_hwnd, &p);
                SendMessageW(g.m_hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)-WHEEL_DELTA), MAKELPARAM(p.x, p.y));
            };
            if (sideMax(live) >= 1.0f) {
                if (live.sideThumb.h < 20.0f) return fail("live side thumb missing");
                const float x = live.sideThumb.x + live.sideThumb.w * 0.5f;
                const float y = live.sideThumb.y + 4.0f;
                sendAt(WM_LBUTTONDOWN, x, y, MK_LBUTTON);
                sendAt(WM_MOUSEMOVE, x, y + 36.0f, MK_LBUTTON);
                sendAt(WM_LBUTTONUP, x, y + 36.0f, 0);
                if (g.m_sideScroll < 4.0f) return fail("thumb drag did not scroll");
                g.m_sideScroll = 0;
                sendWheel(live.side.x + 24.0f, live.side.y + std::min(80.0f, live.side.h * 0.5f));
                if (g.m_sideScroll < 4.0f) return fail("wheel did not scroll settings");
            } else {
                if (live.sideThumb.h > 1.0f) return fail("live side thumb without overflow");
                sendWheel(live.side.x + 24.0f, live.side.y + std::min(80.0f, live.side.h * 0.5f));
                if (std::fabs(g.m_sideScroll) > 0.5f) return fail("wheel scrolled a fitted settings pane");
            }
            g.m_gridScroll = 0;
            live = build(g, dipW, dipH);
            if (gridMax(live) >= 1.0f) {
                if (live.gridThumb.h < 20.0f) return fail("live grid thumb missing");
                const float beforeScroll = g.m_gridScroll;
                sendWheel(live.grid.x + live.grid.w * 0.4f, live.grid.y + 40.0f);
                if (g.m_gridScroll <= beforeScroll + 1.0f) return fail("wheel did not scroll");
            } else {
                const float beforeScroll = g.m_gridScroll;
                sendWheel(live.grid.x + 40.0f, live.grid.y + 40.0f);
                if (std::fabs(g.m_gridScroll - beforeScroll) > 0.5f) return fail("wheel scrolled a fitted grid");
                if (live.gridThumb.h > 1.0f) return fail("live grid thumb without overflow");
            }
            RECT after{};
            GetWindowRect(g.m_hwnd, &after);
            if (after.left != before.left || after.top != before.top) return fail("scroll moved the window");
        }

        if (cardsHit < 6) return fail("only hit " + std::to_string(cardsHit) + " cards");
        report = "GalleryUI: PASS groups=4 cards=6 sizes=4\n";
        return true;
    }
};

float WallpaperGallery::pxToDip(float px) const {
    const float dpi = m_dpi ? (float)m_dpi : 96.0f;
    return px * 96.0f / dpi;
}

void WallpaperGallery::clientDipSize(float& w, float& h) const {
    RECT rc{};
    if (m_hwnd) GetClientRect(m_hwnd, &rc);
    w = std::max(1.0f, pxToDip((float)rc.right));
    h = std::max(1.0f, pxToDip((float)rc.bottom));
}

bool WallpaperGallery::acceptsThumbs() const {
    return m_visible && m_hwnd && IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd);
}

bool WallpaperGallery::runUiSelfTest(std::string& report) { return GalleryUi::selfTest(*this, report); }

void WallpaperGallery::syncSelectionToEffect() {
    if (!m_host) return;
    m_panelId = m_host->settings().effect;
    m_group = 0;
    m_gridScroll = 0;
    m_sideScroll = 0;
    const auto* info = m_host->registry().find(m_panelId);
    if (!info) return;
    const auto names = GalleryUi::groupNames(*this);
    for (int i = 0; i < (int)names.size(); ++i) {
        if (names[(size_t)i] == info->group) {
            m_group = i;
            break;
        }
    }
}

void WallpaperGallery::setSlider(int index, float t) {
    if (!m_host || m_panelId.empty() || index < 0) return;
    m_preset = -1;
    setKnob(m_host->settings(), m_panelId, index, t);
    m_host->applySettingsLive();
}

void WallpaperGallery::toggleAt(int index) {
    if (!m_host || m_panelId.empty()) return;
    m_preset = -1;
    const bool on = knobToggle(m_host->settings(), m_panelId, index);
    setKnobToggle(m_host->settings(), m_panelId, index, !on);
    m_host->applySettingsLive();
    refresh();
}

void WallpaperGallery::pickColor(int which) {
    if (!m_host || m_panelId.empty()) return;
    m_preset = -1;
    const uint32_t current = knobColor(m_host->settings(), m_panelId, which);
    CHOOSECOLORW cc{};
    static COLORREF custom[16]{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = m_hwnd;
    cc.rgbResult = RGB((current >> 16) & 255, (current >> 8) & 255, current & 255);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    const uint32_t rgb = ((GetRValue(cc.rgbResult) & 255u) << 16) | ((GetGValue(cc.rgbResult) & 255u) << 8) |
                         (GetBValue(cc.rgbResult) & 255u);
    setKnobColor(m_host->settings(), m_panelId, which, rgb);
    m_host->applySettingsLive();
    refresh();
}

void WallpaperGallery::pickImage() {
    if (!m_host || (m_panelId != "ape" && m_panelId != "myco")) return;
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.PNG;*.JPG;*.JPEG\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrTitle = L"Choose a picture for the rain reveal";
    if (!GetOpenFileNameW(&ofn)) return;
    const std::string path = narrow(file);
    if (m_panelId == "ape") m_host->settings().ape.imagePath = path;
    else m_host->settings().myco.imagePath = path;
    m_host->applySettingsLive();
    refresh();
}

void WallpaperGallery::editGlyphs() {
    if (!m_host || m_panelId != "matrix") return;
    struct State {
        std::wstring text;
        bool ok = false;
        HWND edit = nullptr;
        HFONT font = nullptr;
    } state;
    state.text = widen(m_host->settings().matrix.customGlyphs);
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_CREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<State*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
            UINT dpi = 96;
            using GetDpi = UINT(WINAPI*)(HWND);
            auto getDpi = (GetDpi)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
            if (getDpi) dpi = getDpi(hwnd);
            if (dpi < 96) dpi = 96;
            const float s = (float)dpi / 96.0f;
            const int pad = (int)std::lround(ui::kPad * s);
            const int editH = (int)std::lround(ui::kBtnH * s);
            const int gap = (int)std::lround(ui::kGap * s);
            RECT client{};
            GetClientRect(hwnd, &client);
            const int inner = std::max(40, (int)client.right - pad * 2);
            const int btnW = (int)std::lround(96 * s);
            st->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->text.c_str(),
                                       WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, pad, pad, inner, editH, hwnd, (HMENU)1,
                                       nullptr, nullptr);
            HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                      (int)client.right - pad - btnW, pad + editH + gap, btnW, editH, hwnd, (HMENU)2,
                                      nullptr, nullptr);
            st->font = CreateFontW(-(int)(14 * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI");
            if (st->font) {
                if (st->edit) SendMessageW(st->edit, WM_SETFONT, (WPARAM)st->font, TRUE);
                if (ok) SendMessageW(ok, WM_SETFONT, (WPARAM)st->font, TRUE);
            }
            return 0;
        }
        if (msg == WM_DESTROY && st && st->font) {
            DeleteObject(st->font);
            st->font = nullptr;
            return 0;
        }
        if (msg == WM_COMMAND && LOWORD(wParam) == 2 && st && st->edit) {
            wchar_t buf[96]{};
            GetWindowTextW(st->edit, buf, 96);
            st->text = buf;
            st->ok = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    };
    wc.hInstance = m_instance ? m_instance : GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"StalgiaAsk";
    RegisterClassW(&wc);
    RECT wr{};
    GetWindowRect(m_hwnd, &wr);
    UINT dpi = 96;
    using GetDpi = UINT(WINAPI*)(HWND);
    auto getDpi = (GetDpi)(void*)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (getDpi && m_hwnd) dpi = getDpi(m_hwnd);
    if (dpi < 96) dpi = 96;
    const float sc = (float)dpi / 96.0f;
    RECT outer{0, 0, (LONG)std::lround(420 * sc),
               (LONG)std::lround((ui::kPad + ui::kBtnH + ui::kGap + ui::kBtnH + ui::kPad) * sc)};
    AdjustWindowRectEx(&outer, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    HWND pop = CreateWindowExW(WS_EX_DLGMODALFRAME, L"StalgiaAsk", L"Custom glyphs",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU, wr.left + (int)(80 * sc), wr.top + (int)(80 * sc),
                               outer.right - outer.left, outer.bottom - outer.top, m_hwnd, nullptr, wc.hInstance,
                               &state);
    if (!pop) return;
    EnableWindow(m_hwnd, FALSE);
    ShowWindow(pop, SW_SHOW);
    MSG msg{};
    while (IsWindow(pop) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if (!IsDialogMessageW(pop, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(m_hwnd, TRUE);
    SetForegroundWindow(m_hwnd);
    if (!state.ok) return;
    m_host->settings().matrix.customGlyphs = narrow(state.text);
    m_host->settings().matrix.glyphSet = 3;
    m_host->applySettingsLive();
    refresh();
}

bool WallpaperGallery::initFactories() {
    if (m_d2d && m_dwrite) return true;
    ID2D1Factory* d2d = nullptr;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_ID2D1Factory, nullptr,
                                 reinterpret_cast<void**>(&d2d))) ||
        !d2d)
        return false;
    IDWriteFactory* dw = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, IID_IDWriteFactory,
                                   reinterpret_cast<IUnknown**>(&dw))) ||
        !dw) {
        d2d->Release();
        return false;
    }
    m_d2d = d2d;
    m_dwrite = dw;
    return true;
}

void WallpaperGallery::releaseFactories() {
    if (m_hwndRt) {
        static_cast<IUnknown*>(m_hwndRt)->Release();
        m_hwndRt = nullptr;
    }
    if (m_dwrite) {
        static_cast<IUnknown*>(m_dwrite)->Release();
        m_dwrite = nullptr;
    }
    if (m_d2d) {
        static_cast<IUnknown*>(m_d2d)->Release();
        m_d2d = nullptr;
    }
}

bool WallpaperGallery::show(HINSTANCE instance, SettingsPanel::Host* host, bool offscreen) {
    m_instance = instance;
    m_host = host;
    if (!initFactories()) return false;
    if (!m_classReg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = wndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hIcon = loadAppIcon(instance, largeIconPx(nullptr));
        wc.hIconSm = loadAppIcon(instance, smallIconPx(nullptr));
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        m_classReg = true;
    }
    if (m_hwnd && m_offscreen != offscreen) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
        if (m_hwndRt) {
            static_cast<IUnknown*>(m_hwndRt)->Release();
            m_hwndRt = nullptr;
        }
    }
    m_offscreen = offscreen;
    if (!m_hwnd) {
        const DWORD ex = offscreen ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) : 0;
        const DWORD style = offscreen ? (WS_POPUP | WS_VISIBLE) : WS_OVERLAPPEDWINDOW;
        const int x = offscreen ? -24000 : CW_USEDEFAULT;
        const int y = offscreen ? -24000 : CW_USEDEFAULT;
        // CreateWindow takes physical pixels. 1100x720 is the client size in DIPs, so 150%
        // stays the same layout instead of opening a shrunken window with no overflow.
        m_dpi = galleryDpi(nullptr);
        auto outerSize = [&](UINT dpi, int& outW, int& outH) {
            const float scale = std::max(1.0f, (dpi ? (float)dpi : 96.0f) / 96.0f);
            RECT rc{0, 0, (LONG)std::ceil(1100.0f * scale), (LONG)std::ceil(720.0f * scale)};
            AdjustWindowRectEx(&rc, style, FALSE, ex);
            outW = std::max(320, (int)(rc.right - rc.left));
            outH = std::max(240, (int)(rc.bottom - rc.top));
            RECT work{};
            if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
                outW = std::min(outW, std::max(320, (int)(work.right - work.left) - 16));
                outH = std::min(outH, std::max(240, (int)(work.bottom - work.top) - 16));
            }
        };
        int wantW = 1100, wantH = 720;
        const UINT sizedDpi = m_dpi ? m_dpi : 96;
        outerSize(sizedDpi, wantW, wantH);
        m_hwnd = CreateWindowExW(ex, kClass, L"Stalgia Gallery", style, x, y, wantW, wantH, nullptr, nullptr,
                                 instance, this);
        if (!m_hwnd) {
            LOG_ERR("CreateWindow gallery failed: %lu", GetLastError());
            return false;
        }
        BOOL dark = TRUE;
        DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DWORD corners = DWMWCP_ROUND;
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
        const UINT createdDpi = galleryDpi(m_hwnd);
        if (createdDpi) m_dpi = createdDpi;
        if (m_dpi && m_dpi != sizedDpi) {
            outerSize(m_dpi, wantW, wantH);
            SetWindowPos(m_hwnd, nullptr, 0, 0, wantW, wantH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    syncSelectionToEffect();
    m_visible = true;
    ShowWindow(m_hwnd, offscreen ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (!offscreen) SetForegroundWindow(m_hwnd);
    InvalidateRect(m_hwnd, nullptr, FALSE);
    LOG_INFO("Gallery %s", offscreen ? "opened off-screen" : "opened");
    return true;
}

void WallpaperGallery::hide() {
    m_visible = false;
    m_dragSlider = -1;
    if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
    LOG_INFO("Gallery hidden");
}

void WallpaperGallery::refresh() {
    if (m_hwnd && m_visible) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void WallpaperGallery::destroy() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    releaseFactories();
    m_visible = false;
    m_host = nullptr;
}

LRESULT CALLBACK WallpaperGallery::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WallpaperGallery* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<WallpaperGallery*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        if (self) self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<WallpaperGallery*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->handle(hwnd, msg, wParam, lParam);
}

LRESULT WallpaperGallery::handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CLOSE:
            hide();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                hide();
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            m_dpi = HIWORD(wParam);
            if (!m_dpi) m_dpi = 96;
            auto* r = reinterpret_cast<RECT*>(lParam);
            if (r) SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                                SWP_NOZORDER | SWP_NOACTIVATE);
            if (m_hwndRt) static_cast<ID2D1HwndRenderTarget*>(m_hwndRt)->SetDpi((float)m_dpi, (float)m_dpi);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            if (m_hwndRt) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                static_cast<ID2D1HwndRenderTarget*>(m_hwndRt)->Resize(
                    D2D1::SizeU((UINT)std::max(1L, rc.right), (UINT)std::max(1L, rc.bottom)));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
            const float scale = (m_dpi ? (float)m_dpi : 96.0f) / 96.0f;
            mm->ptMinTrackSize.x = (LONG)std::ceil(840.0f * scale);
            mm->ptMinTrackSize.y = (LONG)std::ceil(560.0f * scale);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, m_hotKind ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE:
            m_trackLeave = false;
            m_hotKind = 0;
            m_hotIndex = -1;
            m_hotId.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            float dipW = 1, dipH = 1;
            clientDipSize(dipW, dipH);
            const float x = pxToDip((float)pt.x);
            const float y = pxToDip((float)pt.y);
            const auto L = GalleryUi::build(*this, dipW, dipH);
            const float step = -((float)GET_WHEEL_DELTA_WPARAM(wParam) / 120.0f) * 72.0f;
            if (L.side.contains(x, y)) m_sideScroll += step;
            else m_gridScroll += step;
            GalleryUi::clampScroll(*this, dipW, dipH);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            float dipW = 1, dipH = 1;
            clientDipSize(dipW, dipH);
            const float x = pxToDip((float)GET_X_LPARAM(lParam));
            const float y = pxToDip((float)GET_Y_LPARAM(lParam));
            if (m_dragScroll) {
                m_dragScroll = 0;
                m_downKind = 0;
                m_downIndex = -1;
                m_downId.clear();
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (m_dragSlider >= 0) {
                m_dragSlider = -1;
                m_downKind = 0;
                m_downIndex = -1;
                m_downId.clear();
                ReleaseCapture();
                if (m_host) m_host->flushSettings();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const auto L = GalleryUi::build(*this, dipW, dipH);
            const GalleryUi::Hit cur = GalleryUi::hit(L, x, y);
            if (cur.kind == m_downKind && cur.index == m_downIndex && cur.id == m_downId && cur.kind != GalleryUi::HitNone)
                GalleryUi::activate(*this, cur);
            m_downKind = 0;
            m_downIndex = -1;
            m_downId.clear();
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CAPTURECHANGED:
            m_dragScroll = 0;
            if (m_dragSlider >= 0) {
                m_dragSlider = -1;
                if (m_host) m_host->flushSettings();
            }
            m_downKind = 0;
            m_downIndex = -1;
            m_downId.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        default:
            break;
    }

    if ((msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE) && m_host) {
        if (msg == WM_MOUSEMOVE && !m_trackLeave) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            m_trackLeave = true;
        }
        float dipW = 1, dipH = 1;
        clientDipSize(dipW, dipH);
        const float x = pxToDip((float)GET_X_LPARAM(lParam));
        const float y = pxToDip((float)GET_Y_LPARAM(lParam));
        const auto L = GalleryUi::build(*this, dipW, dipH);
        if (m_dragScroll && msg == WM_MOUSEMOVE) {
            const bool grid = m_dragScroll == 1;
            const float maxS = grid ? std::max(0.0f, L.gridContent - std::max(0.0f, L.grid.h - 32.0f))
                                    : std::max(0.0f, L.sideContent - L.side.h);
            const auto& view = grid ? L.grid : L.side;
            const float th = grid ? L.gridThumb.h : L.sideThumb.h;
            const float travel = std::max(1.0f, view.h - ui::kGap * 2.0f - th);
            const float t = std::clamp((y - m_scrollGrab - (view.y + ui::kGap)) / travel, 0.0f, 1.0f);
            if (grid) m_gridScroll = t * maxS;
            else m_sideScroll = t * maxS;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (m_dragSlider >= 0 && (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN)) {
            for (const auto& s : L.sliders) {
                if (s.index == m_dragSlider && s.track.w > 1.0f) {
                    setSlider(s.index, (x - s.track.x) / s.track.w);
                    break;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        const GalleryUi::Hit h = GalleryUi::hit(L, x, y);
        if (msg == WM_LBUTTONDOWN && h.kind == GalleryUi::HitScroll) {
            m_dragScroll = h.index + 1;
            const auto& thumb = h.index == 0 ? L.gridThumb : L.sideThumb;
            m_scrollGrab = y - thumb.y;
            m_downKind = h.kind;
            m_downIndex = h.index;
            m_downId.clear();
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (msg == WM_MOUSEMOVE) {
            if (h.kind != m_hotKind || h.index != m_hotIndex || h.id != m_hotId) {
                m_hotKind = h.kind;
                m_hotIndex = h.index;
                m_hotId = h.id;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        m_downKind = h.kind;
        m_downIndex = h.index;
        m_downId = h.id;
        m_hotKind = h.kind;
        m_hotIndex = h.index;
        m_hotId = h.id;
        if (h.kind == GalleryUi::HitSlider) {
            m_dragSlider = h.index;
            for (const auto& s : L.sliders) {
                const bool onTrack = y >= s.track.y - 6.0f && y <= s.track.y + s.track.h + 8.0f;
                if (s.index == h.index && s.track.w > 1.0f && onTrack)
                    setSlider(s.index, (x - s.track.x) / s.track.w);
            }
        }
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

namespace {

void fillRound(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const GalleryUi::Rect& r, float rad) {
    D2D1_ROUNDED_RECT rr{r.box(), rad, rad};
    rt->FillRoundedRectangle(rr, br);
}

void strokeRound(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, const GalleryUi::Rect& r, float rad, float stroke) {
    D2D1_ROUNDED_RECT rr{r.box(), rad, rad};
    rt->DrawRoundedRectangle(rr, br, stroke);
}

void drawStar(ID2D1Factory* d2d, ID2D1RenderTarget* rt, ID2D1Brush* br, float cx, float cy, float r, bool fill) {
    if (!d2d) return;
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(d2d->CreatePathGeometry(geo.put())) || FAILED(geo->Open(sink.put()))) return;
    const float pi = 3.14159265f;
    for (int i = 0; i < 10; ++i) {
        const float ang = -pi / 2.0f + (float)i * (pi / 5.0f);
        const float rad = (i % 2 == 0) ? r : r * 0.40f;
        const D2D1_POINT_2F p = D2D1::Point2F(cx + std::cos(ang) * rad, cy + std::sin(ang) * rad);
        if (i == 0) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
        else sink->AddLine(p);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    if (fill) rt->FillGeometry(geo.get(), br);
    else rt->DrawGeometry(geo.get(), br, 1.4f);
}

}  // namespace

void WallpaperGallery::paint() {
    PAINTSTRUCT ps{};
    BeginPaint(m_hwnd, &ps);
    if (!m_d2d || !m_host) {
        EndPaint(m_hwnd, &ps);
        return;
    }
    RECT crc{};
    GetClientRect(m_hwnd, &crc);
    auto* d2d = static_cast<ID2D1Factory*>(m_d2d);
    auto* dw = static_cast<IDWriteFactory*>(m_dwrite);
    if (!m_hwndRt) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)m_dpi,
            (float)m_dpi);
        D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
            m_hwnd, D2D1::SizeU((UINT)std::max(1L, crc.right), (UINT)std::max(1L, crc.bottom)));
        ID2D1HwndRenderTarget* rt = nullptr;
        if (FAILED(d2d->CreateHwndRenderTarget(props, hp, &rt)) || !rt) {
            EndPaint(m_hwnd, &ps);
            return;
        }
        m_hwndRt = rt;
    }
    auto* rt = static_cast<ID2D1HwndRenderTarget*>(m_hwndRt);
    rt->SetDpi((float)m_dpi, (float)m_dpi);
    const D2D1_SIZE_F rtSize = rt->GetSize();
    float dipW = rtSize.width > 1.0f ? rtSize.width : pxToDip((float)crc.right);
    float dipH = rtSize.height > 1.0f ? rtSize.height : pxToDip((float)crc.bottom);
    GalleryUi::clampScroll(*this, dipW, dipH);
    const GalleryUi::Layout L = GalleryUi::build(*this, dipW, dipH);

    ComPtr<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), br.put());
    ComPtr<IDWriteTextFormat> title, body, tiny;
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", title.put());
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", body.put());
    dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", tiny.put());
    if (body) body->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    auto text = [&](IDWriteTextFormat* fmt, const GalleryUi::Rect& r, const wchar_t* s, D2D1_COLOR_F c,
                    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                    DWRITE_PARAGRAPH_ALIGNMENT para = DWRITE_PARAGRAPH_ALIGNMENT_NEAR) {
        if (!fmt || !s) return;
        br->SetColor(c);
        fmt->SetTextAlignment(align);
        fmt->SetParagraphAlignment(para);
        rt->DrawTextW(s, (UINT32)wcslen(s), fmt, r.box(), br.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    auto hot = [&](int kind, int index, const std::string& id) {
        return m_hotKind == kind && m_hotIndex == index && m_hotId == id;
    };
    auto down = [&](int kind, int index, const std::string& id) {
        return m_downKind == kind && m_downIndex == index && m_downId == id && hot(kind, index, id);
    };

    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    br->SetColor(D2D1::ColorF(0.07f, 0.08f, 0.10f, 1));
    rt->FillRectangle(D2D1::RectF(0, 0, dipW, dipH), br.get());
    br->SetColor(D2D1::ColorF(0.09f, 0.10f, 0.13f, 1));
    rt->FillRectangle(L.rail.box(), br.get());
    br->SetColor(D2D1::ColorF(0.10f, 0.11f, 0.14f, 1));
    rt->FillRectangle(L.side.box(), br.get());

    text(title.get(), GalleryUi::Rect{16, 16, kRail - 28, 28}, L"Wallpapers", D2D1::ColorF(0.93f, 0.95f, 0.97f, 1));
    for (int i = 0; i < (int)L.groups.size(); ++i) {
        const auto& row = L.groups[(size_t)i];
        const bool on = i == m_group;
        const bool hover = hot(GalleryUi::HitGroup, i, {});
        const bool pressed = down(GalleryUi::HitGroup, i, {});
        D2D1_COLOR_F fill = on ? D2D1::ColorF(0.16f, 0.42f, 0.28f, 1) : D2D1::ColorF(1, 1, 1, 0.04f);
        if (hover) fill.a = std::min(1.0f, fill.a + 0.10f);
        if (pressed) fill = on ? D2D1::ColorF(0.22f, 0.55f, 0.36f, 1) : D2D1::ColorF(1, 1, 1, 0.16f);
        br->SetColor(fill);
        fillRound(rt, br.get(), row.row, 8);
        text(body.get(), row.row, row.name.c_str(), D2D1::ColorF(1, 1, 1, on ? 0.96f : 0.78f),
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    rt->PushAxisAlignedClip(L.grid.box(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (L.emptyGroup) {
        const wchar_t* msg = (m_group == 0) ? L"Star a wallpaper to keep it here." : L"Nothing in this group.";
        text(body.get(), GalleryUi::Rect{L.grid.x + 24, L.grid.y + 28, std::max(40.0f, L.grid.w - 48), 40}, msg,
             D2D1::ColorF(1, 1, 1, 0.62f));
    }
    ThumbnailStage* thumbs = m_host->thumbs();
    const std::string& playing = m_host->settings().effect;
    for (int i = 0; i < (int)L.cards.size(); ++i) {
        const auto& c = L.cards[(size_t)i];
        const bool selected = c.id == m_panelId;
        const bool hover = hot(GalleryUi::HitCard, i, c.id);
        const bool pressed = down(GalleryUi::HitCard, i, c.id);
        br->SetColor(pressed ? D2D1::ColorF(0.20f, 0.22f, 0.26f, 1)
                             : (hover ? D2D1::ColorF(0.17f, 0.18f, 0.22f, 1) : D2D1::ColorF(0.14f, 0.15f, 0.18f, 1)));
        fillRound(rt, br.get(), c.body, 12);
        br->SetColor(selected ? D2D1::ColorF(0.35f, 0.95f, 0.55f, pressed ? 1.0f : 0.95f)
                              : D2D1::ColorF(1, 1, 1, hover ? 0.28f : 0.08f));
        strokeRound(rt, br.get(), c.body, 12, selected ? 2.0f : 1.0f);
        br->SetColor(D2D1::ColorF(0.02f, 0.03f, 0.05f, 1));
        fillRound(rt, br.get(), c.thumb, 8);
        if (thumbs) {
            const uint8_t* px = thumbs->pixels(c.id);
            if (px) {
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                ComPtr<ID2D1Bitmap> bmp;
                if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(ThumbnailStage::kWidth, ThumbnailStage::kHeight), px,
                                               ThumbnailStage::kWidth * 4, bp, bmp.put()))) {
                    rt->DrawBitmap(bmp.get(), c.thumb.box(), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
            }
        }
        const bool starHot = hot(GalleryUi::HitStar, i, c.id);
        const bool starDown = down(GalleryUi::HitStar, i, c.id);
        br->SetColor(c.favorite ? D2D1::ColorF(0.98f, 0.78f, 0.22f, starDown ? 1.0f : 0.95f)
                                : D2D1::ColorF(1, 1, 1, starHot ? 0.85f : 0.55f));
        drawStar(d2d, rt, br.get(), c.star.x + c.star.w * 0.5f, c.star.y + c.star.h * 0.5f,
                 starHot ? 9.0f : 8.0f, c.favorite || starHot);
        text(body.get(), GalleryUi::Rect{c.body.x + 12, c.thumb.y + c.thumb.h + 6, c.body.w - 24, 20}, c.name.c_str(),
             D2D1::ColorF(0.95f, 0.96f, 0.98f, 1));
        text(tiny.get(), GalleryUi::Rect{c.body.x + 12, c.thumb.y + c.thumb.h + 26, c.body.w - 24, 28}, c.blurb.c_str(),
             D2D1::ColorF(1, 1, 1, 0.58f));
        if (c.id == playing) {
            text(tiny.get(), GalleryUi::Rect{c.body.x + 12, c.body.y + 12, 70, 16}, L"Playing",
                 D2D1::ColorF(0.75f, 1.0f, 0.85f, 0.9f));
        }
    }
    if (L.gridThumb.h > 1.0f) {
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));
        fillRound(rt, br.get(), L.gridThumb, 3);
    }
    rt->PopAxisAlignedClip();

    rt->PushAxisAlignedClip(L.side.box(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (L.panelId.empty()) {
        text(body.get(), GalleryUi::Rect{L.side.x + kPad, L.side.y + 24, L.side.w - kPad * 2, 48},
             L"Select a wallpaper.", D2D1::ColorF(1, 1, 1, 0.6f));
    } else {
        text(title.get(), L.titleRect, L.title.c_str(), D2D1::ColorF(0.95f, 0.96f, 0.98f, 1),
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!L.desc.empty())
            text(body.get(), L.descRect, L.desc.c_str(), D2D1::ColorF(1, 1, 1, 0.68f));
        for (int i = 0; i < (int)L.chips.size(); ++i) {
            const auto& chip = L.chips[(size_t)i];
            const bool reset = chip.preset < 0;
            const bool on = chip.preset == m_preset;
            const bool hover = hot(GalleryUi::HitChip, chip.preset, {});
            const bool pressed = down(GalleryUi::HitChip, chip.preset, {});
            br->SetColor(reset ? D2D1::ColorF(1, 1, 1, pressed ? 0.10f : 0.04f)
                               : (on ? D2D1::ColorF(0.16f, 0.42f, 0.28f, 1)
                                     : D2D1::ColorF(1, 1, 1, pressed ? 0.18f : (hover ? 0.12f : 0.06f))));
            fillRound(rt, br.get(), chip.box, 8);
            if (reset) {
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.40f));
                strokeRound(rt, br.get(), chip.box, 8, 1.0f);
            }
            text(tiny.get(), chip.box, chip.label.c_str(), D2D1::ColorF(1, 1, 1, reset ? 0.78f : (on ? 0.98f : 0.88f)),
                 DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        for (const auto& hd : L.headers) {
            text(tiny.get(), hd.row, hd.label, D2D1::ColorF(0.55f, 0.95f, 0.70f, 0.95f),
                 DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        for (const auto& s : L.sliders) {
            const bool hover = hot(GalleryUi::HitSlider, s.index, {});
            text(tiny.get(), s.labelBox, s.label, D2D1::ColorF(1, 1, 1, 0.84f), DWRITE_TEXT_ALIGNMENT_LEADING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            text(tiny.get(), s.valueBox, s.value.c_str(), D2D1::ColorF(1, 1, 1, 0.62f), DWRITE_TEXT_ALIGNMENT_TRAILING,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            br->SetColor(D2D1::ColorF(1, 1, 1, hover ? 0.16f : 0.10f));
            fillRound(rt, br.get(), s.track, 4);
            br->SetColor(D2D1::ColorF(0.35f, 0.90f, 0.55f, 0.95f));
            fillRound(rt, br.get(), GalleryUi::Rect{s.track.x, s.track.y, std::max(8.0f, s.track.w * s.t), s.track.h},
                      4);
            const float kx = s.track.x + s.track.w * s.t;
            br->SetColor(D2D1::ColorF(0.92f, 0.98f, 0.94f, 1));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, s.track.y + s.track.h * 0.5f), hover ? 7.0f : 6.0f,
                                          hover ? 7.0f : 6.0f),
                            br.get());
        }
        for (const auto& sw : L.swatches) {
            const bool hover = hot(GalleryUi::HitSwatch, sw.which, {});
            const bool pressed = down(GalleryUi::HitSwatch, sw.which, {});
            text(tiny.get(), GalleryUi::Rect{sw.row.x, sw.row.y, sw.box.x - sw.row.x - ui::kGap, sw.row.h}, sw.label,
                 D2D1::ColorF(1, 1, 1, 0.84f), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            br->SetColor(rgb(sw.rgb, 1));
            fillRound(rt, br.get(), sw.box, 4);
            br->SetColor(D2D1::ColorF(1, 1, 1, pressed ? 0.95f : (hover ? 0.75f : 0.4f)));
            strokeRound(rt, br.get(), sw.box, 4, hover ? 1.6f : 1.0f);
        }
        for (const auto& t : L.toggles) {
            const bool hover = hot(GalleryUi::HitToggle, t.index, {});
            const bool pressed = down(GalleryUi::HitToggle, t.index, {});
            GalleryUi::Rect box{t.row.x, t.row.y + (t.row.h - 18.0f) * 0.5f, 18, 18};
            br->SetColor(t.on ? D2D1::ColorF(0.35f, 0.90f, 0.55f, 1) : D2D1::ColorF(1, 1, 1, hover ? 0.20f : 0.12f));
            if (pressed) br->SetColor(D2D1::ColorF(0.55f, 1.0f, 0.70f, 1));
            fillRound(rt, br.get(), box, 4);
            text(tiny.get(), GalleryUi::Rect{t.row.x + 28, t.row.y, t.row.w - 36, t.row.h}, t.label,
                 D2D1::ColorF(1, 1, 1, 0.84f), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        for (const auto& b : L.buttons) {
            const bool hover = hot(GalleryUi::HitButton, b.index, {});
            const bool pressed = down(GalleryUi::HitButton, b.index, {});
            br->SetColor(pressed ? D2D1::ColorF(0.22f, 0.55f, 0.36f, 1)
                                 : D2D1::ColorF(1, 1, 1, hover ? 0.16f : 0.08f));
            fillRound(rt, br.get(), b.box, 7);
            text(tiny.get(), b.box, b.label.c_str(), D2D1::ColorF(1, 1, 1, 0.92f), DWRITE_TEXT_ALIGNMENT_CENTER,
                 DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    if (L.sideThumb.h > 1.0f) {
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));
        fillRound(rt, br.get(), L.sideThumb, 3);
    }
    rt->PopAxisAlignedClip();
    const HRESULT endHr = rt->EndDraw();
    if (endHr == (HRESULT)D2DERR_RECREATE_TARGET && m_hwndRt) {
        static_cast<IUnknown*>(m_hwndRt)->Release();
        m_hwndRt = nullptr;
    }
    EndPaint(m_hwnd, &ps);
}
