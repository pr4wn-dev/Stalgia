#include "effect/Presets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
#include <windows.h>

namespace {

float clampf(float v, float a, float b) { return std::max(a, std::min(b, v)); }

EffectLook* lookMut(AppSettings& s, const std::string& id) {
    if (id == "matrix") return &s.matrix.look;
    if (id == "starfield") return &s.starfield.look;
    if (id == "aurora") return &s.aurora.look;
    if (id == "lava") return &s.lava.look;
    if (id == "ape") return &s.ape.look;
    if (id == "myco") return &s.myco.look;
    return nullptr;
}

void addSlider(std::vector<Knob>& v, KnobSection sec, const wchar_t* label, int index, float lo, float hi) {
    Knob k;
    k.kind = KnobKind::Slider;
    k.section = sec;
    k.label = label;
    k.index = index;
    k.lo = lo;
    k.hi = hi;
    v.push_back(k);
}

void addSwatch(std::vector<Knob>& v, const wchar_t* label, int index) {
    Knob k;
    k.kind = KnobKind::Swatch;
    k.section = KnobSection::Color;
    k.label = label;
    k.index = index;
    v.push_back(k);
}

void addToggle(std::vector<Knob>& v, KnobSection sec, const wchar_t* label, int index) {
    Knob k;
    k.kind = KnobKind::Toggle;
    k.section = sec;
    k.label = label;
    k.index = index;
    v.push_back(k);
}

void addButton(std::vector<Knob>& v, KnobSection sec, const wchar_t* label, int index) {
    Knob k;
    k.kind = KnobKind::Button;
    k.section = sec;
    k.label = label;
    k.index = index;
    v.push_back(k);
}

void addShared(std::vector<Knob>& v) {
    addSlider(v, KnobSection::Look, L"Brightness", 20, 0.35f, 1.60f);
    addSlider(v, KnobSection::Look, L"Saturation", 21, 0.0f, 1.80f);
    addSlider(v, KnobSection::Look, L"Vignette", 22, 0.0f, 1.0f);
    addSlider(v, KnobSection::Advanced, L"FPS cap", 23, 0.0f, 3.0f);
    addToggle(v, KnobSection::Advanced, L"Time of day", 10);
    addToggle(v, KnobSection::Advanced, L"Mouse parallax", 11);
}

std::vector<Knob> knobsFor(const std::string& id) {
    std::vector<Knob> v;
    if (id == "matrix") {
        addSlider(v, KnobSection::Motion, L"Speed", 0, 0.1f, 5.0f);
        addSlider(v, KnobSection::Motion, L"Direction", 2, -1.0f, 1.0f);
        addSlider(v, KnobSection::Motion, L"Wind", 3, -1.0f, 1.0f);
        addSlider(v, KnobSection::Look, L"Density", 1, 0.1f, 2.0f);
        addSlider(v, KnobSection::Look, L"Glyph", 4, 10.0f, 48.0f);
        addSlider(v, KnobSection::Look, L"Trail", 5, 6.0f, 60.0f);
        addSlider(v, KnobSection::Look, L"Depth", 6, 0.0f, 1.0f);
        addSlider(v, KnobSection::Look, L"Bloom", 7, 0.0f, 1.6f);
        addSlider(v, KnobSection::Look, L"Glitch", 8, 0.0f, 1.0f);
        addSlider(v, KnobSection::Look, L"Glyph set", 12, 0.0f, 3.0f);
        addSlider(v, KnobSection::Advanced, L"Fade", 9, 0.4f, 3.0f);
        addSlider(v, KnobSection::Advanced, L"Mutate", 10, 0.0f, 0.8f);
        addSlider(v, KnobSection::Advanced, L"Center density", 11, 0.0f, 1.0f);
        addButton(v, KnobSection::Advanced, L"Custom glyphs…", 0);
        addSwatch(v, L"Color", 0);
        addSwatch(v, L"Head color", 1);
    } else if (id == "starfield") {
        addSlider(v, KnobSection::Motion, L"Speed", 0, 0.1f, 5.0f);
        addSlider(v, KnobSection::Motion, L"Warp", 2, 0.0f, 1.0f);
        addSlider(v, KnobSection::Motion, L"Direction", 3, 0.0f, 1.0f);
        addSlider(v, KnobSection::Look, L"Density", 1, 0.2f, 2.0f);
        addSlider(v, KnobSection::Look, L"Twinkle", 4, 0.0f, 1.0f);
        addSlider(v, KnobSection::Look, L"Nebula", 5, 0.0f, 1.0f);
        addSwatch(v, L"Color", 0);
    } else if (id == "aurora") {
        addSlider(v, KnobSection::Motion, L"Wave speed", 0, 0.05f, 3.0f);
        addSlider(v, KnobSection::Look, L"Height", 1, 0.15f, 1.0f);
        addSlider(v, KnobSection::Look, L"Rays", 2, 0.0f, 1.0f);
        addToggle(v, KnobSection::Look, L"Stars behind", 0);
        addToggle(v, KnobSection::Look, L"Horizon", 1);
        addSwatch(v, L"Color", 0);
        addSwatch(v, L"Second color", 1);
    } else if (id == "lava") {
        addSlider(v, KnobSection::Motion, L"Speed", 0, 0.1f, 2.5f);
        addSlider(v, KnobSection::Motion, L"Blobs", 1, 8.0f, 16.0f);
        addSlider(v, KnobSection::Motion, L"Viscosity", 2, 0.35f, 2.2f);
        addSlider(v, KnobSection::Look, L"Glow", 3, 0.2f, 3.0f);
        addSlider(v, KnobSection::Look, L"Size", 4, 0.2f, 2.0f);
        addSlider(v, KnobSection::Look, L"Lamp", 5, 0.0f, 1.0f);
        addSlider(v, KnobSection::Color, L"Heat", 6, 0.0f, 1.0f);
        addSlider(v, KnobSection::Color, L"Gradient", 7, 0.0f, 1.0f);
        addSwatch(v, L"Lava color", 0);
        addSwatch(v, L"Liquid tint", 1);
    } else if (id == "ape" || id == "myco") {
        addSlider(v, KnobSection::Motion, L"Speed", 0, 0.1f, 3.0f);
        addSlider(v, KnobSection::Motion, L"Reveal", 2, 0.2f, 2.2f);
        addSlider(v, KnobSection::Look, L"Density", 1, 0.35f, 1.20f);
        addSlider(v, KnobSection::Look, L"Detail", 6, 0.0f, 1.0f);
        addSlider(v, KnobSection::Look, L"Breathe", 7, 0.0f, 2.0f);
        addSlider(v, KnobSection::Look, L"Brightness", 8, 0.4f, 2.5f);
        addSlider(v, KnobSection::Look, L"Glyph", 3, 7.0f, 40.0f);
        addSlider(v, KnobSection::Look, L"Size", 4, 0.50f, 0.95f);
        addSlider(v, KnobSection::Look, L"Position", 5, 0.25f, 0.75f);
        addToggle(v, KnobSection::Look, id == "myco" ? L"Color" : L"Image colors", 2);
        if (id == "myco") {
            addToggle(v, KnobSection::Look, L"Eyes", 1);
            addToggle(v, KnobSection::Look, L"Breathe", 0);
        } else {
            addToggle(v, KnobSection::Look, L"Eyes", 0);
            addToggle(v, KnobSection::Look, L"Breathe", 1);
        }
        addButton(v, KnobSection::Look, L"Choose image…", 0);
        addSwatch(v, L"Code color", 0);
    }
    addShared(v);
    return v;
}

int fpsFromStep(int step) {
    if (step <= 0) return 0;
    if (step == 1) return 15;
    if (step == 2) return 30;
    return 60;
}

int stepFromFps(int fps) {
    if (fps == 15) return 1;
    if (fps == 30) return 2;
    if (fps == 60) return 3;
    return 0;
}

std::wstring pct(float v) {
    wchar_t b[32];
    swprintf(b, 32, L"%.0f%%", v * 100.0f);
    return b;
}

std::wstring num(float v, const wchar_t* unit) {
    wchar_t b[40];
    swprintf(b, 40, L"%.2f%s", v, unit);
    return b;
}

void lookPreset(EffectLook& l, float bright, float sat, float vig, bool night, bool para, int fps) {
    l.brightness = bright;
    l.saturation = sat;
    l.vignette = vig;
    l.timeOfDay = night;
    l.parallax = para;
    l.fpsCap = fps;
}

}  // namespace

