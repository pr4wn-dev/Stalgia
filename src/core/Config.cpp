#include "core/Config.h"

#include "core/Log.h"
#include "core/Util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace {

std::string readAll(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    std::string out;
    char buf[4096];
    while (true) {
        const size_t n = fread(buf, 1, sizeof(buf), f);
        if (!n) break;
        out.append(buf, n);
    }
    fclose(f);
    return out;
}

bool writeAll(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    if (ok) fflush(f);
    fclose(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

float parseFinite(const std::string& raw, float fallback) {
    if (raw.empty()) return fallback;
    char* end = nullptr;
    const float v = std::strtof(raw.c_str(), &end);
    if (!end || end == raw.c_str() || !std::isfinite(v)) return fallback;
    return v;
}

void skipWs(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

bool parseString(const std::string& s, size_t& i, std::string& out) {
    skipWs(s, i);
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\' && i < s.size()) {
            char e = s[i++];
            if (e == 'n') out.push_back('\n');
            else if (e == 't') out.push_back('\t');
            else out.push_back(e);
        } else {
            out.push_back(c);
        }
    }
    return false;
}

struct Kv {
    std::string key;
    std::string raw;
};

bool parseObjectKvs(const std::string& s, size_t& i, std::vector<Kv>& kvs) {
    skipWs(s, i);
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    kvs.clear();
    while (true) {
        skipWs(s, i);
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        std::string key;
        if (!parseString(s, i, key)) return false;
        skipWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        skipWs(s, i);
        if (i >= s.size()) return false;

        Kv kv;
        kv.key = key;
        if (s[i] == '"') {
            const size_t begin = i;
            std::string v;
            if (!parseString(s, i, v)) return false;
            kv.raw = s.substr(begin, i - begin);
        } else if (s[i] == '[') {
            int depth = 0;
            const size_t begin = i;
            do {
                if (s[i] == '[') ++depth;
                else if (s[i] == ']') --depth;
                else if (s[i] == '"') {
                    ++i;
                    while (i < s.size() && s[i] != '"') {
                        if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                        else ++i;
                    }
                }
                if (i < s.size()) ++i;
            } while (i < s.size() && depth > 0);
            kv.raw = s.substr(begin, i - begin);
        } else if (s[i] == '{') {
            int depth = 0;
            const size_t begin = i;
            do {
                if (s[i] == '{') ++depth;
                else if (s[i] == '}') --depth;
                else if (s[i] == '"') {
                    ++i;
                    while (i < s.size() && s[i] != '"') {
                        if (s[i] == '\\' && i + 1 < s.size()) i += 2;
                        else ++i;
                    }
                }
                ++i;
            } while (i < s.size() && depth > 0);
            kv.raw = s.substr(begin, i - begin);
        } else {
            const size_t begin = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' &&
                   !std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            kv.raw = s.substr(begin, i - begin);
        }
        kvs.push_back(std::move(kv));
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        return false;
    }
}

std::string unquote(const std::string& raw) {
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
        return raw.substr(1, raw.size() - 2);
    }
    return raw;
}

const Kv* findKv(const std::vector<Kv>& kvs, const char* key) {
    for (const auto& kv : kvs)
        if (kv.key == key) return &kv;
    return nullptr;
}

int parseInt(const std::string& raw, int fallback) {
    if (raw.empty()) return fallback;
    return (int)std::lround(std::atof(unquote(raw).c_str()));
}

}  // namespace

int Config::clampFps(int fps) {
    return std::clamp(fps, 10, 60);
}

void Config::resolvePath() {
    const std::wstring portable = exeDirectory() + L"\\config.json";
    if (fileExists(portable)) {
        m_path = portable;
        return;
    }
    const std::wstring dir = appDataDirectory();
    ensureDirectory(dir);
    m_path = dir + L"\\config.json";
    if (!fileExists(m_path)) {
        const std::wstring legacy = legacyAppDataDirectory() + L"\\config.json";
        if (fileExists(legacy) && copyFile(legacy, m_path)) {
            LOG_INFO("Migrated config from Desktop Rain: %s", narrow(legacy).c_str());
        }
    }
}

void clampLook(EffectLook& l) {
    l.brightness = std::clamp(l.brightness, 0.35f, 1.60f);
    l.saturation = std::clamp(l.saturation, 0.0f, 1.80f);
    l.vignette = std::clamp(l.vignette, 0.0f, 1.0f);
    if (l.fpsCap != 0 && l.fpsCap != 15 && l.fpsCap != 30 && l.fpsCap != 60) l.fpsCap = 0;
}

