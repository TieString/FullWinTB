#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <windows.h>

namespace fullwintb::appearance {

using MotionClock = std::chrono::steady_clock;

inline std::chrono::milliseconds IconDelay(LONG distance, UINT dpi) {
    const auto milliseconds = std::lround(std::abs(distance) * 22.0 * 96.0 / (100.0 * dpi));
    return std::chrono::milliseconds(std::min(96L, milliseconds));
}

// One history per icon group keeps the current spring profile and input
// behavior. Individual buttons read that path at their own time offsets.
class IconTrail {
public:
    void Start(MotionClock::time_point now, LONG offset) {
        count_ = newest_ = 0;
        Record(now, offset);
    }

    void Record(MotionClock::time_point now, LONG offset) {
        if (!count_ || offset != frames_[newest_].offset) lastChange_ = now;
        if (!count_ || now > frames_[newest_].time) {
            newest_ = (newest_ + 1) % frames_.size();
            count_ = std::min(count_ + 1, frames_.size());
        }
        frames_[newest_] = {now, offset};
    }

    LONG At(MotionClock::time_point at) const {
        if (!count_) return 0;
        std::size_t index = newest_;
        for (std::size_t n = 1; n < count_ && at < frames_[index].time; ++n) {
            const auto& after = frames_[index];
            index = (index + frames_.size() - 1) % frames_.size();
            const auto& before = frames_[index];
            if (at >= before.time) {
                const double f = std::chrono::duration<double>(at - before.time).count() /
                                 std::chrono::duration<double>(after.time - before.time).count();
                return std::lround(std::lerp(static_cast<double>(before.offset), static_cast<double>(after.offset), f));
            }
        }
        return frames_[index].offset;
    }

    bool Pending(MotionClock::time_point now, MotionClock::duration delay) const {
        return count_ && now < lastChange_ + delay;
    }

private:
    struct Frame {
        MotionClock::time_point time{};
        LONG offset = 0;
    };
    std::array<Frame, 96> frames_{};
    std::size_t newest_ = 0;
    std::size_t count_ = 0;
    MotionClock::time_point lastChange_{};
};

} // namespace fullwintb::appearance
