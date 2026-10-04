#include "mod_plugins.h"
#include "../input/ape_mouse_gadget_context.h"
#include "../input/ape_mouse_gadget_stick.h"

#include <cstdlib>
#include <cstring>

namespace {
constexpr const char* Package = "ape.enhancement.mouse-gadgets";
constexpr const char* Feature = "mouse-gadgets";
ape::MouseGadgetStick stick;

bool option(const char* id, char* out, uint32_t size) {
    return psx_mod_option_value(Package, Feature, id, out, size) != 0;
}
int eligible(uint32_t buttons) {
    // The dispatcher opens the persistent pause/gadget menu on Start/Select.
    // Suspend before publishing that first menu-entry report as well.
    if ((buttons & 0x0009) != 0x0009) return 0;
    const ape::MouseGadgetContext context{
        psx_mod_game_started() != 0,
        psx_mod_read_half(0x800F4470), psx_mod_read_byte(0x800F447C),
        psx_mod_read_half(0x800EC23E), psx_mod_read_byte(0x800BEF74),
        psx_mod_read_byte(0x800EC2D2), psx_mod_read_byte(0x800F51C4),
        psx_mod_read_byte(0x800B87A0), psx_mod_read_word(0x800F4450),
        psx_mod_read_word(0x800EC250)};
    return ape::mouse_gadget_eligible(context) ? 1 : 0;
}
void event(const PSXModMouseEvent* e) {
    if (!e || e->struct_size != sizeof(*e)) { stick.reset(); return; }
    switch (e->type) {
    case PSX_MOD_MOUSE_MOTION:
        // Runtime has already validated arrival time against its SDL clock.
        if (!stick.motion(e->time_ns, e->time_ns, e->dx, e->dy))
            psx_mod_counter_add("ape.mouse.invalid_motion", 1);
        break;
    case PSX_MOD_MOUSE_HOLD_PRESS: stick.hold(true); break;
    case PSX_MOD_MOUSE_HOLD_RELEASE: stick.hold(false); break;
    default: stick.reset(); break;
    }
}
void sample(uint64_t now, PSXModMouseOutput* out) {
    uint8_t rx, ry;
    stick.bytes(now, rx, ry);
    out->override_right = 1;
    out->rx = rx;
    out->ry = ry;
}
void activate() {
    ape::MouseGadgetSettings settings;
    char value[32];
    if (option("sensitivity", value, sizeof value)) {
        char* end = nullptr;
        const long percent = std::strtol(value, &end, 10);
        if (end != value && *end == '\0' && percent >= 25 && percent <= 400)
            settings.sensitivity = double(percent) / 100.0;
    }
    settings.invert_x = option("invert-x", value, sizeof value) && std::strcmp(value, "true") == 0;
    settings.invert_y = option("invert-y", value, sizeof value) && std::strcmp(value, "true") == 0;
    stick.configure(settings);
    uint32_t hold = PSX_MOD_MOUSE_HOLD_RIGHT;
    if (option("hold", value, sizeof value)) {
        if (std::strcmp(value, "LeftAlt") == 0) hold = PSX_MOD_MOUSE_HOLD_LEFT_ALT;
        else if (std::strcmp(value, "None") == 0) hold = PSX_MOD_MOUSE_HOLD_NONE;
    }
    const PSXModMousePolicy policy{sizeof(PSXModMousePolicy), hold, eligible, event, sample};
    if (!psx_mod_set_local_mouse_policy(&policy))
        psx_mod_counter_add("ape.mouse.registration_failed", 1);
}
} // namespace

PSX_MOD_CONSTRUCTOR(ape_register_mouse_gadget_plugin) {
    (void)psx_mod_register_activation_plugin("ape.gadgets.mouse", activate);
}
