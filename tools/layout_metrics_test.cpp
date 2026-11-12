#include "host/UiMetrics.h"

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;

static void expect(bool ok, const char* msg) {
    if (ok) return;
    std::fprintf(stderr, "FAIL %s\n", msg);
    ++fails;
}

static void placeChips(const wchar_t** labels, int n, float contentW, std::vector<ui::R>& out) {
    float x = 0, y = 0;
    for (int i = 0; i < n; ++i) {
        const float w = ui::chipWidth(labels[i], 12.0f, contentW);
        const bool reset = i == n - 1;
        const float lead = (reset && x > 0.0f) ? ui::kGap : 0.0f;
        if (x > 0.0f && x + lead + w > contentW + 0.5f) {
            x = 0;
            y += ui::kChipH + ui::kChipGap;
        } else {
            x += lead;
        }
        out.push_back({x, y, w, ui::kChipH});
        x += w + ui::kChipGap;
    }
}

int main() {
    const float widgetPanel = 360.0f - ui::kPad * 2.0f;
    const float widgetContent = ui::contentWidth(widgetPanel);
    const wchar_t* night[] = {L"Summer dusk", L"Deep woods", L"Meadow", L"Night sky", L"Reset"};
    std::vector<ui::R> chips;
    placeChips(night, 5, widgetContent, chips);
    ui::R content{0, 0, widgetContent, 400};
    ui::R gutter{widgetContent, 0, ui::kGutter, 400};
    expect(ui::audit(chips.data(), (int)chips.size(), content, gutter) == nullptr, "night chips");
    expect(chips.size() == 5, "five chips");
    expect(chips.back().y > chips.front().y, "Reset wrapped off the first line");
    for (const auto& c : chips) expect(c.x + c.w <= widgetContent + 0.5f, "chip inside content");

    float label = ui::textWidth(L"Center density", 12.0f);
    float value = ui::textWidth(L"1.00x", 12.0f);
    ui::Columns cols = ui::columnsFor(widgetContent, label, value);
    expect(cols.label + ui::kGap + cols.track + ui::kGap + cols.value <= widgetContent + 0.5f, "columns fit");
    expect(cols.track >= ui::kMinTrack - 0.5f, "slider has room");
    expect(cols.label + 0.5f >= label, "label column covers the longest label");

    const float side = ui::contentWidth(320.0f);
    const wchar_t* matrix[] = {L"Classic", L"Deep field", L"Binary", L"Glitch", L"Slow drift", L"Reset to default"};
    chips.clear();
    placeChips(matrix, 6, side, chips);
    ui::R sideContent{0, 0, side, 800};
    ui::R sideGutter{side, 0, ui::kGutter, 800};
    expect(ui::audit(chips.data(), (int)chips.size(), sideContent, sideGutter) == nullptr, "matrix chips");
    for (const auto& c : chips) expect(c.x + c.w <= side + 0.5f, "gallery chip clipped");

    if (fails) {
        std::fprintf(stderr, "%d layout checks failed\n", fails);
        return 1;
    }
    std::printf("layout metrics ok, widget content %.0f side content %.0f\n", widgetContent, side);
    return 0;
}