const EffectLook* effectLook(const AppSettings& s, const std::string& id) {
    if (id == "matrix") return &s.matrix.look;
    if (id == "starfield") return &s.starfield.look;
    if (id == "aurora") return &s.aurora.look;
    if (id == "lava") return &s.lava.look;
    if (id == "ape") return &s.ape.look;
    if (id == "myco") return &s.myco.look;
    return nullptr;
}

float nightAmount(bool enabled) {
    if (!enabled) return 0.0f;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const float h = (float)st.wHour + (float)st.wMinute / 60.0f;
    float n = 0.0f;
    if (h >= 18.0f) n = clampf((h - 18.0f) / 3.5f, 0.0f, 1.0f);
    else if (h < 7.0f) n = 1.0f - clampf((h - 4.5f) / 2.5f, 0.0f, 1.0f);
    return n * 0.85f;
}

int frameCapFps(const AppSettings& s) {
    int cap = Config::clampFps(s.fps);
    const EffectLook* look = effectLook(s, s.effect);
    if (!look) return cap;
    if (look->fpsCap == 15 || look->fpsCap == 30 || look->fpsCap == 60) cap = std::min(cap, look->fpsCap);
    return cap;
}

int knobCount(const std::string& id) { return (int)knobsFor(id).size(); }

Knob knobAt(const std::string& id, int ordinal) {
    const auto v = knobsFor(id);
    if (ordinal < 0 || ordinal >= (int)v.size()) return Knob{};
    return v[(size_t)ordinal];
}

