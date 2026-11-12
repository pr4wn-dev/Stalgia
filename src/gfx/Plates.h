#pragma once

#include "core/ComPtr.h"

#include <d3d11.h>

// Optional photoreal backdrop plates. Not shipped in the repo.
// Looked up, in order, under STALGIA_PLATES_DIR, <exe>\plates, and the
// app-data plates folder:
//
//   plates/<effect>/<preset>/{far,mid,near}.png   (jpg and jpeg too)
//   plates/<effect>/sky/{far,mid,near}.png        (fallback per layer)
//
// far is the distant opaque plate. mid and near are closer and use alpha.
// Effects draw on top. Missing files leave the procedural stand-in.
struct PlateLayers {
    ComPtr<ID3D11ShaderResourceView> layer[3];
    float mask = 0.0f;  // bit 0 far, bit 1 mid, bit 2 near
};

bool loadPlateLayers(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset, PlateLayers& out);
