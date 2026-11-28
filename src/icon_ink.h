#pragma once

#include "appearance.h"

#include <algorithm>
#include <cstdint>
#include <span>

namespace fullwintb::appearance {

// RenderTargetBitmap supplies premultiplied BGRA. A suitable icon has a
// transparent surround and a single, near-black or near-white foreground.
inline Ink ClassifyIconInk(std::span<const std::uint8_t> pixels) {
    if (pixels.empty() || pixels.size() % 4) return Ink::system;
    unsigned transparent = 0, opaque = 0, black = 0, white = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const unsigned alpha = pixels[i + 3];
        if (alpha <= 16) ++transparent;
        if (alpha < 32) continue;
        const unsigned red = pixels[i + 2];
        const unsigned green = pixels[i + 1];
        const unsigned blue = pixels[i];
        const unsigned first = std::min({red, green, blue});
        const unsigned last = std::max({red, green, blue});
        if ((last - first) * 255 > 14 * alpha) return Ink::system;
        if (alpha < 128) continue;
        ++opaque;
        if (last * 255 <= 52 * alpha) ++black;
        else if (first * 255 >= 205 * alpha) ++white;
    }
    const auto count = pixels.size() / 4;
    if (opaque < 16 || transparent * 10 < count) return Ink::system;
    if (!white && black * 20 >= opaque * 19) return Ink::black;
    if (!black && white * 20 >= opaque * 19) return Ink::white;
    return Ink::system;
}

} // namespace fullwintb::appearance
