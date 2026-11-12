#pragma once

#include <cstdint>
#include <string>
#include <vector>

class Renderer;
class EffectRegistry;
struct AppSettings;

// Small offscreen renders of each wallpaper. Nothing is drawn unless a caller
// asks for the gallery set or the single widget thumbnail.
class ThumbnailStage {
public:
    static constexpr int kWidth = 192;
    static constexpr int kHeight = 108;

    void release();
    // Renders due slots. Returns true when at least one CPU image changed.
    bool tick(Renderer* renderer, const EffectRegistry& registry, const AppSettings& settings, bool gallery,
              bool widget, const std::string& widgetEffect, uint32_t tuneGen);

    bool ready(const std::string& id) const;
    bool failed(const std::string& id) const;
    bool settled(const std::string& id) const;
    uint32_t generation(const std::string& id) const;
    const uint8_t* pixels(const std::string& id) const;

private:
    struct Slot {
        std::string id;
        void* fx = nullptr;  // IEffect*, owned
        void* tex = nullptr;
        void* rtv = nullptr;
        void* stage = nullptr;
        void* stageB = nullptr;
        int writeStage = 0;
        bool readPending = false;
        double time = 0;
        std::vector<uint8_t> bgra;
        uint32_t generation = 0;
        unsigned long long nextMs = 0;
        bool ready = false;
        bool failed = false;
        uint32_t tuneGen = 0xFFFFFFFFu;
    };

    Slot* slotFor(const std::string& id);
    const Slot* slotFor(const std::string& id) const;
    bool ensureSlot(Renderer* renderer, const EffectRegistry& registry, const AppSettings& settings, Slot& slot,
                    uint32_t tuneGen);
    void freeSlot(Slot& slot);

    std::vector<Slot> m_slots;
};
