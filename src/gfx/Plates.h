#pragma once

#include "core/ComPtr.h"

#include <d3d11.h>

// Photoreal backdrop plates, four layers back to front:
//   0 sky (opaque), 1 mountains, 2 mid, 3 foreground.
// The shipped WebP sets are embedded resources and decoded with WIC.
// A folder can still override them:
//   STALGIA_PLATES_DIR, <exe>\plates, or the app-data plates folder
//   plates/<effect>/<preset>/layer-a-sky.webp
//   layer-b-mountains, layer-c-mid, layer-d-foreground
// PNG and JPEG are accepted too. Alpha at or above 250 is snapped to 255.
// The aurora set is desaturated a little toward neutral so the animated
// curtain reads as the light on the peaks and ice.
//
// startPlatePreload decodes the shipped sets on a worker thread. The frame
// loop uploads them once and caches the GPU textures, so a preset change
// does not decode. Still night and Milky Way share one set.
struct PlateLayers {
    ComPtr<ID3D11ShaderResourceView> layer[4];
    float mask = 0.0f;  // bit 0 sky, 1 mountains, 2 mid, 3 foreground
    // True when this preset has plates that are still decoding or uploading.
    // The caller should draw without them and ask again next frame.
    bool pending = false;
};

void startPlatePreload();
void releasePlateCache();
// Uploads at most one decoded set that is not on this device yet.
void servicePlateUploads(ID3D11Device* device);
// Cached lookup. Never decodes on the calling thread for a set the worker owns.
// False and pending: try again next frame. False and not pending: this preset has no plates.
bool acquirePlateLayers(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset, PlateLayers& out);