void Config::clampSettings() {
    m_s.fps = clampFps(m_s.fps);
    m_s.matrix.speed = std::clamp(m_s.matrix.speed, 0.1f, 5.0f);
    m_s.matrix.density = std::clamp(m_s.matrix.density, 0.1f, 2.0f);
    m_s.matrix.glyphSize = std::clamp(m_s.matrix.glyphSize, 10, 48);
    m_s.matrix.trailLength = std::clamp(m_s.matrix.trailLength, 6.0f, 60.0f);
    m_s.matrix.fadePower = std::clamp(m_s.matrix.fadePower, 0.4f, 3.0f);
    m_s.matrix.mutate = std::clamp(m_s.matrix.mutate, 0.0f, 0.8f);
    m_s.matrix.glyphSet = std::clamp(m_s.matrix.glyphSet, 0, 3);
    m_s.matrix.depth = std::clamp(m_s.matrix.depth, 0.0f, 1.0f);
    m_s.matrix.bloom = std::clamp(m_s.matrix.bloom, 0.0f, 1.6f);
    m_s.matrix.glitch = std::clamp(m_s.matrix.glitch, 0.0f, 1.0f);
    m_s.matrix.direction = std::clamp(m_s.matrix.direction, -1.0f, 1.0f);
    m_s.matrix.wind = std::clamp(m_s.matrix.wind, -1.0f, 1.0f);
    m_s.matrix.region = std::clamp(m_s.matrix.region, 0.0f, 1.0f);
    clampLook(m_s.matrix.look);
    m_s.starfield.speed = std::clamp(m_s.starfield.speed, 0.1f, 5.0f);
    m_s.starfield.density = std::clamp(m_s.starfield.density, 0.2f, 2.0f);
    m_s.starfield.twinkle = std::clamp(m_s.starfield.twinkle, 0.0f, 1.0f);
    m_s.starfield.nebula = std::clamp(m_s.starfield.nebula, 0.0f, 1.0f);
    m_s.starfield.warp = std::clamp(m_s.starfield.warp, 0.0f, 1.0f);
    m_s.starfield.direction = std::clamp(m_s.starfield.direction, 0.0f, 1.0f);
    clampLook(m_s.starfield.look);
    m_s.aurora.speed = std::clamp(m_s.aurora.speed, 0.05f, 3.0f);
    m_s.aurora.height = std::clamp(m_s.aurora.height, 0.15f, 1.0f);
    m_s.aurora.rays = std::clamp(m_s.aurora.rays, 0.0f, 1.0f);
    clampLook(m_s.aurora.look);
    m_s.lava.blobCount = std::clamp(m_s.lava.blobCount, 8, 16);
    m_s.lava.speed = std::clamp(m_s.lava.speed, 0.1f, 2.5f);
    m_s.lava.glow = std::clamp(m_s.lava.glow, 0.2f, 3.0f);
    m_s.lava.blobSize = std::clamp(m_s.lava.blobSize, 0.2f, 2.0f);
    m_s.lava.viscosity = std::clamp(m_s.lava.viscosity, 0.35f, 2.2f);
    m_s.lava.heat = std::clamp(m_s.lava.heat, 0.0f, 1.0f);
    m_s.lava.gradient = std::clamp(m_s.lava.gradient, 0.0f, 1.0f);
    m_s.lava.lamp = std::clamp(m_s.lava.lamp, 0.0f, 1.0f);
    clampLook(m_s.lava.look);
    m_s.ape.bgDim = std::clamp(m_s.ape.bgDim, 0.35f, 1.20f);
    m_s.ape.apeBrightness = std::clamp(m_s.ape.apeBrightness, 0.4f, 2.5f);
    m_s.ape.size = std::clamp(m_s.ape.size, 0.50f, 0.95f);
    m_s.ape.posX = std::clamp(m_s.ape.posX, 0.25f, 0.75f);
    m_s.ape.speed = std::clamp(m_s.ape.speed, 0.1f, 3.0f);
    m_s.ape.glyphSize = std::clamp(m_s.ape.glyphSize, 7, 40);
    m_s.ape.reveal = std::clamp(m_s.ape.reveal, 0.2f, 2.2f);
    m_s.ape.detail = std::clamp(m_s.ape.detail, 0.0f, 1.0f);
    m_s.ape.breatheAmt = std::clamp(m_s.ape.breatheAmt, 0.0f, 2.0f);
    clampLook(m_s.ape.look);
    m_s.myco.bgDim = std::clamp(m_s.myco.bgDim, 0.35f, 1.20f);
    m_s.myco.brightness = std::clamp(m_s.myco.brightness, 0.4f, 2.5f);
    m_s.myco.size = std::clamp(m_s.myco.size, 0.50f, 0.95f);
    m_s.myco.posX = std::clamp(m_s.myco.posX, 0.25f, 0.75f);
    m_s.myco.speed = std::clamp(m_s.myco.speed, 0.1f, 3.0f);
    m_s.myco.glyphSize = std::clamp(m_s.myco.glyphSize, 7, 40);
    m_s.myco.reveal = std::clamp(m_s.myco.reveal, 0.2f, 2.2f);
    m_s.myco.detail = std::clamp(m_s.myco.detail, 0.0f, 1.0f);
    m_s.myco.breatheAmt = std::clamp(m_s.myco.breatheAmt, 0.0f, 2.0f);
    clampLook(m_s.myco.look);
    m_s.fireflies.speed = std::clamp(m_s.fireflies.speed, 0.05f, 2.0f);
    m_s.fireflies.density = std::clamp(m_s.fireflies.density, 0.15f, 2.2f);
    m_s.fireflies.mist = std::clamp(m_s.fireflies.mist, 0.0f, 1.0f);
    m_s.fireflies.stars = std::clamp(m_s.fireflies.stars, 0.0f, 1.0f);
    m_s.fireflies.woods = std::clamp(m_s.fireflies.woods, 0.0f, 1.0f);
    clampLook(m_s.fireflies.look);
    m_s.snow.speed = std::clamp(m_s.snow.speed, 0.05f, 2.0f);
    m_s.snow.density = std::clamp(m_s.snow.density, 0.15f, 2.2f);
    m_s.snow.wind = std::clamp(m_s.snow.wind, -1.0f, 1.0f);
    m_s.snow.backdrop = std::clamp(m_s.snow.backdrop, 0, 2);
    clampLook(m_s.snow.look);
}