float knobValue(const AppSettings& s, const std::string& id, int index) {
    const EffectLook* look = effectLook(s, id);
    if (look) {
        if (index == 20) return look->brightness;
        if (index == 21) return look->saturation;
        if (index == 22) return look->vignette;
        if (index == 23) return (float)stepFromFps(look->fpsCap);
    }
    if (id == "matrix") {
        const auto& m = s.matrix;
        if (index == 0) return m.speed;
        if (index == 1) return m.density;
        if (index == 2) return m.direction;
        if (index == 3) return m.wind;
        if (index == 4) return (float)m.glyphSize;
        if (index == 5) return m.trailLength;
        if (index == 6) return m.depth;
        if (index == 7) return m.bloom;
        if (index == 8) return m.glitch;
        if (index == 9) return m.fadePower;
        if (index == 10) return m.mutate;
        if (index == 11) return m.region;
        if (index == 12) return (float)m.glyphSet;
    } else if (id == "starfield") {
        const auto& m = s.starfield;
        if (index == 0) return m.speed;
        if (index == 1) return m.density;
        if (index == 2) return m.warp;
        if (index == 3) return m.direction;
        if (index == 4) return m.twinkle;
        if (index == 5) return m.nebula;
    } else if (id == "aurora") {
        if (index == 0) return s.aurora.speed;
        if (index == 1) return s.aurora.height;
        if (index == 2) return s.aurora.rays;
    } else if (id == "lava") {
        const auto& m = s.lava;
        if (index == 0) return m.speed;
        if (index == 1) return (float)m.blobCount;
        if (index == 2) return m.viscosity;
        if (index == 3) return m.glow;
        if (index == 4) return m.blobSize;
        if (index == 5) return m.lamp;
        if (index == 6) return m.heat;
        if (index == 7) return m.gradient;
    } else if (id == "ape") {
        const auto& m = s.ape;
        if (index == 0) return m.speed;
        if (index == 1) return m.bgDim;
        if (index == 2) return m.reveal;
        if (index == 3) return (float)m.glyphSize;
        if (index == 4) return m.size;
        if (index == 5) return m.posX;
        if (index == 6) return m.detail;
        if (index == 7) return m.breatheAmt;
        if (index == 8) return m.apeBrightness;
    } else if (id == "myco") {
        const auto& m = s.myco;
        if (index == 0) return m.speed;
        if (index == 1) return m.bgDim;
        if (index == 2) return m.reveal;
        if (index == 3) return (float)m.glyphSize;
        if (index == 4) return m.size;
        if (index == 5) return m.posX;
        if (index == 6) return m.detail;
        if (index == 7) return m.breatheAmt;
        if (index == 8) return m.brightness;
    }
    return 0;
}

