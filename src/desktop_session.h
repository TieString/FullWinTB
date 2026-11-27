#pragma once

#include <algorithm>
#include <windows.h>

namespace fullwintb {

class DesktopSession {
public:
    bool Active() const { return phase_ == Phase::active; }
    bool Settling() const { return phase_ == Phase::settling; }

    bool Lock() {
        if (phase_ == Phase::locked) return false;
        phase_ = Phase::locked;
        return true;
    }

    bool Unlock(ULONGLONG now) {
        if (phase_ != Phase::locked) return false;
        phase_ = Phase::settling;
        deadline_ = now + 1800;
        ObserveChange(now);
        return true;
    }

    void ObserveChange(ULONGLONG now) {
        if (Settling()) readyAt_ = std::min(deadline_, now + 420);
    }

    UINT Delay(ULONGLONG now) const {
        return Settling() && readyAt_ > now ? static_cast<UINT>(readyAt_ - now) : 0;
    }

    bool Resume(ULONGLONG now) {
        if (!Settling() || Delay(now)) return false;
        phase_ = Phase::active;
        return true;
    }

private:
    enum class Phase { active, locked, settling };
    Phase phase_ = Phase::active;
    ULONGLONG readyAt_ = 0;
    ULONGLONG deadline_ = 0;
};

} // namespace fullwintb