bool Config::migrateStaleTunes() {
    bool dirty = false;
    if (m_s.version < kFormatVersion) {
        LOG_INFO("Config format %d → %d", m_s.version, kFormatVersion);
        m_s.version = kFormatVersion;
        dirty = true;
    } else if (m_s.version > kFormatVersion) {
        // Forward-compatible: keep unknown newer format, still rewrite tunes we know.
        m_s.version = kFormatVersion;
        dirty = true;
    }

    if (m_s.tuneMatrix < kTuneMatrix) {
        LOG_INFO("Resetting matrix settings (tune %d → %d)", m_s.tuneMatrix, kTuneMatrix);
        const auto glyphs = m_s.matrix.customGlyphs;
        const bool para = m_s.matrix.look.parallax;
        m_s.matrix = MatrixSettings{};
        m_s.matrix.customGlyphs = glyphs;
        m_s.matrix.look.parallax = para;
        m_s.tuneMatrix = kTuneMatrix;
        dirty = true;
    }
    if (m_s.tuneStarfield < kTuneStarfield) {
        LOG_INFO("Resetting starfield settings (tune %d → %d)", m_s.tuneStarfield, kTuneStarfield);
        const bool para = m_s.starfield.look.parallax;
        m_s.starfield = StarfieldSettings{};
        m_s.starfield.look.parallax = para;
        m_s.tuneStarfield = kTuneStarfield;
        dirty = true;
    }
    if (m_s.tuneAurora < kTuneAurora) {
        LOG_INFO("Resetting aurora settings (tune %d → %d)", m_s.tuneAurora, kTuneAurora);
        const bool para = m_s.aurora.look.parallax;
        m_s.aurora = AuroraSettings{};
        m_s.aurora.look.parallax = para;
        m_s.tuneAurora = kTuneAurora;
        dirty = true;
    }
    if (m_s.tuneLava < kTuneLava) {
        LOG_INFO("Resetting lava settings (tune %d → %d)", m_s.tuneLava, kTuneLava);
        const bool para = m_s.lava.look.parallax;
        m_s.lava = LavaSettings{};
        m_s.lava.look.parallax = para;
        m_s.tuneLava = kTuneLava;
        dirty = true;
    }
    if (m_s.tuneApe < kTuneApe) {
        LOG_INFO("Resetting ape settings (tune %d → %d) — dropping stale bgDim/glyphSize/size/brightness",
                 m_s.tuneApe, kTuneApe);
        const auto path = m_s.ape.imagePath;
        const bool fromImage = m_s.ape.fromImage;
        const bool para = m_s.ape.look.parallax;
        m_s.ape = ApeSettings{};
        m_s.ape.imagePath = path;
        m_s.ape.fromImage = fromImage;
        m_s.ape.look.parallax = para;
        m_s.tuneApe = kTuneApe;
        dirty = true;
    }
    if (m_s.tuneMyco < kTuneMyco) {
        LOG_INFO("Resetting myco settings (tune %d → %d)", m_s.tuneMyco, kTuneMyco);
        const auto path = m_s.myco.imagePath;
        const bool para = m_s.myco.look.parallax;
        m_s.myco = MycoSettings{};
        m_s.myco.imagePath = path;
        m_s.myco.look.parallax = para;
        m_s.tuneMyco = kTuneMyco;
        dirty = true;
    }
    if (m_s.tuneFireflies < kTuneFireflies) {
        LOG_INFO("Resetting fireflies settings (tune %d → %d)", m_s.tuneFireflies, kTuneFireflies);
        const bool para = m_s.fireflies.look.parallax;
        m_s.fireflies = FireflySettings{};
        m_s.fireflies.look.parallax = para;
        m_s.tuneFireflies = kTuneFireflies;
        dirty = true;
    }
    if (m_s.tuneSnow < kTuneSnow) {
        LOG_INFO("Resetting snow settings (tune %d → %d)", m_s.tuneSnow, kTuneSnow);
        const bool para = m_s.snow.look.parallax;
        m_s.snow = SnowSettings{};
        m_s.snow.look.parallax = para;
        m_s.tuneSnow = kTuneSnow;
        dirty = true;
    }
    return dirty;
}

namespace {
void stampCurrentTunes(AppSettings& s) {
    s.version = Config::kFormatVersion;
    s.tuneMatrix = Config::kTuneMatrix;
    s.tuneStarfield = Config::kTuneStarfield;
    s.tuneAurora = Config::kTuneAurora;
    s.tuneLava = Config::kTuneLava;
    s.tuneApe = Config::kTuneApe;
    s.tuneMyco = Config::kTuneMyco;
    s.tuneFireflies = Config::kTuneFireflies;
    s.tuneSnow = Config::kTuneSnow;
}
}

bool Config::load() {
    m_migrated = false;
    m_fresh = false;
    resolvePath();
    if (!fileExists(m_path)) {
        m_fresh = true;
        m_s = AppSettings{};
        stampCurrentTunes(m_s);
        LOG_INFO("No config at %s — writing defaults (format %d)", narrow(m_path).c_str(), kFormatVersion);
        save();
        return true;
    }
    std::string text = readAll(m_path);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text.erase(0, 3);
    }
    if (text.empty()) {
        LOG_WARN("Config file empty, using defaults");
        copyFile(m_path, m_path + L".bad");
        m_s = AppSettings{};
        stampCurrentTunes(m_s);
        save();
        return true;
    }
    if (!parse(text)) {
        LOG_WARN("Failed to parse config; copied it to config.json.bad and loading defaults");
        copyFile(m_path, m_path + L".bad");
        m_s = AppSettings{};
        stampCurrentTunes(m_s);
        save();
        return true;
    }
    clampSettings();
    if (migrateStaleTunes()) {
        m_migrated = true;
        clampSettings();
        LOG_INFO("Config migrated to format %d (stale effect tuning reset to current defaults)",
                 kFormatVersion);
        save();
    }
    LOG_INFO("Loaded config from %s effect=%s format=%d tune.fireflies=%d tune.snow=%d widget=%d",
             narrow(m_path).c_str(), m_s.effect.c_str(), m_s.version, m_s.tuneFireflies, m_s.tuneSnow,
             m_s.showWidget ? 1 : 0);
    return true;
}

bool Config::save() const {
    if (m_path.empty()) return false;
    const std::wstring dir = m_path.substr(0, m_path.find_last_of(L"\\/"));
    ensureDirectory(dir);
    if (!writeAll(m_path, serialize())) {
        LOG_ERR("Failed to write config to %s", narrow(m_path).c_str());
        return false;
    }
    return true;
}

