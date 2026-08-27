#include "mod_plugins.h"

/* Keep Ape Escape's guest cadence stock. The framework performs all temporal
 * blends and swaps synchronously on the renderer's owning thread/context. */
static void ape_frame_smoothing_set(unsigned presents_per_second) {
    (void)psx_mod_set_frame_interpolation_blend(
        PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE);
    (void)psx_mod_set_frame_interpolation(presents_per_second);
}

static void ape_frame_smoothing_display_activate(void) {
    ape_frame_smoothing_set(0u);
}

static void ape_frame_smoothing_120_activate(void) {
    ape_frame_smoothing_set(120u);
}

static void ape_frame_smoothing_144_activate(void) {
    ape_frame_smoothing_set(144u);
}

static void ape_frame_smoothing_165_activate(void) {
    ape_frame_smoothing_set(165u);
}

PSX_MOD_CONSTRUCTOR(ape_register_frame_smoothing_plugins) {
    (void)psx_mod_register_activation_plugin(
        "ape.frame-smoothing.display", ape_frame_smoothing_display_activate);
    (void)psx_mod_register_activation_plugin(
        "ape.frame-smoothing.120", ape_frame_smoothing_120_activate);
    (void)psx_mod_register_activation_plugin(
        "ape.frame-smoothing.144", ape_frame_smoothing_144_activate);
    (void)psx_mod_register_activation_plugin(
        "ape.frame-smoothing.165", ape_frame_smoothing_165_activate);
}
