#pragma once

#include "appearance.h"
#include <optional>

namespace fullwintb::appearance {

class AutomaticInk {
public:
    bool Observe(unsigned brightness);
    void Pause() { pending_ = Ink::system; }
    Ink Current() const { return current_; }
    bool Settling() const { return pending_ != Ink::system; }

private:
    Ink current_ = Ink::system;
    Ink pending_ = Ink::system;
};

std::optional<unsigned> ReadTaskbarBrightness(HWND bar, const RECT& monitor, bool horizontal);

} // namespace fullwintb::appearance
