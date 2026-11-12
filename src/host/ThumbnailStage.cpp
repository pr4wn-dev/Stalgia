#include "host/ThumbnailStage.h"

#include "core/ComPtr.h"
#include "core/Config.h"
#include "core/Log.h"
#include "effect/EffectRegistry.h"
#include "effect/Presets.h"
#include "effect/IEffect.h"
#include "gfx/Renderer.h"

#include <cstring>

#include <d3d11.h>

namespace {
constexpr unsigned long long kIntervalMs = 80;  // ~12 fps
}

void ThumbnailStage::freeSlot(Slot& slot) {
    if (slot.fx) {
        auto* fx = static_cast<IEffect*>(slot.fx);
        fx->release();
        delete fx;
        slot.fx = nullptr;
    }
    if (slot.rtv) {
        static_cast<IUnknown*>(slot.rtv)->Release();
        slot.rtv = nullptr;
    }
    if (slot.tex) {
        static_cast<IUnknown*>(slot.tex)->Release();
        slot.tex = nullptr;
    }
    if (slot.stage) {
        static_cast<IUnknown*>(slot.stage)->Release();
        slot.stage = nullptr;
    }
    if (slot.stageB) {
        static_cast<IUnknown*>(slot.stageB)->Release();
        slot.stageB = nullptr;
    }
    slot.time = 0;
    slot.readPending = false;
    slot.writeStage = 0;
    slot.ready = false;
    slot.tuneGen = 0xFFFFFFFFu;
}

void ThumbnailStage::release() {
    for (auto& s : m_slots) freeSlot(s);
    m_slots.clear();
}

ThumbnailStage::Slot* ThumbnailStage::slotFor(const std::string& id) {
    for (auto& s : m_slots)
        if (s.id == id) return &s;
    return nullptr;
}

const ThumbnailStage::Slot* ThumbnailStage::slotFor(const std::string& id) const {
    for (const auto& s : m_slots)
        if (s.id == id) return &s;
    return nullptr;
}

bool ThumbnailStage::ready(const std::string& id) const {
    const Slot* s = slotFor(id);
    return s && s->ready;
}

bool ThumbnailStage::failed(const std::string& id) const {
    const Slot* s = slotFor(id);
    return s && s->failed;
}

bool ThumbnailStage::settled(const std::string& id) const {
    const Slot* s = slotFor(id);
    return s && (s->ready || s->failed);
}

uint32_t ThumbnailStage::generation(const std::string& id) const {
    const Slot* s = slotFor(id);
    return s ? s->generation : 0;
}

const uint8_t* ThumbnailStage::pixels(const std::string& id) const {
    const Slot* s = slotFor(id);
    if (!s || !s->ready || s->bgra.empty()) return nullptr;
    return s->bgra.data();
}

bool ThumbnailStage::ensureSlot(Renderer* renderer, const EffectRegistry& registry, const AppSettings& settings,
                                Slot& slot, uint32_t tuneGen) {
    if (slot.failed) return false;
    if (slot.fx && slot.rtv && slot.stage) return true;
    if (!renderer || !renderer->device() || !renderer->context()) return false;

    auto fx = registry.create(slot.id);
    if (!fx || fx->id() != slot.id) {
        slot.failed = true;
        LOG_WARN("Thumbnail: no effect '%s'", slot.id.c_str());
        return false;
    }
    if (!fx->initialize(renderer->device(), renderer->context())) {
        slot.failed = true;
        LOG_WARN("Thumbnail init failed '%s'", slot.id.c_str());
        fx->release();
        return false;
    }
    fx->applySettings(settings);
    fx->resize(kWidth, kHeight, 1.0f);
    fx->precompile(renderer);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* stage = nullptr;
    ID3D11Texture2D* stageB = nullptr;
    HRESULT hr = renderer->device()->CreateTexture2D(&td, nullptr, &tex);
    if (SUCCEEDED(hr)) hr = renderer->device()->CreateRenderTargetView(tex, nullptr, &rtv);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = renderer->device()->CreateTexture2D(&td, nullptr, &stage);
    if (SUCCEEDED(hr)) hr = renderer->device()->CreateTexture2D(&td, nullptr, &stageB);
    if (FAILED(hr) || !tex || !rtv || !stage || !stageB) {
        if (rtv) rtv->Release();
        if (tex) tex->Release();
        if (stage) stage->Release();
        if (stageB) stageB->Release();
        fx->release();
        slot.failed = true;
        LOG_WARN("Thumbnail target failed '%s' hr=0x%08X", slot.id.c_str(), (unsigned)hr);
        return false;
    }
    slot.fx = fx.release();
    slot.tex = tex;
    slot.rtv = rtv;
    slot.stage = stage;
    slot.stageB = stageB;
    slot.tuneGen = tuneGen;
    slot.bgra.assign((size_t)kWidth * (size_t)kHeight * 4, 0);
    return true;
}

