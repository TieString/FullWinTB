#pragma once

#include "taskbar_layout.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace fullwintb::appearance {

class Spring {
public:
    Spring(double mass, double stiffness, double dampingRatio)
        : frequency_(std::sqrt(stiffness / mass)), dampingRatio_(dampingRatio),
          decay_(frequency_ * dampingRatio),
          separation_(frequency_ * std::sqrt(std::abs(1.0 - dampingRatio * dampingRatio))) {}

    void Advance(double& position, double& velocity, double target, double elapsed) const {
        const double dt = std::clamp(elapsed, 0.0, 0.064);
        if (!dt) return;
        const double distance = position - target;
        const double initialVelocity = velocity;
        double next;
        if (std::abs(dampingRatio_ - 1.0) < 1e-6) {
            const double factor = std::exp(-decay_ * dt);
            const double correction = initialVelocity + decay_ * distance;
            next = factor * (distance + correction * dt);
            velocity = factor * correction - decay_ * next;
        } else if (dampingRatio_ < 1.0) {
            const double factor = std::exp(-decay_ * dt);
            const double cosine = std::cos(separation_ * dt);
            const double sine = std::sin(separation_ * dt);
            const double correction = initialVelocity + decay_ * distance;
            next = factor * (distance * cosine + correction / separation_ * sine);
            velocity = factor * (-distance * separation_ * sine + correction * cosine) - decay_ * next;
        } else {
            const double fast = -(decay_ + separation_);
            const double slow = frequency_ * frequency_ / fast;
            const double slowComponent = (initialVelocity - fast * distance) / (slow - fast);
            const double towardSlow = slowComponent * std::exp(slow * dt);
            const double towardFast = (distance - slowComponent) * std::exp(fast * dt);
            next = towardSlow + towardFast;
            velocity = towardSlow * slow + towardFast * fast;
        }
        position = target + next;
    }

private:
    double frequency_;
    double dampingRatio_;
    double decay_;
    double separation_;
};

// One point from Feel's magnetic field, restricted to the taskbar's axis.
// Feel (c) 2026 diip3sh, MIT. See src/feel/FEEL-LICENSE.txt.
class ReturnMagnet {
public:
    explicit ReturnMagnet(const MotionSettings& settings)
        : drag_(static_cast<double>(settings.damping) / settings.mass),
          reach_(Square(tau * settings.reach / 100.0)), click_(Square(tau * settings.click / 100.0)) {}

    double CoastTime() const { return 1.0 / drag_; }

    void Advance(double& position, double& velocity, double seconds, double dpiScale,
                 ReturnStyle style = ReturnStyle::elastic) const {
        const double elapsed = std::clamp(seconds, 0.0, 0.064);
        if (!elapsed) return;
        const double radius = 28.0 * dpiScale;
        // Feel uses RK4 with at most 1/240 s per step. With a very light body
        // and high damping, subdivide further to keep the decay stable.
        const int steps = std::max(1, static_cast<int>(std::ceil(elapsed * std::max(240.0, drag_))));
        const double step = elapsed / steps;
        const auto acceleration = [&](double x, double v) {
            const double f = std::max(0.0, 1.0 - std::abs(x) / radius);
            const double spring = reach_ + click_ * f * f * (3.0 - 2.0 * f);
            return -spring * x - drag_ * v;
        };
        for (int i = 0; i < steps; ++i) {
            const double before = position;
            const double a1 = acceleration(position, velocity);
            const double v2 = velocity + step * 0.5 * a1;
            const double a2 = acceleration(position + step * 0.5 * velocity, v2);
            const double v3 = velocity + step * 0.5 * a2;
            const double a3 = acceleration(position + step * 0.5 * v2, v3);
            const double v4 = velocity + step * a3;
            const double a4 = acceleration(position + step * v3, v4);
            position += step / 6.0 * (velocity + 2 * v2 + 2 * v3 + v4);
            velocity += step / 6.0 * (a1 + 2 * a2 + 2 * a3 + a4);
            if (style == ReturnStyle::simple && before != 0 && before * position <= 0) {
                position = velocity = 0;
                return;
            }
        }
    }

private:
    static constexpr double tau = 6.283185307179586;
    static constexpr double Square(double value) { return value * value; }

