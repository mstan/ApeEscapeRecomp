#pragma once
#include <cstdint>

namespace ape {

struct MouseGadgetContext {
    bool game_started;
    uint16_t dispatcher;
    uint8_t transition;
    uint16_t actor_state;
    uint8_t menu, gadget, unlocked, pad_id;
    uint32_t scene_flags, actor_flags;
};

// Source-derived guards from SCUS-94423's persistent state dispatcher and
// interactive predicate. See docs/mouse-gadgets.md for addresses/provenance.
inline bool mouse_gadget_eligible(const MouseGadgetContext& c) {
    if (!c.game_started || c.menu != 0 || c.pad_id != 0x73 ||
        c.transition < 3 || c.transition > 5 || (c.scene_flags & 1) != 0 ||
        c.gadget >= 8 || (c.unlocked & (1u << c.gadget)) == 0 ||
        c.actor_state == 0x23 || c.actor_state == 0x4A || c.actor_state == 0x2D ||
        (c.actor_flags & 0xF0) == 0xE0) return false;
    // These exact table entries dispatch the common interactive loop 5BF70.
    // Unknown/new states, the card menu (0x0E), and overlay menus fail closed.
    switch (c.dispatcher) {
    case 0x0B: case 0x0D: case 0x11: case 0x13:
    case 0x18: case 0x22: case 0x24: return true;
    default: return false;
    }
}
} // namespace ape