void setKnob(AppSettings& s, const std::string& id, int index, float t) {
    t = clampf(t, 0.0f, 1.0f);
    const auto all = knobsFor(id);
    float lo = 0, hi = 1;
    bool found = false;
    for (const auto& k : all) {
        if (k.kind == KnobKind::Slider && k.index == index) {
            lo = k.lo;
            hi = k.hi;
            found = true;
            break;
        }
    }
    if (!found) return;
    float v = lo + t * (hi - lo);
    const bool integer = (id == "matrix" && (index == 4 || index == 12)) || (id == "lava" && index == 1) ||
                         ((id == "ape" || id == "myco") && index == 3) || index == 23;
    if (integer) v = std::round(v);

    if (EffectLook* look = lookMut(s, id)) {
        if (index == 20) {
            look->brightness = v;
            return;
        }
        if (index == 21) {
            look->saturation = v;
            return;
        }
        if (index == 22) {
            look->vignette = v;
            return;
        }
        if (index == 23) {
            look->fpsCap = fpsFromStep((int)v);
            return;
        }
    }
    if (id == "matrix") {
        auto& m = s.matrix;
        if (index == 0) m.speed = v;
        else if (index == 1) m.density = v;
        else if (index == 2) m.direction = v;
        else if (index == 3) m.wind = v;
        else if (index == 4) m.glyphSize = (int)v;
        else if (index == 5) m.trailLength = v;
        else if (index == 6) m.depth = v;
        else if (index == 7) m.bloom = v;
        else if (index == 8) m.glitch = v;
        else if (index == 9) m.fadePower = v;
        else if (index == 10) m.mutate = v;
        else if (index == 11) m.region = v;
        else if (index == 12) m.glyphSet = (int)v;
    } else if (id == "starfield") {
        auto& m = s.starfield;
        if (index == 0) m.speed = v;
        else if (index == 1) m.density = v;
        else if (index == 2) m.warp = v;
        else if (index == 3) m.direction = v;
        else if (index == 4) m.twinkle = v;
        else if (index == 5) m.nebula = v;
    } else if (id == "aurora") {
        if (index == 0) s.aurora.speed = v;
        else if (index == 1) s.aurora.height = v;
        else if (index == 2) s.aurora.rays = v;
    } else if (id == "lava") {
        auto& m = s.lava;
        if (index == 0) m.speed = v;
        else if (index == 1) m.blobCount = (int)v;
        else if (index == 2) m.viscosity = v;
        else if (index == 3) m.glow = v;
        else if (index == 4) m.blobSize = v;
        else if (index == 5) m.lamp = v;
        else if (index == 6) m.heat = v;
        else if (index == 7) m.gradient = v;
    } else if (id == "ape") {
        auto& m = s.ape;
        if (index == 0) m.speed = v;
        else if (index == 1) m.bgDim = v;
        else if (index == 2) m.reveal = v;
        else if (index == 3) m.glyphSize = (int)v;
        else if (index == 4) m.size = v;
        else if (index == 5) m.posX = v;
        else if (index == 6) m.detail = v;
        else if (index == 7) m.breatheAmt = v;
        else if (index == 8) m.apeBrightness = v;
    } else if (id == "myco") {
        auto& m = s.myco;
        if (index == 0) m.speed = v;
        else if (index == 1) m.bgDim = v;
        else if (index == 2) m.reveal = v;
        else if (index == 3) m.glyphSize = (int)v;
        else if (index == 4) m.size = v;
        else if (index == 5) m.posX = v;
        else if (index == 6) m.detail = v;
        else if (index == 7) m.breatheAmt = v;
        else if (index == 8) m.brightness = v;
    }
}

bool knobToggle(const AppSettings& s, const std::string& id, int index) {
    const EffectLook* look = effectLook(s, id);
    if (index == 10) return look && look->timeOfDay;
    if (index == 11) return look && look->parallax;
    if (id == "aurora") {
        if (index == 0) return s.aurora.stars;
        if (index == 1) return s.aurora.horizon;
    } else if (id == "ape") {
        if (index == 0) return s.ape.eyeGlow;
        if (index == 1) return s.ape.breathing;
        if (index == 2) return s.ape.fromImage;
    } else if (id == "myco") {
        if (index == 0) return s.myco.breathing;
        if (index == 1) return s.myco.eyeGlow;
        if (index == 2) return s.myco.colorize;
    }
    return false;
}