    double drag_;
    double reach_;
    double click_;
};

struct MotionProfile {
    MotionProfile() : MotionProfile(MotionSettings{}) {}

    explicit MotionProfile(const MotionSettings& settings, ReturnStyle style = ReturnStyle::elastic)
        : released(settings), held(settings.mass / 100.0, 2200, 0.9),
          glideRate(std::clamp(1.0 / released.CoastTime(), 18.0, 72.0)),
          wheelStep(settings.wheelStep), hold(settings.hold), returnStyle(style) {}

    ReturnMagnet released;
    Spring held;
    double glideRate;
    int wheelStep;
    UINT hold;
    ReturnStyle returnStyle;
};

inline const MotionProfile defaultMotionProfile;

class IconTilt {
public:
    void Step(LONG delta, double elapsed, double dpiScale) {
        if (elapsed <= 0 || (!delta && !Active())) return;
        const double speed = delta / (elapsed * dpiScale);
        const double desired = std::clamp(speed * (8.0 / 520.0), -8.0, 8.0);
        const auto& response = std::abs(desired) > std::abs(angle_) ? follow : straighten;
        response.Advance(angle_, speed_, desired, elapsed);
        angle_ = std::clamp(angle_, -8.0, 8.0);
        if (!desired && std::abs(angle_) < 0.05 && std::abs(speed_) < 1.0) angle_ = speed_ = 0;
    }

    int Tenths() const { return std::lround(angle_ * 10); }
    bool Active() const { return angle_ != 0 || speed_ != 0; }

private:
    inline static const Spring follow{1, 1521, 0.9};
    inline static const Spring straighten{1, 576, 0.88};
    double angle_ = 0;
    double speed_ = 0;
};

class IconWheelGesture {
public:
    void Start(POINT cursor, ULONGLONG now) {
        anchor_ = cursor;
        Extend(now);
    }

    void Extend(ULONGLONG now) { expires_ = now + 750; }

    POINT Anchor() const { return anchor_; }
    bool Active() const { return expires_ != 0; }

    bool Continues(POINT cursor, ULONGLONG now, LONG tolerance) const {
        return expires_ > now && std::abs(cursor.x - anchor_.x) <= tolerance &&
               std::abs(cursor.y - anchor_.y) <= tolerance;
    }

    UINT Remaining(ULONGLONG now) const {
        return expires_ > now ? static_cast<UINT>(expires_ - now) : 0;
    }

private:
    POINT anchor_{};
    ULONGLONG expires_ = 0;
};

class IconMotion {
public:
    IconOffset Position() const { return position_.Rounded(); }
    IconOffset Target() const { return target_.Rounded(); }
    bool Pinned() const { return pinned_; }
    bool Moving() const { return gliding_ || position_ != target_ || velocity_ != Point{}; }
    bool Dragging() const { return dragging_; }
    bool ShowsLatch() const { return pinned_ || dragging_ || gliding_ || parked_; }
    bool WaitingToReturn() const { return parked_; }
    IconOffset WheelOrigin() const { return gliding_ || parked_ ? Target() : Position(); }

    UINT ReturnDelay(ULONGLONG now) const {
        return holdUntil_ && *holdUntil_ > now ? static_cast<UINT>(*holdUntil_ - now) : 0;
    }

    bool WatchReturn(ULONGLONG now, UINT hold, bool inside) {
        if (!parked_) return false;
        if (inside) {
            const bool canceled = holdUntil_.has_value();
            holdUntil_.reset();
            return canceled;
        }
        if (holdUntil_) return false;
        holdUntil_ = now + hold;
        return true;
    }

    bool Wake(ULONGLONG now) {
        if (!holdUntil_ || now < *holdUntil_) return false;
        ClearWait();
        target_ = {};
        return true;
    }

    void ConstrainTarget(IconOffset minimum, IconOffset maximum) {
        target_.x = std::clamp(target_.x, static_cast<double>(minimum.x), static_cast<double>(maximum.x));
        target_.y = std::clamp(target_.y, static_cast<double>(minimum.y), static_cast<double>(maximum.y));
    }

    void SetPosition(IconOffset position) {
        gliding_ = false;
        ClearWait();
        position_ = Point::From(position);
        target_ = (pinned_ || dragging_) ? position_ : Point{};
        velocity_ = {};
    }