namespace {

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') {
            o.push_back('\\');
            o.push_back((char)c);
        } else if (c == '\n') {
            o += "\\n";
        } else if (c >= 32) {
            o.push_back((char)c);
        }
    }
    return o;
}

std::string readJsonString(const std::string& raw) {
    size_t i = 0;
    std::string out;
    if (parseString(raw, i, out)) return out;
    return unquote(raw);
}

void readLook(const std::vector<Kv>& mk, EffectLook& l) {
    if (const Kv* c = findKv(mk, "brightness")) l.brightness = parseFinite(c->raw, l.brightness);
    if (const Kv* c = findKv(mk, "saturation")) l.saturation = parseFinite(c->raw, l.saturation);
    if (const Kv* c = findKv(mk, "vignette")) l.vignette = parseFinite(c->raw, l.vignette);
    if (const Kv* c = findKv(mk, "fpsCap")) l.fpsCap = parseInt(c->raw, l.fpsCap);
    if (const Kv* c = findKv(mk, "timeOfDay")) l.timeOfDay = (c->raw == "true");
    if (const Kv* c = findKv(mk, "parallax")) l.parallax = (c->raw == "true");
}

void writeLook(std::ostringstream& o, const EffectLook& l) {
    o << "    \"brightness\": " << l.brightness << ",\n";
    o << "    \"saturation\": " << l.saturation << ",\n";
    o << "    \"vignette\": " << l.vignette << ",\n";
    o << "    \"fpsCap\": " << l.fpsCap << ",\n";
    o << "    \"timeOfDay\": " << (l.timeOfDay ? "true" : "false") << ",\n";
    o << "    \"parallax\": " << (l.parallax ? "true" : "false") << "\n";
}

}  // namespace

