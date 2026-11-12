#pragma once

// One spacing system for every Stalgia surface. All values are DIPs.
// A 4-unit grid: padding 16, gaps 8, a 24-wide scrollbar gutter that content
// never enters. Callers scale to physical pixels with the window DPI.

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

namespace ui {

constexpr float kUnit = 4.f;
constexpr float kPad = 16.f;
constexpr float kGap = 8.f;
constexpr float kSectionGap = 16.f;
constexpr float kHeaderH = 20.f;
constexpr float kHeaderGap = 8.f;
constexpr float kRowH = 32.f;
constexpr float kRowGap = 8.f;
constexpr float kChipH = 28.f;
constexpr float kChipGap = 8.f;
constexpr float kChipPadX = 12.f;
constexpr float kBtnH = 32.f;
constexpr float kGutter = 24.f;
constexpr float kThumbW = 6.f;
constexpr float kBottom = 16.f;
constexpr float kValueMin = 56.f;
constexpr float kTrackH = 6.f;
constexpr float kMinTrack = 64.f;
constexpr float kMinLabel = 72.f;

inline float textWidth(const wchar_t* s, float fontPx) {
    if (!s || !s[0] || fontPx <= 0.f) return 0.f;
    float w = 0.f;
    for (const wchar_t* p = s; *p; ++p) {
        const wchar_t c = *p;
        float em = 0.56f;
        if (c == L' ') em = 0.33f;
        else if (c == L'i' || c == L'l' || c == L'I' || c == L'j' || c == L'.' || c == L',' || c == L'\'' ||
                 c == L'!' || c == L'|')
            em = 0.28f;
        else if (c == L'm' || c == L'w' || c == L'M' || c == L'W' || c == L'%' || c == L'@')
            em = 0.92f;
        else if ((c >= L'A' && c <= L'Z') || c == L'\u00d7')
            em = 0.70f;
        else if (c >= L'0' && c <= L'9')
            em = 0.58f;
        w += em * fontPx;
    }
    return w;
}

inline float chipWidth(const wchar_t* label, float fontPx, float maxW) {
    const float need = std::ceil(textWidth(label, fontPx) + kChipPadX * 2.f);
    const float cap = std::max(44.f, maxW);
    return std::min(cap, std::max(44.f, need));
}

struct Columns {
    float label = 96.f;
    float value = 56.f;
    float track = 80.f;
};

// Label and value columns from the measured text. The slider takes whatever
// remains and stays at least kMinTrack when the pane is wide enough.
inline Columns columnsFor(float contentW, float labelNeed, float valueNeed) {
    Columns c;
    c.value = std::max(kValueMin, valueNeed + 8.f);
    c.label = std::max(kMinLabel, labelNeed + 4.f);
    float track = contentW - c.label - c.value - kGap * 2.f;
    if (track < kMinTrack) {
        const float spare = std::max(0.f, c.label - kMinLabel);
        const float cut = std::min(spare, kMinTrack - track);
        c.label -= cut;
        track = contentW - c.label - c.value - kGap * 2.f;
    }
    c.track = std::max(24.f, track);
    return c;
}

inline float contentWidth(float panelW) { return std::max(80.f, panelW - kPad - kGutter); }

inline float thumbX(float panelX, float panelW) {
    return panelX + panelW - kGutter + (kGutter - kThumbW) * 0.5f;
}

struct R {
    float x = 0, y = 0, w = 0, h = 0;
};

inline bool live(const R& r) { return r.w > 1.f && r.h > 1.f; }

inline bool intersects(const R& a, const R& b) {
    if (!live(a) || !live(b)) return false;
    return a.x < b.x + b.w - 0.5f && b.x < a.x + a.w - 0.5f && a.y < b.y + b.h - 0.5f && b.y < a.y + a.h - 0.5f;
}

inline bool inside(const R& r, const R& bounds) {
    if (!live(r)) return true;
    return r.x >= bounds.x - 0.5f && r.y >= bounds.y - 0.5f && r.x + r.w <= bounds.x + bounds.w + 0.5f &&
           r.y + r.h <= bounds.y + bounds.h + 0.5f;
}

inline bool overlapsX(const R& r, const R& gutter) {
    if (!live(r) || gutter.w <= 0.f) return false;
    return r.x < gutter.x + gutter.w - 0.5f && gutter.x < r.x + r.w - 0.5f;
}

// controls sit in `content` (already inset from the panel). `gutter` is the
// scrollbar strip. Returns a short reason, or nullptr when the layout is clean.
inline const char* audit(const R* items, int n, const R& content, const R& gutter) {
    for (int i = 0; i < n; ++i) {
        if (!live(items[i])) continue;
        if (!inside(items[i], content)) return "outside content";
        if (overlapsX(items[i], gutter)) return "overlaps gutter";
        for (int j = i + 1; j < n; ++j) {
            if (intersects(items[i], items[j])) return "controls overlap";
        }
    }
    return nullptr;
}

}  // namespace ui
