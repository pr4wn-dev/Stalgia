#pragma once

#include "effect/IEffect.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct EffectInfo {
    std::string id;
    std::string displayName;
    std::string group;
    std::string description;
};

class EffectRegistry {
public:
    using Factory = std::function<std::unique_ptr<IEffect>()>;

    EffectRegistry();

    std::vector<EffectInfo> list() const;
    std::vector<std::string> groups() const;
    const EffectInfo* find(const std::string& id) const;
    std::unique_ptr<IEffect> create(const std::string& id) const;
    bool contains(const std::string& id) const;

    // Used by sources compiled from STALGIA_PRIVATE_EFFECTS_DIR.
    void addExternal(const char* id, const char* displayName, const char* group, const char* description,
                     Factory factory);

private:
    void add(const char* id, const char* displayName, const char* group, const char* description,
             Factory factory);

    struct Entry {
        EffectInfo info;
        Factory factory;
    };
    std::vector<Entry> m_entries;
};
