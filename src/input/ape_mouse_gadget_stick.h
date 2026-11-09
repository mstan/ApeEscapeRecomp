#pragma once

#include <cstdint>

namespace ape {

// SDL-independent displacement reducer. All times are injected nanoseconds.
// Queries are const: only real motion/control events change the event anchor.
struct MouseGadgetSettings {
    double sensitivity = 1.0;
    bool invert_x = false;
    bool invert_y = false;
};

struct MouseGadgetVector { double x = 0.0, y = 0.0; };

class MouseGadgetStick {
public:
    static constexpr uint64_t StallNs = 250000000;
    static constexpr uint64_t DecayNs = 100000000;
    static constexpr uint64_t DirectionLeadNs = 4000000;
    static constexpr uint64_t PulseNs = 80000000;
    static constexpr uint64_t ReturnNs = 80000000;

    void configure(MouseGadgetSettings settings);
    void reset();
    // Returns false and resets on nonfinite, future, backwards or stale input.
    bool motion(uint64_t event_ns, uint64_t now_ns, double dx, double dy);
    // A press alone is neutral. Release is immediate, independent of time.
    void hold(bool pressed);
    MouseGadgetVector sample(uint64_t now_ns) const;
    void bytes(uint64_t now_ns, uint8_t& rx, uint8_t& ry) const;

private:
    MouseGadgetSettings settings_;
    double qx_ = 0.0, qy_ = 0.0;
    double previous_qx_ = 0.0, previous_qy_ = 0.0, direction_lead_ = 0.0;
    MouseGadgetVector target_;
    uint64_t event_ns_ = 0;
    bool has_motion_ = false, held_ = false;
};

} // namespace ape
