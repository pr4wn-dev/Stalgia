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
// loop uploads one layer per frame and caches the GPU textures. A preset
// change never decodes and never waits: until the photo is on the GPU the
// effect draws its procedural frame, then the photo fades in. Still night
// and Milky Way share one set.
struct PlateLayers {
    ComPtr<ID3D11ShaderResourceView> layer[4];
    float mask = 0.0f;  // bit 0 sky, 1 mountains, 2 mid, 3 foreground
    // True when this preset has plates that are still decoding or uploading.
    // The caller should draw without them and ask again next frame.
    bool pending = false;
};

void startPlatePreload();
void releasePlateCache();
// Uploads at most one decoded layer that is not on this device yet.
void servicePlateUploads(ID3D11Device* device);
// Cached lookup. Never decodes or uploads on this call.
// False and pending: try again next frame. False and not pending: this preset has no plates.
bool acquirePlateLayers(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset, PlateLayers& out);
// True when this preset's plates are on the device, or the preset has none.
bool plateSetReady(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset);
// True when every queued set has finished decoding and uploading.
bool plateUploadsIdle(ID3D11Device* device);
