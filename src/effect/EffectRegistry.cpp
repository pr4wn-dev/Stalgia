#include "effect/EffectRegistry.h"

#include "effects/Aurora.h"
#include "effects/CodeApe.h"
#include "effects/LavaLamp.h"
#include "effects/MatrixRain.h"
#include "effects/SlowMyco.h"
#include "effects/Starfield.h"

void EffectRegistry::add(const char* id, const char* displayName, const char* group, const char* description,
                         Factory factory) {
    Entry e;
    e.info.id = id;
    e.info.displayName = displayName;
    e.info.group = group;
    e.info.description = description;
    e.factory = std::move(factory);
    m_entries.push_back(std::move(e));
}

EffectRegistry::EffectRegistry() {
    // New wallpapers only need a group and a one-line description here.
    add("matrix", "Matrix (Classic)", "Matrix Rain", "Classic falling katakana rain.",
        [] { return std::unique_ptr<IEffect>(new MatrixRain()); });
    add("ape", "Code Ape", "Matrix Rain", "Rain that reveals a silverback.",
        [] { return std::unique_ptr<IEffect>(new CodeApe()); });
    add("myco", "Slow Myco", "Matrix Rain", "Rain that reveals a mushroom turtle.",
        [] { return std::unique_ptr<IEffect>(new SlowMyco()); });
    add("starfield", "Stars", "Space", "Deep night sky, the Milky Way, and a warp mode.",
        [] { return std::unique_ptr<IEffect>(new Starfield()); });
    add("aurora", "Aurora", "Space", "Folded ribbons with a bright green hem and rising rays.",
        [] { return std::unique_ptr<IEffect>(new Aurora()); });
    add("lava", "Lava", "Retro", "You are inside the lamp.",
        [] { return std::unique_ptr<IEffect>(new LavaLamp()); });
}

std::vector<EffectInfo> EffectRegistry::list() const {
    std::vector<EffectInfo> out;
    out.reserve(m_entries.size());
    for (const auto& e : m_entries) out.push_back(e.info);
    return out;
}

std::vector<std::string> EffectRegistry::groups() const {
    std::vector<std::string> out;
    for (const auto& e : m_entries) {
        bool seen = false;
        for (const auto& g : out)
            if (g == e.info.group) seen = true;
        if (!seen && !e.info.group.empty()) out.push_back(e.info.group);
    }
    return out;
}

const EffectInfo* EffectRegistry::find(const std::string& id) const {
    for (const auto& e : m_entries)
        if (e.info.id == id) return &e.info;
    return nullptr;
}

std::unique_ptr<IEffect> EffectRegistry::create(const std::string& id) const {
    for (const auto& e : m_entries) {
        if (e.info.id == id) return e.factory();
    }
    if (!m_entries.empty()) return m_entries.front().factory();
    return nullptr;
}

bool EffectRegistry::contains(const std::string& id) const {
    for (const auto& e : m_entries)
        if (e.info.id == id) return true;
    return false;
}