    void Aim(IconOffset target) { target_ = Point::From(target); }
    void Kick(double x, double y) {
        velocity_.x += x;
        velocity_.y += y;
    }

    void ScrollTo(IconOffset target, const MotionProfile& profile) {
        ClearWait();
        gliding_ = true;
        Aim(target);
        velocity_ = {(target_.x - position_.x) * profile.glideRate, (target_.y - position_.y) * profile.glideRate};
    }

    void BeginDrag(IconOffset position) {
        dragging_ = true;
        SetPosition(position);
    }

    void EndDrag(bool settle = false) {
        if (!dragging_) return;
        dragging_ = false;
        if (pinned_) return;
        if (settle && Target()) gliding_ = true;
        else target_ = {};
    }

    void TogglePin(IconOffset position) {
        dragging_ = false;
        if (pinned_) Release();
        else {
            pinned_ = true;
            SetPosition(position);
        }
    }

    void Release() {
        pinned_ = false;
        dragging_ = false;
        gliding_ = false;
        ClearWait();
        target_ = {};
    }

    void Step(double elapsed, double dpiScale, const MotionProfile& profile = defaultMotionProfile) {
        if (gliding_) {
            const double dt = std::clamp(elapsed, 0.0, 0.064);
            const double factor = std::exp(-profile.glideRate * dt);
            const Point remaining{(target_.x - position_.x) * factor, (target_.y - position_.y) * factor};
            position_ = {target_.x - remaining.x, target_.y - remaining.y};
            velocity_ = {remaining.x * profile.glideRate, remaining.y * profile.glideRate};
            if (remaining.Small(0.25 * dpiScale)) {
                position_ = target_;
                velocity_ = {};
                gliding_ = false;
                parked_ = static_cast<bool>(Target());
            }
            return;
        }
        if (!Moving()) return;
        const auto advance = [&](double& position, double& velocity, double target) {
            if (dragging_ || pinned_) profile.held.Advance(position, velocity, target, elapsed);
            else profile.released.Advance(position, velocity, elapsed, dpiScale, profile.returnStyle);
        };
        advance(position_.x, velocity_.x, target_.x);
        advance(position_.y, velocity_.y, target_.y);
        const Point remaining{target_.x - position_.x, target_.y - position_.y};
        if (remaining.Small(0.25 * dpiScale) && velocity_.Small(4.0 * dpiScale)) {
            position_ = target_;
            velocity_ = {};
        }
    }

    IconOffset Limit(IconOffset minimum, IconOffset maximum, ReturnStyle style = ReturnStyle::elastic) {
        const bool returning = !pinned_ && !dragging_ && !gliding_ && target_ == Point{};
        const auto clip = [&](double& position, double& velocity, LONG first, LONG last) {
            // A dock on the screen edge has travel on one side of home.
            // Reflect the spring's excess distance and speed back onto the visible side.
            if (returning && style == ReturnStyle::elastic && first < last &&
                ((first == 0 && position < 0) || (last == 0 && position > 0))) {
                position = -position;
                velocity = -velocity;
            }
            const double boundary = std::clamp(position, static_cast<double>(first), static_cast<double>(last));
            if (position == boundary) return;
            position = boundary;
            velocity = (!pinned_ && !dragging_ && !boundary) ? 0 : -0.24 * velocity;
        };
        clip(position_.x, velocity_.x, minimum.x, maximum.x);
        clip(position_.y, velocity_.y, minimum.y, maximum.y);
        return Position();
    }

private:
    struct Point {
        double x = 0;
        double y = 0;
        static Point From(IconOffset offset) { return {static_cast<double>(offset.x), static_cast<double>(offset.y)}; }
        IconOffset Rounded() const { return {std::lround(x), std::lround(y)}; }
        bool Small(double threshold) const { return std::abs(x) < threshold && std::abs(y) < threshold; }
        bool operator==(const Point&) const = default;
    };

    void ClearWait() {
        parked_ = false;
        holdUntil_.reset();
    }

    Point position_;
    Point velocity_;
    Point target_;
    std::optional<ULONGLONG> holdUntil_;
    bool pinned_ = false;
    bool dragging_ = false;
    bool gliding_ = false;
    bool parked_ = false;
};

} // namespace fullwintb::appearance
