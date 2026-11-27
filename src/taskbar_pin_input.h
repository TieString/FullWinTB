#pragma once

#include "taskbar_layout.h"

#include <optional>

namespace fullwintb::appearance {

class PinInput {
public:
    enum class Origin { wheel, startDrag };
    struct Result {
        bool consumed = false;
        bool changed = false;
        std::optional<IconGroup> pin;
    };

    void Arm(IconGroup group, Origin origin, POINT anchor, ULONGLONG now, UINT duration);
    void Reset() { *this = {}; }
    void Expire(ULONGLONG now);
    bool Active() const { return phase_ != Phase::idle; }
    bool Armed() const { return phase_ == Phase::armed; }
    Origin Source() const { return origin_; }
    IconGroup Group() const { return group_; }
    POINT Anchor() const { return anchor_; }
    UINT Remaining(ULONGLONG now) const;
    Result Route(UINT message, POINT cursor, bool eligible, UINT dpi, ULONGLONG now);

    static constexpr ULONG_PTR replayTag = 0x41754252;

private:
    enum class Phase { idle, armed, firstDown, between, secondDown, discardUp };

    void Replay(bool released) const;

    Phase phase_ = Phase::idle;
    Origin origin_ = Origin::wheel;
    IconGroup group_ = IconGroup::apps;
    POINT anchor_{};
    POINT first_{};
    HWND target_ = nullptr;
    DWORD targetThread_ = 0;
    ULONGLONG deadline_ = 0;
};

} // namespace fullwintb::appearance