void setKnobToggle(AppSettings& s, const std::string& id, int index, bool on) {
    if (EffectLook* look = lookMut(s, id)) {
        if (index == 10) {
            look->timeOfDay = on;
            return;
        }
        if (index == 11) {
            look->parallax = on;
            return;
        }
    }
    if (id == "aurora") {
        if (index == 0) s.aurora.stars = on;
        else if (index == 1) s.aurora.horizon = on;
    } else if (id == "ape") {
        if (index == 0) s.ape.eyeGlow = on;
        else if (index == 1) s.ape.breathing = on;
        else if (index == 2) s.ape.fromImage = on;
    } else if (id == "myco") {
        if (index == 0) s.myco.breathing = on;
        else if (index == 1) s.myco.eyeGlow = on;
        else if (index == 2) s.myco.colorize = on;
    }
}

uint32_t knobColor(const AppSettings& s, const std::string& id, int index) {
    if (id == "matrix") return index == 0 ? s.matrix.color : s.matrix.headColor;
    if (id == "starfield") return s.starfield.color;
    if (id == "aurora") return index == 0 ? s.aurora.colorA : s.aurora.colorB;
    if (id == "lava") return index == 0 ? s.lava.lavaColor : s.lava.liquidColor;
    if (id == "ape") return s.ape.color;
    if (id == "myco") return s.myco.color;
    return 0xFFFFFF;
}

void setKnobColor(AppSettings& s, const std::string& id, int index, uint32_t rgb) {
    if (id == "matrix") {
        if (index == 0) s.matrix.color = rgb;
        else s.matrix.headColor = rgb;
    } else if (id == "starfield") {
        s.starfield.color = rgb;
    } else if (id == "aurora") {
        if (index == 0) s.aurora.colorA = rgb;
        else s.aurora.colorB = rgb;
    } else if (id == "lava") {
        if (index == 0) s.lava.lavaColor = rgb;
        else s.lava.liquidColor = rgb;
    } else if (id == "ape") {
        s.ape.color = rgb;
    } else if (id == "myco") {
        s.myco.color = rgb;
    }
}

std::wstring knobReadout(const AppSettings& s, const std::string& id, int index) {
    const float v = knobValue(s, id, index);
    if (index == 20 || index == 21 || index == 22) return pct(index == 22 ? v : (index == 20 ? v : v));
    if (index == 20) return pct(v);
    if (index == 21) return pct(v / 1.0f);
    if (index == 23) {
        const int fps = fpsFromStep((int)std::lround(v));
        if (fps == 0) return L"global";
        wchar_t b[24];
        swprintf(b, 24, L"%d fps", fps);
        return b;
    }
    if (id == "matrix" && index == 12) {
        switch ((int)std::lround(v)) {
            case 1: return L"Latin";
            case 2: return L"Binary";
            case 3: return L"Custom";
            default: return L"Kana";
        }
    }
    if (id == "matrix" && index == 2) {
        if (v > 0.15f) return L"down";
        if (v < -0.15f) return L"up";
        return L"drift";
    }
    if ((id == "matrix" && (index == 6 || index == 8 || index == 11)) ||
        (id == "starfield" && (index == 2 || index == 4 || index == 5)) ||
        (id == "aurora" && (index == 1 || index == 2)) ||
        (id == "lava" && (index == 5 || index == 6 || index == 7)) ||
        ((id == "ape" || id == "myco") && index == 6)) {
        return pct(v);
    }
    if (id == "matrix" && index == 4) {
        wchar_t b[16];
        swprintf(b, 16, L"%.0f px", v);
        return b;
    }
    if ((id == "ape" || id == "myco") && index == 3) {
        wchar_t b[16];
        swprintf(b, 16, L"%.0f px", v);
        return b;
    }
    if (id == "lava" && index == 1) {
        wchar_t b[16];
        swprintf(b, 16, L"%.0f", v);
        return b;
    }
    const wchar_t* unit = L"";
    if (index == 0) unit = L"×";
    return num(v, unit);
}

int presetCount(const std::string& id) {
    if (id == "matrix") return 5;
    if (id == "starfield" || id == "aurora" || id == "lava" || id == "ape" || id == "myco") return 4;
    return 0;
}