bool Config::parse(const std::string& text) {
    size_t i = 0;
    std::vector<Kv> kvs;
    if (!parseObjectKvs(text, i, kvs)) return false;

    // Missing keys stay at these sentinels so migrateStaleTunes() can see them.
    m_s.version = 1;
    m_s.tuneMatrix = 0;
    m_s.tuneStarfield = 0;
    m_s.tuneAurora = 0;
    m_s.tuneLava = 0;
    m_s.tuneApe = 0;
    m_s.tuneMyco = 0;
    m_s.tuneFireflies = 0;
    m_s.tuneSnow = 0;

    if (const Kv* v = findKv(kvs, "version")) m_s.version = parseInt(v->raw, 1);
    if (const Kv* v = findKv(kvs, "fps")) m_s.fps = parseInt(v->raw, m_s.fps);
    if (const Kv* v = findKv(kvs, "effect")) m_s.effect = unquote(v->raw);
    m_s.favorites.clear();
    if (const Kv* v = findKv(kvs, "favorites")) {
        size_t j = 0;
        const std::string& raw = v->raw;
        while (j < raw.size()) {
            if (raw[j] == '"') {
                std::string id;
                if (!parseString(raw, j, id)) break;
                if (!id.empty()) m_s.favorites.push_back(id);
            } else {
                ++j;
            }
        }
    }
    if (const Kv* v = findKv(kvs, "pauseOnFullscreen")) m_s.pauseOnFullscreen = (v->raw == "true");
    if (const Kv* v = findKv(kvs, "pauseOnCovered")) m_s.pauseOnCovered = (v->raw == "true");
    if (const Kv* v = findKv(kvs, "pauseOnBatterySaver")) m_s.pauseOnBatterySaver = (v->raw == "true");
    if (const Kv* v = findKv(kvs, "startWithWindows")) m_s.startWithWindows = (v->raw == "true");
    if (const Kv* v = findKv(kvs, "showWidget")) m_s.showWidget = (v->raw != "false");
    if (const Kv* v = findKv(kvs, "hideDesktopIcons")) m_s.hideDesktopIcons = (v->raw == "true");
    if (const Kv* v = findKv(kvs, "widgetMini")) m_s.widgetMini = (v->raw == "true");
    if (const Kv* v = findKv(kvs, "widgetX")) m_s.widgetX = parseInt(v->raw, m_s.widgetX);
    if (const Kv* v = findKv(kvs, "widgetY")) m_s.widgetY = parseInt(v->raw, m_s.widgetY);

    if (const Kv* v = findKv(kvs, "tune")) {
        size_t j = 0;
        std::vector<Kv> tk;
        if (parseObjectKvs(v->raw, j, tk)) {
            if (const Kv* c = findKv(tk, "matrix")) m_s.tuneMatrix = parseInt(c->raw, 0);
            if (const Kv* c = findKv(tk, "starfield")) m_s.tuneStarfield = parseInt(c->raw, 0);
            if (const Kv* c = findKv(tk, "aurora")) m_s.tuneAurora = parseInt(c->raw, 0);
            if (const Kv* c = findKv(tk, "lava")) m_s.tuneLava = parseInt(c->raw, 0);
            if (const Kv* c = findKv(tk, "ape")) m_s.tuneApe = parseInt(c->raw, 0);
            if (const Kv* c = findKv(tk, "myco")) m_s.tuneMyco = parseInt(c->raw, 0);
            if (const Kv* c = findKv(tk, "fireflies")) m_s.tuneFireflies = parseInt(c->raw, 0);
            if (const Kv* c = findKv(tk, "snow")) m_s.tuneSnow = parseInt(c->raw, 0);
        }
    } else {
        // Pre-1.5.0 files have no tune object. Only Code Ape's defaults actually
        // changed across 1.4.x; grandfather the others so lava colors etc. stay.
        m_s.tuneMatrix = kTuneMatrix;
        m_s.tuneStarfield = kTuneStarfield;
        m_s.tuneAurora = kTuneAurora;
        m_s.tuneLava = kTuneLava;
        m_s.tuneApe = 0;
        m_s.tuneMyco = 0;
        m_s.tuneFireflies = 0;
        m_s.tuneSnow = 0;
    }

    if (const Kv* v = findKv(kvs, "matrix")) {
        size_t j = 0;
        std::vector<Kv> mk;
        if (parseObjectKvs(v->raw, j, mk)) {
            if (const Kv* c = findKv(mk, "color"))
                m_s.matrix.color = hexFromString(unquote(c->raw), m_s.matrix.color);
            if (const Kv* c = findKv(mk, "headColor"))
                m_s.matrix.headColor = hexFromString(unquote(c->raw), m_s.matrix.headColor);
            if (const Kv* c = findKv(mk, "speed")) m_s.matrix.speed = parseFinite(c->raw, m_s.matrix.speed);
            if (const Kv* c = findKv(mk, "density")) m_s.matrix.density = parseFinite(c->raw, m_s.matrix.density);
            if (const Kv* c = findKv(mk, "glyphSize"))
                m_s.matrix.glyphSize = parseInt(c->raw, m_s.matrix.glyphSize);
            if (const Kv* c = findKv(mk, "trailLength"))
                m_s.matrix.trailLength = parseFinite(c->raw, m_s.matrix.trailLength);
            if (const Kv* c = findKv(mk, "fadePower"))
                m_s.matrix.fadePower = parseFinite(c->raw, m_s.matrix.fadePower);
            if (const Kv* c = findKv(mk, "mutate")) m_s.matrix.mutate = parseFinite(c->raw, m_s.matrix.mutate);
            if (const Kv* c = findKv(mk, "glyphSet")) m_s.matrix.glyphSet = parseInt(c->raw, m_s.matrix.glyphSet);
            if (const Kv* c = findKv(mk, "customGlyphs")) m_s.matrix.customGlyphs = readJsonString(c->raw);
            if (const Kv* c = findKv(mk, "depth")) m_s.matrix.depth = parseFinite(c->raw, m_s.matrix.depth);
            if (const Kv* c = findKv(mk, "bloom")) m_s.matrix.bloom = parseFinite(c->raw, m_s.matrix.bloom);
            if (const Kv* c = findKv(mk, "glitch")) m_s.matrix.glitch = parseFinite(c->raw, m_s.matrix.glitch);
            if (const Kv* c = findKv(mk, "direction"))
                m_s.matrix.direction = parseFinite(c->raw, m_s.matrix.direction);
            if (const Kv* c = findKv(mk, "wind")) m_s.matrix.wind = parseFinite(c->raw, m_s.matrix.wind);
            if (const Kv* c = findKv(mk, "region")) m_s.matrix.region = parseFinite(c->raw, m_s.matrix.region);
            readLook(mk, m_s.matrix.look);
        }
    }
    if (const Kv* v = findKv(kvs, "starfield")) {
        size_t j = 0;
        std::vector<Kv> mk;
        if (parseObjectKvs(v->raw, j, mk)) {
            if (const Kv* c = findKv(mk, "color"))
                m_s.starfield.color = hexFromString(unquote(c->raw), m_s.starfield.color);
            if (const Kv* c = findKv(mk, "speed")) m_s.starfield.speed = parseFinite(c->raw, m_s.starfield.speed);
            if (const Kv* c = findKv(mk, "density"))
                m_s.starfield.density = parseFinite(c->raw, m_s.starfield.density);
            if (const Kv* c = findKv(mk, "twinkle")) m_s.starfield.twinkle = parseFinite(c->raw, m_s.starfield.twinkle);
            if (const Kv* c = findKv(mk, "nebula")) m_s.starfield.nebula = parseFinite(c->raw, m_s.starfield.nebula);
            if (const Kv* c = findKv(mk, "warp")) m_s.starfield.warp = parseFinite(c->raw, m_s.starfield.warp);
            if (const Kv* c = findKv(mk, "direction"))
                m_s.starfield.direction = parseFinite(c->raw, m_s.starfield.direction);
            readLook(mk, m_s.starfield.look);
        }
    }
    if (const Kv* v = findKv(kvs, "aurora")) {
        size_t j = 0;
        std::vector<Kv> mk;
        if (parseObjectKvs(v->raw, j, mk)) {
            if (const Kv* c = findKv(mk, "colorA"))
                m_s.aurora.colorA = hexFromString(unquote(c->raw), m_s.aurora.colorA);
            if (const Kv* c = findKv(mk, "colorB"))
                m_s.aurora.colorB = hexFromString(unquote(c->raw), m_s.aurora.colorB);
            if (const Kv* c = findKv(mk, "speed")) m_s.aurora.speed = parseFinite(c->raw, m_s.aurora.speed);
            if (const Kv* c = findKv(mk, "height")) m_s.aurora.height = parseFinite(c->raw, m_s.aurora.height);
            if (const Kv* c = findKv(mk, "rays")) m_s.aurora.rays = parseFinite(c->raw, m_s.aurora.rays);
            if (const Kv* c = findKv(mk, "stars")) m_s.aurora.stars = (c->raw != "false");
            if (const Kv* c = findKv(mk, "horizon")) m_s.aurora.horizon = (c->raw == "true");
            readLook(mk, m_s.aurora.look);
        }
    }
    if (const Kv* v = findKv(kvs, "lava")) {
        size_t j = 0;
        std::vector<Kv> mk;
        if (parseObjectKvs(v->raw, j, mk)) {
            if (const Kv* c = findKv(mk, "lavaColor"))
                m_s.lava.lavaColor = hexFromString(unquote(c->raw), m_s.lava.lavaColor);
            if (const Kv* c = findKv(mk, "liquidColor"))
                m_s.lava.liquidColor = hexFromString(unquote(c->raw), m_s.lava.liquidColor);
            if (const Kv* c = findKv(mk, "blobCount"))
                m_s.lava.blobCount = parseInt(c->raw, m_s.lava.blobCount);
            if (const Kv* c = findKv(mk, "speed")) m_s.lava.speed = parseFinite(c->raw, m_s.lava.speed);
            if (const Kv* c = findKv(mk, "glow")) m_s.lava.glow = parseFinite(c->raw, m_s.lava.glow);
            if (const Kv* c = findKv(mk, "blobSize")) m_s.lava.blobSize = parseFinite(c->raw, m_s.lava.blobSize);
            if (const Kv* c = findKv(mk, "viscosity")) m_s.lava.viscosity = parseFinite(c->raw, m_s.lava.viscosity);
            if (const Kv* c = findKv(mk, "heat")) m_s.lava.heat = parseFinite(c->raw, m_s.lava.heat);
            if (const Kv* c = findKv(mk, "gradient")) m_s.lava.gradient = parseFinite(c->raw, m_s.lava.gradient);
            if (const Kv* c = findKv(mk, "lamp")) m_s.lava.lamp = parseFinite(c->raw, m_s.lava.lamp);
            readLook(mk, m_s.lava.look);
        }
    }
    if (const Kv* v = findKv(kvs, "ape")) {
        size_t j = 0;
        std::vector<Kv> mk;
        if (parseObjectKvs(v->raw, j, mk)) {
            if (const Kv* c = findKv(mk, "color"))
                m_s.ape.color = hexFromString(unquote(c->raw), m_s.ape.color);
            if (const Kv* c = findKv(mk, "bgDim")) m_s.ape.bgDim = parseFinite(c->raw, m_s.ape.bgDim);
            if (const Kv* c = findKv(mk, "apeBrightness"))
                m_s.ape.apeBrightness = parseFinite(c->raw, m_s.ape.apeBrightness);
            if (const Kv* c = findKv(mk, "size")) m_s.ape.size = parseFinite(c->raw, m_s.ape.size);
            if (const Kv* c = findKv(mk, "posX")) m_s.ape.posX = parseFinite(c->raw, m_s.ape.posX);
            if (const Kv* c = findKv(mk, "speed")) m_s.ape.speed = parseFinite(c->raw, m_s.ape.speed);
            if (const Kv* c = findKv(mk, "glyphSize"))
                m_s.ape.glyphSize = parseInt(c->raw, m_s.ape.glyphSize);
            if (const Kv* c = findKv(mk, "eyeGlow")) m_s.ape.eyeGlow = (c->raw == "true");
            if (const Kv* c = findKv(mk, "breathing")) m_s.ape.breathing = (c->raw == "true");
            if (const Kv* c = findKv(mk, "reveal")) m_s.ape.reveal = parseFinite(c->raw, m_s.ape.reveal);
            if (const Kv* c = findKv(mk, "detail")) m_s.ape.detail = parseFinite(c->raw, m_s.ape.detail);
            if (const Kv* c = findKv(mk, "breatheAmt")) m_s.ape.breatheAmt = parseFinite(c->raw, m_s.ape.breatheAmt);
            if (const Kv* c = findKv(mk, "fromImage")) m_s.ape.fromImage = (c->raw == "true");
            if (const Kv* c = findKv(mk, "imagePath")) m_s.ape.imagePath = readJsonString(c->raw);
            readLook(mk, m_s.ape.look);
        }
    }
    if (const Kv* v = findKv(kvs, "myco")) {
        size_t j = 0;
        std::vector<Kv> mk;
        if (parseObjectKvs(v->raw, j, mk)) {
            if (const Kv* c = findKv(mk, "color"))
                m_s.myco.color = hexFromString(unquote(c->raw), m_s.myco.color);
            if (const Kv* c = findKv(mk, "bgDim")) m_s.myco.bgDim = parseFinite(c->raw, m_s.myco.bgDim);
            if (const Kv* c = findKv(mk, "brightness"))
                m_s.myco.brightness = parseFinite(c->raw, m_s.myco.brightness);
            if (const Kv* c = findKv(mk, "size")) m_s.myco.size = parseFinite(c->raw, m_s.myco.size);
            if (const Kv* c = findKv(mk, "posX")) m_s.myco.posX = parseFinite(c->raw, m_s.myco.posX);
            if (const Kv* c = findKv(mk, "speed")) m_s.myco.speed = parseFinite(c->raw, m_s.myco.speed);
            if (const Kv* c = findKv(mk, "glyphSize"))
                m_s.myco.glyphSize = parseInt(c->raw, m_s.myco.glyphSize);
            if (const Kv* c = findKv(mk, "eyeGlow")) m_s.myco.eyeGlow = (c->raw == "true");
            if (const Kv* c = findKv(mk, "breathing")) m_s.myco.breathing = (c->raw == "true");
            if (const Kv* c = findKv(mk, "colorize")) m_s.myco.colorize = (c->raw != "false");
            if (const Kv* c = findKv(mk, "reveal")) m_s.myco.reveal = parseFinite(c->raw, m_s.myco.reveal);
            if (const Kv* c = findKv(mk, "detail")) m_s.myco.detail = parseFinite(c->raw, m_s.myco.detail);
            if (const Kv* c = findKv(mk, "breatheAmt")) m_s.myco.breatheAmt = parseFinite(c->raw, m_s.myco.breatheAmt);
            if (const Kv* c = findKv(mk, "imagePath")) m_s.myco.imagePath = readJsonString(c->raw);
            readLook(mk, m_s.myco.look);
        }
    }
    if (const Kv* v = findKv(kvs, "fireflies")) {
        size_t j = 0;
        std::vector<Kv> mk;
        if (parseObjectKvs(v->raw, j, mk)) {
            if (const Kv* c = findKv(mk, "color"))
                m_s.fireflies.color = hexFromString(unquote(c->raw), m_s.fireflies.color);
            if (const Kv* c = findKv(mk, "speed")) m_s.fireflies.speed = parseFinite(c->raw, m_s.fireflies.speed);
            if (const Kv* c = findKv(mk, "density"))
                m_s.fireflies.density = parseFinite(c->raw, m_s.fireflies.density);
            if (const Kv* c = findKv(mk, "mist")) m_s.fireflies.mist = parseFinite(c->raw, m_s.fireflies.mist);
            if (const Kv* c = findKv(mk, "stars")) m_s.fireflies.stars = parseFinite(c->raw, m_s.fireflies.stars);
            if (const Kv* c = findKv(mk, "woods")) m_s.fireflies.woods = parseFinite(c->raw, m_s.fireflies.woods);
            readLook(mk, m_s.fireflies.look);
        }
    }
    if (const Kv* v = findKv(kvs, "snow")) {
        size_t j = 0;
        std::vector<Kv> mk;
        if (parseObjectKvs(v->raw, j, mk)) {
            if (const Kv* c = findKv(mk, "color"))
                m_s.snow.color = hexFromString(unquote(c->raw), m_s.snow.color);
            if (const Kv* c = findKv(mk, "speed")) m_s.snow.speed = parseFinite(c->raw, m_s.snow.speed);
            if (const Kv* c = findKv(mk, "density")) m_s.snow.density = parseFinite(c->raw, m_s.snow.density);
            if (const Kv* c = findKv(mk, "wind")) m_s.snow.wind = parseFinite(c->raw, m_s.snow.wind);
            if (const Kv* c = findKv(mk, "backdrop")) m_s.snow.backdrop = parseInt(c->raw, m_s.snow.backdrop);
            readLook(mk, m_s.snow.look);
        }
    }
    return true;
}

