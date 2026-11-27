#pragma once

namespace fullwintb::appearance {

inline constexpr int stickerWidth = 112;
inline constexpr int stickerHeight = 64;
inline constexpr int stickerFrames = 13;
inline constexpr int stickerPaletteBytes = 256 * 4;
inline constexpr int stickerFrameBytes = stickerWidth * stickerHeight;
inline constexpr int stickerAtlasBytes = stickerPaletteBytes + stickerFrameBytes * stickerFrames;

} // namespace fullwintb::appearance
