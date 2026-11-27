#pragma once

#include <algorithm>
#include <cstdint>

namespace fullwintb::appearance {

enum class ReturnStyle { elastic, simple };

struct MotionSettings {
    int mass = 100;      // All four values are stored in hundredths.
    int damping = 3504;
    int click = 500;
    int reach = 85;
    int wheelStep = 32;  // Device-independent pixels per detent.
    int hold = 1200;    // Milliseconds after arriving at the displaced position.

    bool operator==(const MotionSettings&) const = default;
};

inline constexpr MotionSettings minimumMotionSettings{15, 200, 100, 20, 12, 0};
inline constexpr MotionSettings maximumMotionSettings{800, 16000, 1400, 300, 64, 3000};
inline constexpr std::uint64_t motionSettingsVersion = 1ull << 63;
inline constexpr std::uint64_t motionSettingsExtended = 1ull << 62;

inline constexpr MotionSettings ClampMotionSettings(const MotionSettings& settings) {
    return {
        std::clamp(settings.mass, minimumMotionSettings.mass, maximumMotionSettings.mass),
        std::clamp(settings.damping, minimumMotionSettings.damping, maximumMotionSettings.damping),
        std::clamp(settings.click, minimumMotionSettings.click, maximumMotionSettings.click),
        std::clamp(settings.reach, minimumMotionSettings.reach, maximumMotionSettings.reach),
        std::clamp(settings.wheelStep, minimumMotionSettings.wheelStep, maximumMotionSettings.wheelStep),
        std::clamp(settings.hold, minimumMotionSettings.hold, maximumMotionSettings.hold),
    };
}

inline constexpr std::uint64_t PackMotionSettings(const MotionSettings& settings) {
    const auto values = ClampMotionSettings(settings);
    // The extended packet uses 10, 14, 11, 9, 6 and 12 bits, in field order.
    return motionSettingsVersion | motionSettingsExtended | static_cast<std::uint64_t>(values.mass) |
        (static_cast<std::uint64_t>(values.damping) << 10) |
        (static_cast<std::uint64_t>(values.click) << 24) |
        (static_cast<std::uint64_t>(values.reach) << 35) |
        (static_cast<std::uint64_t>(values.wheelStep - minimumMotionSettings.wheelStep) << 44) |
        (static_cast<std::uint64_t>(values.hold) << 50);
}

inline constexpr MotionSettings UnpackMotionSettings(std::uint64_t packet) {
    if (!(packet & motionSettingsVersion)) return {};
    // Bit 62 was zero for every valid legacy reach value.
    if (packet & motionSettingsExtended) return ClampMotionSettings({
        static_cast<int>(packet & 0x3ff),
        static_cast<int>((packet >> 10) & 0x3fff),
        static_cast<int>((packet >> 24) & 0x7ff),
        static_cast<int>((packet >> 35) & 0x1ff),
        static_cast<int>((packet >> 44) & 0x3f) + minimumMotionSettings.wheelStep,
        static_cast<int>((packet >> 50) & 0xfff),
    });
    return ClampMotionSettings({
        static_cast<int>(packet & 0xffff),
        static_cast<int>((packet >> 16) & 0xffff),
        static_cast<int>((packet >> 32) & 0xffff),
        static_cast<int>((packet >> 48) & 0x7fff),
    });
}

static_assert(UnpackMotionSettings(PackMotionSettings({})) == MotionSettings{});

} // namespace fullwintb::appearance