std::string Config::serialize() const {
    std::ostringstream o;
    o << "{\n";
    o << "  \"version\": " << kFormatVersion << ",\n";
    o << "  \"tune\": {\n";
    o << "    \"matrix\": " << kTuneMatrix << ",\n";
    o << "    \"starfield\": " << kTuneStarfield << ",\n";
    o << "    \"aurora\": " << kTuneAurora << ",\n";
    o << "    \"lava\": " << kTuneLava << ",\n";
    o << "    \"ape\": " << kTuneApe << ",\n";
    o << "    \"myco\": " << kTuneMyco << ",\n";
    o << "    \"fireflies\": " << kTuneFireflies << ",\n";
    o << "    \"snow\": " << kTuneSnow << "\n";
    o << "  },\n";
    o << "  \"fps\": " << m_s.fps << ",\n";
    o << "  \"effect\": \"" << jsonEscape(m_s.effect) << "\",\n";
    o << "  \"favorites\": [";
    for (size_t fi = 0; fi < m_s.favorites.size(); ++fi) {
        if (fi) o << ", ";
        o << "\"" << m_s.favorites[fi] << "\"";
    }
    o << "],\n";
    o << "  \"pauseOnFullscreen\": " << (m_s.pauseOnFullscreen ? "true" : "false") << ",\n";
    o << "  \"pauseOnCovered\": " << (m_s.pauseOnCovered ? "true" : "false") << ",\n";
    o << "  \"pauseOnBatterySaver\": " << (m_s.pauseOnBatterySaver ? "true" : "false") << ",\n";
    o << "  \"startWithWindows\": " << (m_s.startWithWindows ? "true" : "false") << ",\n";
    o << "  \"showWidget\": " << (m_s.showWidget ? "true" : "false") << ",\n";
    o << "  \"hideDesktopIcons\": " << (m_s.hideDesktopIcons ? "true" : "false") << ",\n";
    o << "  \"widgetMini\": " << (m_s.widgetMini ? "true" : "false") << ",\n";
    o << "  \"widgetX\": " << m_s.widgetX << ",\n";
    o << "  \"widgetY\": " << m_s.widgetY << ",\n";
    o << "  \"matrix\": {\n";
    o << "    \"color\": \"" << hexToString(m_s.matrix.color) << "\",\n";
    o << "    \"headColor\": \"" << hexToString(m_s.matrix.headColor) << "\",\n";
    o << "    \"speed\": " << m_s.matrix.speed << ",\n";
    o << "    \"density\": " << m_s.matrix.density << ",\n";
    o << "    \"glyphSize\": " << m_s.matrix.glyphSize << ",\n";
    o << "    \"trailLength\": " << m_s.matrix.trailLength << ",\n";
    o << "    \"fadePower\": " << m_s.matrix.fadePower << ",\n";
    o << "    \"mutate\": " << m_s.matrix.mutate << ",\n";
    o << "    \"glyphSet\": " << m_s.matrix.glyphSet << ",\n";
    o << "    \"customGlyphs\": \"" << jsonEscape(m_s.matrix.customGlyphs) << "\",\n";
    o << "    \"depth\": " << m_s.matrix.depth << ",\n";
    o << "    \"bloom\": " << m_s.matrix.bloom << ",\n";
    o << "    \"glitch\": " << m_s.matrix.glitch << ",\n";
    o << "    \"direction\": " << m_s.matrix.direction << ",\n";
    o << "    \"wind\": " << m_s.matrix.wind << ",\n";
    o << "    \"region\": " << m_s.matrix.region << ",\n";
    writeLook(o, m_s.matrix.look);
    o << "  },\n";
    o << "  \"starfield\": {\n";
    o << "    \"color\": \"" << hexToString(m_s.starfield.color) << "\",\n";
    o << "    \"speed\": " << m_s.starfield.speed << ",\n";
    o << "    \"density\": " << m_s.starfield.density << ",\n";
    o << "    \"twinkle\": " << m_s.starfield.twinkle << ",\n";
    o << "    \"nebula\": " << m_s.starfield.nebula << ",\n";
    o << "    \"warp\": " << m_s.starfield.warp << ",\n";
    o << "    \"direction\": " << m_s.starfield.direction << ",\n";
    writeLook(o, m_s.starfield.look);
    o << "  },\n";
    o << "  \"aurora\": {\n";
    o << "    \"colorA\": \"" << hexToString(m_s.aurora.colorA) << "\",\n";
    o << "    \"colorB\": \"" << hexToString(m_s.aurora.colorB) << "\",\n";
    o << "    \"speed\": " << m_s.aurora.speed << ",\n";
    o << "    \"height\": " << m_s.aurora.height << ",\n";
    o << "    \"rays\": " << m_s.aurora.rays << ",\n";
    o << "    \"stars\": " << (m_s.aurora.stars ? "true" : "false") << ",\n";
    o << "    \"horizon\": " << (m_s.aurora.horizon ? "true" : "false") << ",\n";
    writeLook(o, m_s.aurora.look);
    o << "  },\n";
    o << "  \"lava\": {\n";
    o << "    \"lavaColor\": \"" << hexToString(m_s.lava.lavaColor) << "\",\n";
    o << "    \"liquidColor\": \"" << hexToString(m_s.lava.liquidColor) << "\",\n";
    o << "    \"blobCount\": " << m_s.lava.blobCount << ",\n";
    o << "    \"speed\": " << m_s.lava.speed << ",\n";
    o << "    \"glow\": " << m_s.lava.glow << ",\n";
    o << "    \"blobSize\": " << m_s.lava.blobSize << ",\n";
    o << "    \"viscosity\": " << m_s.lava.viscosity << ",\n";
    o << "    \"heat\": " << m_s.lava.heat << ",\n";
    o << "    \"gradient\": " << m_s.lava.gradient << ",\n";
    o << "    \"lamp\": " << m_s.lava.lamp << ",\n";
    writeLook(o, m_s.lava.look);
    o << "  },\n";
    o << "  \"ape\": {\n";
    o << "    \"color\": \"" << hexToString(m_s.ape.color) << "\",\n";
    o << "    \"bgDim\": " << m_s.ape.bgDim << ",\n";
    o << "    \"apeBrightness\": " << m_s.ape.apeBrightness << ",\n";
    o << "    \"size\": " << m_s.ape.size << ",\n";
    o << "    \"posX\": " << m_s.ape.posX << ",\n";
    o << "    \"speed\": " << m_s.ape.speed << ",\n";
    o << "    \"glyphSize\": " << m_s.ape.glyphSize << ",\n";
    o << "    \"eyeGlow\": " << (m_s.ape.eyeGlow ? "true" : "false") << ",\n";
    o << "    \"breathing\": " << (m_s.ape.breathing ? "true" : "false") << ",\n";
    o << "    \"reveal\": " << m_s.ape.reveal << ",\n";
    o << "    \"detail\": " << m_s.ape.detail << ",\n";
    o << "    \"breatheAmt\": " << m_s.ape.breatheAmt << ",\n";
    o << "    \"fromImage\": " << (m_s.ape.fromImage ? "true" : "false") << ",\n";
    o << "    \"imagePath\": \"" << jsonEscape(m_s.ape.imagePath) << "\",\n";
    writeLook(o, m_s.ape.look);
    o << "  },\n";
    o << "  \"myco\": {\n";
    o << "    \"color\": \"" << hexToString(m_s.myco.color) << "\",\n";
    o << "    \"bgDim\": " << m_s.myco.bgDim << ",\n";
    o << "    \"brightness\": " << m_s.myco.brightness << ",\n";
    o << "    \"size\": " << m_s.myco.size << ",\n";
    o << "    \"posX\": " << m_s.myco.posX << ",\n";
    o << "    \"speed\": " << m_s.myco.speed << ",\n";
    o << "    \"glyphSize\": " << m_s.myco.glyphSize << ",\n";
    o << "    \"eyeGlow\": " << (m_s.myco.eyeGlow ? "true" : "false") << ",\n";
    o << "    \"breathing\": " << (m_s.myco.breathing ? "true" : "false") << ",\n";
    o << "    \"colorize\": " << (m_s.myco.colorize ? "true" : "false") << ",\n";
    o << "    \"reveal\": " << m_s.myco.reveal << ",\n";
    o << "    \"detail\": " << m_s.myco.detail << ",\n";
    o << "    \"breatheAmt\": " << m_s.myco.breatheAmt << ",\n";
    o << "    \"imagePath\": \"" << jsonEscape(m_s.myco.imagePath) << "\",\n";
    writeLook(o, m_s.myco.look);
    o << "  },\n";
    o << "  \"fireflies\": {\n";
    o << "    \"color\": \"" << hexToString(m_s.fireflies.color) << "\",\n";
    o << "    \"speed\": " << m_s.fireflies.speed << ",\n";
    o << "    \"density\": " << m_s.fireflies.density << ",\n";
    o << "    \"mist\": " << m_s.fireflies.mist << ",\n";
    o << "    \"stars\": " << m_s.fireflies.stars << ",\n";
    o << "    \"woods\": " << m_s.fireflies.woods << ",\n";
    writeLook(o, m_s.fireflies.look);
    o << "  },\n";
    o << "  \"snow\": {\n";
    o << "    \"color\": \"" << hexToString(m_s.snow.color) << "\",\n";
    o << "    \"speed\": " << m_s.snow.speed << ",\n";
    o << "    \"density\": " << m_s.snow.density << ",\n";
    o << "    \"wind\": " << m_s.snow.wind << ",\n";
    o << "    \"backdrop\": " << m_s.snow.backdrop << ",\n";
    writeLook(o, m_s.snow.look);
    o << "  }\n";
    o << "}\n";
    return o.str();
}
