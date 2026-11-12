#pragma once

#include "core/Config.h"

#include <string>

enum class KnobSection { Motion = 0, Look = 1, Color = 2, Advanced = 3 };
enum class KnobKind { Slider, Swatch, Toggle, Button };

struct Knob {
    KnobKind kind = KnobKind::Slider;
    KnobSection section = KnobSection::Motion;
    const wchar_t* label = L"";
    int index = 0;
    float lo = 0;
    float hi = 1;
};

int knobCount(const std::string& id);
Knob knobAt(const std::string& id, int ordinal);
float knobValue(const AppSettings& s, const std::string& id, int index);
void setKnob(AppSettings& s, const std::string& id, int index, float t);
bool knobToggle(const AppSettings& s, const std::string& id, int index);
void setKnobToggle(AppSettings& s, const std::string& id, int index, bool on);
uint32_t knobColor(const AppSettings& s, const std::string& id, int index);
void setKnobColor(AppSettings& s, const std::string& id, int index, uint32_t rgb);
std::wstring knobReadout(const AppSettings& s, const std::string& id, int index);

int presetCount(const std::string& id);
const char* presetName(const std::string& id, int index);
void applyPreset(AppSettings& s, const std::string& id, int index);
void resetEffect(AppSettings& s, const std::string& id);

const EffectLook* effectLook(const AppSettings& s, const std::string& id);
float nightAmount(bool enabled);
int frameCapFps(const AppSettings& s);
