#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Decode a PNG or JPEG and build a rain-reveal mask. Long side is capped at 256.
bool loadImageMask(const std::wstring& path, std::vector<uint8_t>& luma, std::vector<uint8_t>& rgb, int& w, int& h);
