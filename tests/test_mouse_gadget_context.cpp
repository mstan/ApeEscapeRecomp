#include "ape_mouse_gadget_context.h"
#include <cstdio>
#include <initializer_list>

int main() {
    // Source-derived truth table, not a substitute for a gameplay observation.
    const ape::MouseGadgetContext good{true, 0x0B, 3, 0, 0, 0, 0xFF, 0x73, 0, 0};
    unsigned allowed = 0;
    for (unsigned state = 0; state < 256; ++state) {
        auto c = good; c.dispatcher = uint16_t(state);
        const bool expected = state == 0x0B || state == 0x0D || state == 0x11 ||
            state == 0x13 || state == 0x18 || state == 0x22 || state == 0x24;
        if (ape::mouse_gadget_eligible(c) != expected) return 1;
        allowed += expected;
    }
    for (unsigned phase = 0; phase < 256; ++phase) {
        auto c = good; c.transition = uint8_t(phase);
        if (ape::mouse_gadget_eligible(c) != (phase >= 3 && phase <= 5)) return 2;
    }
    for (unsigned menu = 1; menu < 256; ++menu) {
        auto c = good; c.menu = uint8_t(menu);
        if (ape::mouse_gadget_eligible(c)) return 3;
    }
    for (unsigned gadget = 0; gadget < 256; ++gadget) {
        auto c = good; c.gadget = uint8_t(gadget);
        if (ape::mouse_gadget_eligible(c) != (gadget < 8)) return 4;
        c.unlocked = 0;
        if (ape::mouse_gadget_eligible(c)) return 5;
    }
    auto c = good; c.game_started = false;
    if (ape::mouse_gadget_eligible(c)) return 6;
    c = good; c.pad_id = 0x41;
    if (ape::mouse_gadget_eligible(c)) return 7;
    c = good; c.scene_flags = 1;
    if (ape::mouse_gadget_eligible(c)) return 8;
    for (auto actor : {0x23, 0x4A, 0x2D}) {
        c = good; c.actor_state = uint16_t(actor);
        if (ape::mouse_gadget_eligible(c)) return 9;
    }
    c = good; c.actor_flags = 0xE0;
    if (ape::mouse_gadget_eligible(c)) return 10;
    std::printf("source context table: %u dispatcher entries; menu/transition/actor guards\n", allowed);
    return 0;
}
