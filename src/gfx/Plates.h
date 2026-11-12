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
struct PlateLayers {
    ComPtr<ID3D11ShaderResourceView> layer[4];
    float mask = 0.0f;  // bit 0 sky, 1 mountains, 2 mid, 3 foreground
};

bool loadPlateLayers(ID3D11Device* device, const wchar_t* effect, const wchar_t* preset, PlateLayers& out);