bool ThumbnailStage::tick(Renderer* renderer, const EffectRegistry& registry, const AppSettings& settings,
                          bool gallery, bool widget, const std::string& widgetEffect, uint32_t tuneGen) {
    if (!gallery && !widget) return false;
    if (!renderer || !renderer->device()) return false;

    if (m_slots.empty()) {
        for (const auto& info : registry.list()) {
            Slot s;
            s.id = info.id;
            m_slots.push_back(std::move(s));
        }
    }

    const unsigned long long now = GetTickCount64();
    bool changed = false;
    for (Slot& slot : m_slots) {
        const bool want = gallery || (widget && slot.id == widgetEffect);
        if (!want) continue;
        if (!ensureSlot(renderer, registry, settings, slot, tuneGen)) continue;
        if (slot.nextMs != 0 && now < slot.nextMs) continue;
        const size_t index = (size_t)(&slot - m_slots.data());
        slot.nextMs = now + kIntervalMs + (slot.generation == 0 ? index * 12ull : 0ull);

        auto* fx = static_cast<IEffect*>(slot.fx);
        auto* rtv = static_cast<ID3D11RenderTargetView*>(slot.rtv);
        // Reapply only when the user changes a knob. Doing it every tick resets rain columns
        // and the thumbnail never moves.
        if (slot.tuneGen != tuneGen) {
            fx->applySettings(settings);
            fx->resize(kWidth, kHeight, 1.0f);
            slot.tuneGen = tuneGen;
        }
        slot.time += 1.0 / 12.0;
        const double time = slot.time;
        Renderer::Grade grade;
        if (const EffectLook* look = effectLook(settings, slot.id)) {
            grade.brightness = look->brightness;
            grade.saturation = look->saturation;
            grade.vignette = look->vignette;
            grade.night = nightAmount(look->timeOfDay);
        }
        renderer->setGrade(grade);
        renderer->beginOffscreen(rtv, kWidth, kHeight);
        renderer->clear(0.01f, 0.012f, 0.02f, 1.0f);
        fx->update(1.0f / 12.0f, time);
        FrameContext ctx;
        ctx.renderer = renderer;
        ctx.device = renderer->device();
        ctx.context = renderer->context();
        ctx.rtv = rtv;
        ctx.width = kWidth;
        ctx.height = kHeight;
        ctx.dpiScale = 1.0f;
        ctx.primaryHeight = 1080;
        ctx.dt = 1.0f / 12.0f;
        ctx.time = time;
        fx->render(ctx);
        renderer->endOffscreen();

        ID3D11Resource* stages[2] = {static_cast<ID3D11Resource*>(slot.stage),
                                     static_cast<ID3D11Resource*>(slot.stageB)};
        const int write = slot.writeStage & 1;
        if (slot.readPending) {
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT hr = renderer->context()->Map(stages[1 - write], 0, D3D11_MAP_READ,
                                                        D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                // Leave the previous copy in place and overwrite this frame's target next tick.
            } else if (SUCCEEDED(hr)) {
                for (int y = 0; y < kHeight; ++y) {
                    const auto* src = static_cast<const uint8_t*>(map.pData) + (size_t)y * map.RowPitch;
                    std::memcpy(slot.bgra.data() + (size_t)y * kWidth * 4, src, (size_t)kWidth * 4);
                }
                renderer->context()->Unmap(stages[1 - write], 0);
                slot.readPending = false;
                slot.ready = true;
                slot.generation++;
                changed = true;
                renderer->context()->CopyResource(stages[write], static_cast<ID3D11Resource*>(slot.tex));
                slot.writeStage ^= 1;
                slot.readPending = true;
            } else {
                slot.readPending = false;
            }
        } else {
            renderer->context()->CopyResource(stages[write], static_cast<ID3D11Resource*>(slot.tex));
            slot.writeStage ^= 1;
            slot.readPending = true;
        }
    }
    return changed;
}
