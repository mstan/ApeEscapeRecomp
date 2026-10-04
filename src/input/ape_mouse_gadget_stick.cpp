#include "ape_mouse_gadget_stick.h"
#include "psx_stick.h"

#include <algorithm>
#include <cmath>

namespace ape {

void MouseGadgetStick::configure(MouseGadgetSettings settings) {
    if (!std::isfinite(settings.sensitivity) ||
        settings.sensitivity < 0.25 || settings.sensitivity > 4.0)
        settings.sensitivity = 1.0;
    settings_ = settings;
    reset();
}

void MouseGadgetStick::reset() {
    qx_ = qy_ = 0.0;
    previous_qx_ = previous_qy_ = direction_lead_ = 0.0;
    target_ = {};
    event_ns_ = 0;
    has_motion_ = held_ = false;
}

bool MouseGadgetStick::motion(uint64_t t, uint64_t now, double dx, double dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy) || t > now ||
        now - t > StallNs || (has_motion_ && t < event_ns_)) {
        reset();
        return false;
    }
    // Even perfectly timed zero packets cannot extend a pulse or hold target.
    if (dx == 0.0 && dy == 0.0) return true;
    if (has_motion_ && t != event_ns_) {
        const uint64_t interval = t - event_ns_;
        previous_qx_ = qx_;
        previous_qy_ = qy_;
        // Extrapolate only within the live pulse history. Ancient velocity
        // must not influence a fresh stroke after quiet output has ended.
        // Equal-time splits share this prior event and factor; samples never
        // extrapolate direction or add turns during silence.
        direction_lead_ = interval < PulseNs + ReturnNs ?
            std::min(0.5, double(DirectionLeadNs) / double(interval)) : 0.0;
        const double decay = std::exp(-double(interval) / double(DecayNs));
        qx_ *= decay;
        qy_ *= decay;
    }
    // Reject hostile finite magnitudes instead of overflowing accumulation or
    // clamping each packet (which would break equal-time split equivalence).
    if (std::abs(dx) > 1e9 || std::abs(dy) > 1e9 ||
        std::abs(qx_ + dx) > 1e12 || std::abs(qy_ + dy) > 1e12) {
        reset();
        return false;
    }
    qx_ += dx;
    qy_ += dy;
    event_ns_ = t;
    has_motion_ = true;
    const double radius = std::hypot(qx_, qy_);
    const double noise = 2.0;
    const double full_distance = 48.0 / settings_.sensitivity;
    const double magnitude = std::clamp((radius - noise) / (full_distance - noise), 0.0, 1.0);
    // Recompute on EVERY real update, including below-gate cancellation.
    target_ = {};
    if (magnitude > 0.0) {
        double correction_x = direction_lead_ * (qx_ - previous_qx_);
        double correction_y = direction_lead_ * (qy_ - previous_qy_);
        const double correction_radius = std::hypot(correction_x, correction_y);
        // A correction at most half the displacement cannot reverse it or
        // collapse the direction before normalization. The corrected length
        // is at least radius/2, and its angle stays within 30 degrees of q.
        const double correction_limit = radius * 0.5;
        if (correction_radius > correction_limit) {
            const double scale = correction_limit / correction_radius;
            correction_x *= scale;
            correction_y *= scale;
        }
        const double direction_x = qx_ + correction_x;
        const double direction_y = qy_ + correction_y;
        const double direction_radius = std::hypot(direction_x, direction_y);
        // Gate/amplitude remain based on displacement, so a correction cannot
        // resurrect cancellation or amplify a small/noisy packet.
        const bool valid_direction = direction_radius > 0.0;
        target_.x = (valid_direction ? direction_x / direction_radius : qx_ / radius) *
                    magnitude * (settings_.invert_x ? -1.0 : 1.0);
        target_.y = (valid_direction ? direction_y / direction_radius : qy_ / radius) *
                    magnitude * (settings_.invert_y ? -1.0 : 1.0);
    }
    return true;
}

void MouseGadgetStick::hold(bool pressed) {
    reset();
    held_ = pressed;
}

MouseGadgetVector MouseGadgetStick::sample(uint64_t now) const {
    if (!has_motion_ || now < event_ns_) return {};
    if (held_) return target_;
    const uint64_t age = now - event_ns_;
    if (age >= PulseNs + ReturnNs) return {};
    const double scale = age <= PulseNs ? 1.0 :
        1.0 - double(age - PulseNs) / double(ReturnNs);
    return {target_.x * scale, target_.y * scale};
}

void MouseGadgetStick::bytes(uint64_t now, uint8_t& rx, uint8_t& ry) const {
    const auto v = sample(now);
    // Same radial mapper as native sticks, with no second host deadzone.
    psx_stick_to_dualshock(static_cast<int16_t>(std::lround(v.x * 32767.0)),
                          static_cast<int16_t>(std::lround(v.y * 32767.0)),
                          0, 5376, &rx, &ry);
}

} // namespace ape