const char* presetName(const std::string& id, int index) {
    if (id == "matrix") {
        static const char* n[] = {"Classic", "Deep field", "Binary", "Glitch", "Slow drift"};
        if (index >= 0 && index < 5) return n[index];
    } else if (id == "starfield") {
        static const char* n[] = {"Still night", "Milky Way", "Nebula", "Warp"};
        if (index >= 0 && index < 4) return n[index];
    } else if (id == "aurora") {
        static const char* n[] = {"Classic", "Magenta", "Polar", "Horizon"};
        if (index >= 0 && index < 4) return n[index];
    } else if (id == "lava") {
        static const char* n[] = {"Classic", "Magma", "Cool wax", "Lamp"};
        if (index >= 0 && index < 4) return n[index];
    } else if (id == "ape") {
        static const char* n[] = {"Classic", "Soft reveal", "Dense code", "Dim"};
        if (index >= 0 && index < 4) return n[index];
    } else if (id == "myco") {
        static const char* n[] = {"Classic", "Mono", "Slow bloom", "Night"};
        if (index >= 0 && index < 4) return n[index];
    }
    return "Preset";
}

void resetEffect(AppSettings& s, const std::string& id) {
    // Presets replace the look, not the user's own glyphs, photos, or parallax choice.
    if (id == "matrix") {
        const auto glyphs = s.matrix.customGlyphs;
        const bool para = s.matrix.look.parallax;
        s.matrix = MatrixSettings{};
        s.matrix.customGlyphs = glyphs;
        s.matrix.look.parallax = para;
    } else if (id == "starfield") {
        const bool para = s.starfield.look.parallax;
        s.starfield = StarfieldSettings{};
        s.starfield.look.parallax = para;
    } else if (id == "aurora") {
        const bool para = s.aurora.look.parallax;
        s.aurora = AuroraSettings{};
        s.aurora.look.parallax = para;
    } else if (id == "lava") {
        const bool para = s.lava.look.parallax;
        s.lava = LavaSettings{};
        s.lava.look.parallax = para;
    } else if (id == "ape") {
        const auto path = s.ape.imagePath;
        const bool fromImage = s.ape.fromImage;
        const bool para = s.ape.look.parallax;
        s.ape = ApeSettings{};
        s.ape.imagePath = path;
        s.ape.fromImage = fromImage;
        s.ape.look.parallax = para;
    } else if (id == "myco") {
        const auto path = s.myco.imagePath;
        const bool para = s.myco.look.parallax;
        s.myco = MycoSettings{};
        s.myco.imagePath = path;
        s.myco.look.parallax = para;
    }
}

void applyPreset(AppSettings& s, const std::string& id, int index) {
    resetEffect(s, id);
    if (id == "matrix") {
        auto& m = s.matrix;
        if (index == 1) {
            m.depth = 0.9f;
            m.density = 1.25f;
            m.speed = 0.75f;
            m.region = 0.85f;
            m.bloom = 0.85f;
            m.wind = 0.2f;
            lookPreset(m.look, 0.9f, 1.05f, 0.35f, false, true, 30);
        } else if (index == 2) {
            m.glyphSet = 2;
            m.color = 0xB6FFC8;
            m.headColor = 0xFFFFFF;
            m.speed = 1.6f;
            m.glitch = 0.45f;
            m.trailLength = 16;
            m.bloom = 0.4f;
            lookPreset(m.look, 1.05f, 0.7f, 0.12f, false, false, 30);
        } else if (index == 3) {
            m.glyphSet = 1;
            m.glitch = 0.85f;
            m.wind = 0.65f;
            m.mutate = 0.35f;
            m.trailLength = 12;
            m.speed = 1.35f;
            m.color = 0x39FFB0;
            m.headColor = 0xE8FFF4;
            lookPreset(m.look, 1.0f, 1.2f, 0.2f, false, false, 30);
        } else if (index == 4) {
            m.speed = 0.45f;
            m.trailLength = 40;
            m.mutate = 0.02f;
            m.wind = 0.35f;
            m.depth = 0.4f;
            m.bloom = 0.3f;
            m.glitch = 0.04f;
            lookPreset(m.look, 0.85f, 0.9f, 0.28f, true, false, 30);
        }
    } else if (id == "starfield") {
        auto& m = s.starfield;
        if (index == 1) {
            m.speed = 0.18f;
            m.density = 1.55f;
            m.twinkle = 0.28f;
            m.nebula = 0.05f;
            m.warp = 0.0f;
            m.color = 0xFFF6E8;
            lookPreset(m.look, 0.92f, 0.9f, 0.34f, true, false, 30);
        } else if (index == 2) {
            m.speed = 0.30f;
            m.density = 0.85f;
            m.twinkle = 0.32f;
            m.nebula = 0.85f;
            m.warp = 0.0f;
            m.color = 0xD9C6FF;
            lookPreset(m.look, 1.0f, 1.2f, 0.28f, false, false, 30);
        } else if (index == 3) {
            m.speed = 1.7f;
            m.density = 0.62f;
            m.twinkle = 0.12f;
            m.nebula = 0.10f;
            m.warp = 0.92f;
            m.direction = 0.06f;
            m.color = 0xE7F0FF;
            lookPreset(m.look, 1.05f, 1.05f, 0.20f, false, false, 0);
        }
    } else if (id == "aurora") {
        auto& m = s.aurora;
        if (index == 1) {
            m.colorA = 0xFF5FA2;
            m.colorB = 0xB44CFF;
            m.speed = 0.36f;
            m.height = 0.66f;
            m.rays = 0.60f;
            m.stars = true;
            lookPreset(m.look, 1.0f, 1.25f, 0.24f, false, false, 30);
        } else if (index == 2) {
            m.colorA = 0x86FFE0;
            m.colorB = 0x6A4BFF;
            m.speed = 0.24f;
            m.height = 0.78f;
            m.rays = 0.70f;
            m.stars = true;
            lookPreset(m.look, 0.96f, 1.1f, 0.30f, false, false, 30);
        } else if (index == 3) {
            m.colorA = 0x3CFF78;
            m.colorB = 0xFF4B7A;
            m.horizon = true;
            m.stars = true;
            m.speed = 0.26f;
            m.height = 0.85f;
            m.rays = 0.48f;
            lookPreset(m.look, 0.92f, 1.0f, 0.36f, true, false, 30);
        }
    } else if (id == "lava") {
        auto& m = s.lava;
        if (index == 1) {
            m.lavaColor = 0xFF3B10;
            m.heat = 0.9f;
            m.glow = 2.0f;
            m.blobCount = 14;
            m.speed = 1.3f;
            m.viscosity = 0.7f;
            m.gradient = 0.8f;
            lookPreset(m.look, 1.05f, 1.2f, 0.2f, false, false, 30);
        } else if (index == 2) {
            m.lavaColor = 0x6A5CFF;
            m.liquidColor = 0x120818;
            m.speed = 0.55f;
            m.viscosity = 1.7f;
            m.heat = 0.35f;
            m.glow = 1.1f;
            m.gradient = 0.45f;
            lookPreset(m.look, 0.95f, 1.15f, 0.28f, false, false, 30);
        } else if (index == 3) {
            m.lamp = 1.0f;
            m.blobCount = 9;
            m.blobSize = 0.95f;
            m.speed = 0.8f;
            m.viscosity = 1.25f;
            m.glow = 1.6f;
            m.lavaColor = 0xFF8A2A;
            lookPreset(m.look, 1.0f, 1.1f, 0.45f, false, false, 30);
        }
    } else if (id == "ape") {
        auto& m = s.ape;
        if (index == 1) {
            m.reveal = 0.45f;
            m.breatheAmt = 1.6f;
            m.detail = 0.8f;
            m.speed = 0.7f;
            lookPreset(m.look, 1.0f, 0.9f, 0.16f, false, false, 30);
        } else if (index == 2) {
            m.glyphSize = 7;
            m.speed = 1.5f;
            m.bgDim = 1.15f;
            m.reveal = 1.4f;
            m.detail = 0.4f;
            lookPreset(m.look, 0.95f, 0.8f, 0.12f, false, false, 30);
        } else if (index == 3) {
            m.look.brightness = 0.62f;
            m.look.vignette = 0.4f;
            m.bgDim = 0.55f;
            m.apeBrightness = 1.3f;
            lookPreset(m.look, 0.62f, 0.75f, 0.4f, false, false, 30);
        }
    } else if (id == "myco") {
        auto& m = s.myco;
        if (index == 1) {
            m.colorize = false;
            m.color = 0xF4F7FF;
            m.detail = 0.7f;
            lookPreset(m.look, 1.0f, 0.4f, 0.16f, false, false, 30);
        } else if (index == 2) {
            m.reveal = 0.4f;
            m.breatheAmt = 1.8f;
            m.speed = 0.55f;
            m.detail = 0.85f;
            lookPreset(m.look, 1.0f, 1.15f, 0.2f, false, true, 30);
        } else if (index == 3) {
            m.colorize = true;
            m.speed = 0.7f;
            lookPreset(m.look, 0.7f, 0.9f, 0.38f, true, false, 30);
        }
    }
}
