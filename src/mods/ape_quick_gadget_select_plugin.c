#include "mod_plugins.h"

#include <stdint.h>

/*
 * Quick Gadget Select - backport of the Ape Escape 2/3 gadget row.
 *
 * A second press of the equipped face button opens a row of the unlocked
 * gadgets drawn over the frame; further presses of the same button cycle the
 * selection, and either right-stick use or a different face button commits.
 *
 * The guest-code half of this feature (suppressing the stock slingshot ammo
 * cycle, which claims the same second press) is NOT here: it is a declarative
 * [[patch]] in the package manifest. Activation callbacks run before the guest
 * boots, so a psx_mod_write_code_word() from one would be overwritten by the
 * game's own EXE load.
 */

/* Guest addresses. Everything below 0x800B5000 is inside the boot EXE image
 * (game.toml: load_address 0x80010000, text_size 0xA5000); the rest is BSS.
 * APE_HELD_GADGET_ADDRESS is confirmed by the stock slingshot block that the
 * manifest patch rewrites: `lui v0,0x800F` / `lb v1,-0x3D2E(v0)`. */
static const uint32_t APE_TRANSITION_PHASE_ADDRESS = 0x800F447Cu;
/* Game-side pad shadow. It starts at the controller's reply header, so the
 * layout is: id, 0x5A, the two digital button bytes, then -- and only when the
 * id says DualShock -- right X, right Y, left X, left Y. Measured live at the
 * title screen and in the attract demo with no pad attached: 41 5A FF FF 00 00,
 * i.e. a DIGITAL pad, where the axis bytes are not part of the report at all.
 * That is why the axes must never be read without checking the id first. */
static const uint32_t APE_PAD_ID_ADDRESS = 0x800B87A0u;
static const uint32_t APE_PAD_LOW_INPUT_ADDRESS = 0x800B87A2u;
static const uint32_t APE_FACE_INPUT_ADDRESS = 0x800B87A3u;
static const uint32_t APE_RIGHT_STICK_X_ADDRESS = 0x800B87A4u;
static const uint32_t APE_RIGHT_STICK_Y_ADDRESS = 0x800B87A5u;
/* Four bytes per gadget: u, v, then the 16-bit CLUT id. */
static const uint32_t APE_GADGET_ICON_TABLE_ADDRESS = 0x800B2254u;
static const uint32_t APE_PLAYER_IDLE_COUNTER_ADDRESS = 0x800EC328u;
/* Four bytes, one gadget id per face-button slot. */
static const uint32_t APE_GADGET_SLOT_BASE = 0x800F51A8u;
static const uint32_t APE_HELD_GADGET_ADDRESS = 0x800EC2D2u;
/* Bit per gadget. */
static const uint32_t APE_UNLOCKED_GADGETS_ADDRESS = 0x800F51C4u;

static const uint32_t PSX_GPU_GP0_ADDRESS = 0x1F801810u;
static const uint32_t PSX_GPU_GPUSTAT_ADDRESS = 0x1F801814u;

enum {
    APE_TRANSITION_PLAYING = 0x03u,
    APE_TRANSITION_NEARBY = 0x04u,
    APE_TRANSITION_LOADED = 0x05u,

    APE_PAD_START = 0x08u,

    /* Controller reply id: 0x41 digital, 0x73 DualShock. */
    APE_PAD_ID_ANALOG = 0x73u,

    APE_FACE_TRIANGLE = 0x10u,
    APE_FACE_CIRCLE = 0x20u,
    APE_FACE_CROSS = 0x40u,
    APE_FACE_SQUARE = 0x80u,
    APE_FACE_MASK = 0xF0u,

    APE_FACE_SLOT_COUNT = 4u,
    APE_GADGET_COUNT = 8u,
    APE_INVALID_GADGET = 0xFFu,

    APE_RIGHT_STICK_DEADZONE = 28,
    APE_IDLE_COUNTER_TRIGGER = 901u,

    /* Gap kept between the row and the edges of the picture. The right edge
     * itself comes from psx_mod_display_width() at draw time: Ape Escape
     * scans out 384, not the 320 a fixed constant would assume. */
    APE_ROW_MARGIN = 4,
    APE_ROW_Y = 5,
    APE_ICON_SIZE = 32,
    APE_TILE_GAP = 2,
    APE_TILE_STEP = APE_ICON_SIZE + APE_TILE_GAP,

    /* Texture page holding the gadget icons: 4bpp at (896, 256), dithered. */
    APE_ICON_TEXPAGE = 0x021Eu
};

static uint8_t g_previous_face_buttons;
static uint8_t g_last_face_button;
static uint8_t g_quick_face_button;
static uint8_t g_quick_selected_gadget = APE_INVALID_GADGET;
static uint8_t g_quick_select_active;

static uint8_t g_snapshot_slots[APE_FACE_SLOT_COUNT];
static uint8_t g_snapshot_held_gadget = APE_INVALID_GADGET;
static uint8_t g_snapshot_valid;

/*
 * Last guest state this plugin wrote. Plugin statics are not part of a save
 * state, so a restore (or a rewind step, which does the same thing many times
 * a second) can replace guest RAM underneath an open row and leave the
 * snapshot describing an abandoned timeline. Writing that snapshot back would
 * corrupt the restored save. Every write records what it left behind; each
 * frame re-checks it, and any mismatch abandons the snapshot without writing.
 * This also covers the game itself reassigning gadgets.
 */
static uint8_t g_expected_slots[APE_FACE_SLOT_COUNT];
static uint8_t g_expected_held_gadget;
static uint8_t g_expected_valid;

static uint8_t ape_pressed_face_buttons(void) {
    return (uint8_t)(~psx_mod_read_byte(APE_FACE_INPUT_ADDRESS) & APE_FACE_MASK);
}

static int ape_start_is_pressed(void) {
    return ((uint8_t)~psx_mod_read_byte(APE_PAD_LOW_INPUT_ADDRESS) &
            APE_PAD_START) != 0u;
}

static int ape_axis_is_active(uint32_t address, int deadzone) {
    const int value = (int)psx_mod_read_byte(address) - 0x80;
    return value < -deadzone || value > deadzone;
}

/*
 * Only a DualShock report carries stick axes. On a digital pad the axis bytes
 * are outside the reply and read 0x00, which is a full deflection away from the
 * 0x80 centre -- so reading them unguarded reports the stick as permanently
 * active, and the row would commit on the very frame it opened. Check the id
 * first and fail closed: without an analog pad the row is committed with
 * another face button instead.
 */
static int ape_pad_is_analog(void) {
    return psx_mod_read_byte(APE_PAD_ID_ADDRESS) == APE_PAD_ID_ANALOG;
}

static int ape_right_stick_is_active(void) {
    if (!ape_pad_is_analog())
        return 0;
    return ape_axis_is_active(APE_RIGHT_STICK_X_ADDRESS,
                              APE_RIGHT_STICK_DEADZONE) ||
           ape_axis_is_active(APE_RIGHT_STICK_Y_ADDRESS,
                              APE_RIGHT_STICK_DEADZONE);
}

static int ape_is_entering_idle(void) {
    return psx_mod_read_half(APE_PLAYER_IDLE_COUNTER_ADDRESS) >=
           APE_IDLE_COUNTER_TRIGGER;
}

static int ape_is_playable_transition(uint8_t phase) {
    return phase >= APE_TRANSITION_PLAYING &&
           phase <= APE_TRANSITION_LOADED;
}

static int ape_is_single_bit(uint8_t value) {
    return value != 0u && (value & (uint8_t)(value - 1u)) == 0u;
}

static uint8_t ape_slot_from_face_button(uint8_t face_button) {
    switch (face_button) {
        case APE_FACE_TRIANGLE:
            return 0u;
        case APE_FACE_SQUARE:
            return 1u;
        case APE_FACE_CIRCLE:
            return 2u;
        case APE_FACE_CROSS:
            return 3u;
        default:
            return APE_INVALID_GADGET;
    }
}

static uint8_t ape_face_button_from_slot(uint8_t slot) {
    static const uint8_t faces[APE_FACE_SLOT_COUNT] = {
        APE_FACE_TRIANGLE, APE_FACE_SQUARE, APE_FACE_CIRCLE, APE_FACE_CROSS
    };
    return slot < APE_FACE_SLOT_COUNT ? faces[slot] : 0u;
}

static uint8_t ape_read_slot(uint8_t slot) {
    return psx_mod_read_byte(APE_GADGET_SLOT_BASE + (uint32_t)slot);
}

static void ape_write_slot(uint8_t slot, uint8_t gadget) {
    psx_mod_write_byte(APE_GADGET_SLOT_BASE + (uint32_t)slot, gadget);
}

/* Record the guest state this plugin has just finished writing. */
static void ape_record_expected_state(void) {
    uint8_t slot;
    for (slot = 0u; slot < APE_FACE_SLOT_COUNT; ++slot)
        g_expected_slots[slot] = ape_read_slot(slot);
    g_expected_held_gadget = psx_mod_read_byte(APE_HELD_GADGET_ADDRESS);
    g_expected_valid = 1u;
}

static void ape_forget_expected_state(void) {
    g_expected_valid = 0u;
}

/* False once guest RAM stops matching what this plugin left there. */
static int ape_state_is_continuous(void) {
    uint8_t slot;
    if (!g_expected_valid)
        return 1;
    for (slot = 0u; slot < APE_FACE_SLOT_COUNT; ++slot) {
        if (ape_read_slot(slot) != g_expected_slots[slot])
            return 0;
    }
    return psx_mod_read_byte(APE_HELD_GADGET_ADDRESS) == g_expected_held_gadget;
}

static int ape_slot_value_is_valid(uint8_t gadget) {
    return gadget < APE_GADGET_COUNT || gadget == APE_INVALID_GADGET;
}

static int ape_has_valid_gadget_context(void) {
    const uint8_t unlocked =
        psx_mod_read_byte(APE_UNLOCKED_GADGETS_ADDRESS);
    const uint8_t held = psx_mod_read_byte(APE_HELD_GADGET_ADDRESS);
    uint8_t slot;
    uint8_t held_matches = 0u;

    if (unlocked == 0u || held >= APE_GADGET_COUNT)
        return 0;

    if (g_quick_select_active && g_snapshot_valid) {
        if (g_quick_selected_gadget >= APE_GADGET_COUNT ||
            (unlocked & (uint8_t)(1u << g_quick_selected_gadget)) == 0u) {
            return 0;
        }
        for (slot = 0u; slot < APE_FACE_SLOT_COUNT; ++slot) {
            if (!ape_slot_value_is_valid(g_snapshot_slots[slot]))
                return 0;
        }
        return 1;
    }

    if ((unlocked & (uint8_t)(1u << held)) == 0u)
        return 0;

    for (slot = 0u; slot < APE_FACE_SLOT_COUNT; ++slot) {
        const uint8_t gadget = ape_read_slot(slot);
        if (!ape_slot_value_is_valid(gadget))
            return 0;
        if (gadget == held)
            ++held_matches;
    }
    return held_matches != 0u;
}

static uint8_t ape_find_snapshot_slot_with_gadget(uint8_t gadget,
                                                   uint8_t excluded_slot) {
    uint8_t slot;
    for (slot = 0u; slot < APE_FACE_SLOT_COUNT; ++slot) {
        if (slot != excluded_slot && g_snapshot_slots[slot] == gadget)
            return slot;
    }
    return APE_INVALID_GADGET;
}

static void ape_take_snapshot(void) {
    uint8_t slot;
    for (slot = 0u; slot < APE_FACE_SLOT_COUNT; ++slot)
        g_snapshot_slots[slot] = ape_read_slot(slot);
    g_snapshot_held_gadget =
        psx_mod_read_byte(APE_HELD_GADGET_ADDRESS);
    g_snapshot_valid = 1u;
}

static void ape_restore_snapshot_slots(void) {
    uint8_t slot;
    if (!g_snapshot_valid)
        return;
    for (slot = 0u; slot < APE_FACE_SLOT_COUNT; ++slot)
        ape_write_slot(slot, g_snapshot_slots[slot]);
}

static void ape_discard_snapshot(void) {
    g_snapshot_valid = 0u;
    g_snapshot_held_gadget = APE_INVALID_GADGET;
}

static void ape_close_quick_select_state(void) {
    g_quick_select_active = 0u;
    g_quick_face_button = 0u;
    g_quick_selected_gadget = APE_INVALID_GADGET;
}

/* Drop an open row without writing anything to the guest. Used when the world
 * changed underneath the snapshot and restoring it would be corruption. */
static void ape_abandon_quick_select(void) {
    ape_discard_snapshot();
    ape_close_quick_select_state();
    ape_forget_expected_state();
    g_last_face_button = 0u;
}

static void ape_apply_snapshot_swap(uint8_t gadget);

static void ape_commit_quick_select(uint8_t replacement_face_button) {
    uint8_t replacement_slot;

    ape_apply_snapshot_swap(g_quick_selected_gadget);

    if (replacement_face_button != 0u) {
        replacement_slot = ape_slot_from_face_button(replacement_face_button);
        if (replacement_slot < APE_FACE_SLOT_COUNT) {
            psx_mod_write_byte(APE_HELD_GADGET_ADDRESS,
                               ape_read_slot(replacement_slot));
        }
    }

    ape_discard_snapshot();
    ape_close_quick_select_state();
    /* The committed layout is now the game's own; stop tracking it. */
    ape_forget_expected_state();
}

static void ape_cancel_quick_select(uint8_t replacement_face_button) {
    if (g_snapshot_valid) {
        ape_restore_snapshot_slots();

        if (replacement_face_button != 0u) {
            const uint8_t replacement_slot =
                ape_slot_from_face_button(replacement_face_button);
            if (replacement_slot < APE_FACE_SLOT_COUNT) {
                psx_mod_write_byte(APE_HELD_GADGET_ADDRESS,
                                   g_snapshot_slots[replacement_slot]);
            } else {
                psx_mod_write_byte(APE_HELD_GADGET_ADDRESS,
                                   g_snapshot_held_gadget);
            }
        } else {
            psx_mod_write_byte(APE_HELD_GADGET_ADDRESS,
                               g_snapshot_held_gadget);
        }
    }

    ape_discard_snapshot();
    ape_close_quick_select_state();
    ape_forget_expected_state();
}

static void ape_reset_quick_select_state(uint8_t pressed_face_buttons,
                                         int restore_snapshot) {
    if (restore_snapshot)
        ape_cancel_quick_select(0u);
    else
        ape_abandon_quick_select();
    g_previous_face_buttons = pressed_face_buttons;
    g_last_face_button = 0u;
}

static uint8_t ape_find_next_unlocked_gadget(uint8_t current,
                                              uint8_t unlocked) {
    uint8_t step;
    const uint8_t start = current < APE_GADGET_COUNT
                              ? current
                              : (uint8_t)(APE_GADGET_COUNT - 1u);

    for (step = 1u; step <= APE_GADGET_COUNT; ++step) {
        const uint8_t candidate =
            (uint8_t)((start + step) & (APE_GADGET_COUNT - 1u));
        if ((unlocked & (uint8_t)(1u << candidate)) != 0u)
            return candidate;
    }
    return current;
}

static void ape_seed_selected_face_from_held_gadget(void) {
    const uint8_t held = psx_mod_read_byte(APE_HELD_GADGET_ADDRESS);
    uint8_t matching_face = 0u;
    uint8_t matches = 0u;
    uint8_t slot;

    if (held >= APE_GADGET_COUNT)
        return;

    for (slot = 0u; slot < APE_FACE_SLOT_COUNT; ++slot) {
        if (ape_read_slot(slot) == held) {
            matching_face = ape_face_button_from_slot(slot);
            ++matches;
        }
    }

    if (matches == 1u)
        g_last_face_button = matching_face;
}

static uint32_t ape_gpu_color(uint8_t red, uint8_t green, uint8_t blue) {
    return (uint32_t)red | ((uint32_t)green << 8u) |
           ((uint32_t)blue << 16u);
}

static void ape_gpu_rect(int x, int y, int width, int height, uint32_t color) {
    if (width <= 0 || height <= 0)
        return;

    psx_mod_write_word(PSX_GPU_GP0_ADDRESS, 0x60000000u | color);
    psx_mod_write_word(PSX_GPU_GP0_ADDRESS,
                       ((uint32_t)(uint16_t)y << 16u) |
                           (uint32_t)(uint16_t)x);
    psx_mod_write_word(PSX_GPU_GP0_ADDRESS,
                       ((uint32_t)(uint16_t)height << 16u) |
                           (uint32_t)(uint16_t)width);
}

static void ape_gpu_native_gadget_icon(uint8_t gadget, int x, int y) {
    const uint32_t descriptor =
        APE_GADGET_ICON_TABLE_ADDRESS + (uint32_t)gadget * 4u;
    const uint8_t u = psx_mod_read_byte(descriptor);
    const uint8_t v = psx_mod_read_byte(descriptor + 1u);
    const uint16_t clut = psx_mod_read_half(descriptor + 2u);

    psx_mod_write_word(PSX_GPU_GP0_ADDRESS, 0x65000000u);
    psx_mod_write_word(PSX_GPU_GP0_ADDRESS,
                       ((uint32_t)(uint16_t)y << 16u) |
                           (uint32_t)(uint16_t)x);
    psx_mod_write_word(PSX_GPU_GP0_ADDRESS,
                       ((uint32_t)clut << 16u) |
                           ((uint32_t)v << 8u) | (uint32_t)u);
    psx_mod_write_word(PSX_GPU_GP0_ADDRESS,
                       ((uint32_t)APE_ICON_SIZE << 16u) |
                           (uint32_t)APE_ICON_SIZE);
}

static void ape_draw_quick_select_row(uint8_t unlocked, uint8_t selected) {
    const uint32_t row_background = ape_gpu_color(18u, 20u, 27u);
    const uint32_t normal_border = ape_gpu_color(78u, 84u, 96u);
    const uint32_t selected_border = ape_gpu_color(255u, 226u, 90u);
    const uint32_t icon_background = ape_gpu_color(24u, 27u, 35u);
    /*
     * Where the picture actually ends. The width is whatever the guest is
     * scanning out right now -- Ape Escape uses 368-mode and scans out 384,
     * so anything derived from an assumed 320 puts the row well short of the
     * edge. The widescreen reveal, zero at 4:3, extends the picture by that
     * much on each side on top of it.
     */
    const uint32_t display_width = psx_mod_display_width();
    const int32_t reveal = psx_mod_widescreen_x_margin();
    int row_right;
    int row_left_limit;
    uint32_t saved_gpustat;
    uint32_t saved_texpage;
    uint8_t count = 0u;
    uint8_t gadget;
    int total_width;
    int x;

    for (gadget = 0u; gadget < APE_GADGET_COUNT; ++gadget) {
        if ((unlocked & (uint8_t)(1u << gadget)) != 0u)
            ++count;
    }
    if (count == 0u)
        return;
    /* No geometry yet: wait rather than draw the row in a guessed place. */
    if (display_width == 0u)
        return;

    row_right = (int)display_width + (int)reveal - APE_ROW_MARGIN;
    row_left_limit = APE_ROW_MARGIN - (int)reveal;

    total_width = (int)count * APE_ICON_SIZE +
                  ((int)count - 1) * APE_TILE_GAP;
    x = row_right - total_width;
    if (x < row_left_limit)
        x = row_left_limit;

    /*
     * GPU state. The drawing offset, drawing area and mask settings are
     * deliberately left as the game set them: this runs on the guest VBlank
     * tick, before the game's own VBlank handler flips, so inheriting the
     * offset is what places the row in the buffer that is about to become
     * visible. Setting our own would put it in the wrong half of VRAM.
     *
     * The texture page and texture window are different - they are ours to
     * set, because a leftover texture window would wrap the icon UVs. GPUSTAT
     * bits 0..10 mirror the texture page, so it can be put back; the texture
     * window has no readback and is left cleared. That is safe here because
     * the game re-establishes both from its own ordering table at the top of
     * every frame, and nothing else draws between this row and that.
     */
    saved_gpustat = psx_mod_read_word(PSX_GPU_GPUSTAT_ADDRESS);
    saved_texpage = saved_gpustat & 0x7FFu;
    psx_mod_write_word(PSX_GPU_GP0_ADDRESS, 0xE2000000u);
    psx_mod_write_word(PSX_GPU_GP0_ADDRESS, 0xE1000000u | APE_ICON_TEXPAGE);

    ape_gpu_rect(x - 3, APE_ROW_Y - 3, total_width + 6,
                 APE_ICON_SIZE + 6, row_background);

    for (gadget = 0u; gadget < APE_GADGET_COUNT; ++gadget) {
        const int selected_gadget = gadget == selected;
        if ((unlocked & (uint8_t)(1u << gadget)) == 0u)
            continue;

        ape_gpu_rect(x - 1, APE_ROW_Y - 1, APE_ICON_SIZE + 2,
                     APE_ICON_SIZE + 2,
                     selected_gadget ? selected_border : normal_border);
        ape_gpu_rect(x, APE_ROW_Y, APE_ICON_SIZE, APE_ICON_SIZE,
                     icon_background);
        ape_gpu_native_gadget_icon(gadget, x, APE_ROW_Y);

        if (selected_gadget) {
            ape_gpu_rect(x + 3, APE_ROW_Y - 3, APE_ICON_SIZE - 6, 2,
                         selected_border);
        }

        x += APE_TILE_STEP;
    }

    psx_mod_write_word(PSX_GPU_GP0_ADDRESS, 0xE1000000u | saved_texpage);
}

static void ape_apply_snapshot_swap(uint8_t gadget) {
    const uint8_t selected_slot =
        ape_slot_from_face_button(g_quick_face_button);
    uint8_t other_slot;

    if (!g_snapshot_valid || selected_slot >= APE_FACE_SLOT_COUNT ||
        gadget >= APE_GADGET_COUNT) {
        return;
    }

    ape_restore_snapshot_slots();
    other_slot =
        ape_find_snapshot_slot_with_gadget(gadget, selected_slot);
    if (other_slot < APE_FACE_SLOT_COUNT)
        ape_write_slot(other_slot, g_snapshot_slots[selected_slot]);

    ape_write_slot(selected_slot, gadget);
    psx_mod_write_byte(APE_HELD_GADGET_ADDRESS, gadget);
    ape_record_expected_state();
}

static void ape_preview_gadget(uint8_t gadget) {
    ape_apply_snapshot_swap(gadget);
    if (g_snapshot_valid && gadget < APE_GADGET_COUNT)
        g_quick_selected_gadget = gadget;
}

static void ape_advance_quick_selected_gadget(void) {
    const uint8_t unlocked =
        psx_mod_read_byte(APE_UNLOCKED_GADGETS_ADDRESS);
    const uint8_t next =
        ape_find_next_unlocked_gadget(g_quick_selected_gadget, unlocked);
    ape_preview_gadget(next);
}

static void ape_open_quick_select(uint8_t face_button, uint8_t gadget) {
    ape_take_snapshot();
    g_quick_select_active = 1u;
    g_quick_face_button = face_button;
    g_quick_selected_gadget = gadget;
    ape_record_expected_state();
}

static int ape_cancel_for_idle(void) {
    if (!g_quick_select_active || !ape_is_entering_idle())
        return 0;

    ape_cancel_quick_select(0u);
    g_last_face_button = 0u;
    return 1;
}

static void ape_quick_gadget_select_vblank(void) {
    uint8_t pressed_face_buttons;
    uint8_t just_pressed;

    if (!psx_mod_game_started()) {
        ape_reset_quick_select_state(0u, 0);
        return;
    }

    /* Before anything reads or writes gadget state: if guest RAM no longer
     * matches what this plugin last wrote, a save state, a rewind or the game
     * itself replaced it. The snapshot describes a timeline that no longer
     * exists, so drop it silently instead of writing it back. */
    if (g_quick_select_active && !ape_state_is_continuous()) {
        ape_abandon_quick_select();
        g_previous_face_buttons = ape_pressed_face_buttons();
        return;
    }

    pressed_face_buttons = ape_pressed_face_buttons();
    if (!ape_is_playable_transition(
            psx_mod_read_byte(APE_TRANSITION_PHASE_ADDRESS)) ||
        !ape_has_valid_gadget_context() || ape_start_is_pressed()) {
        ape_reset_quick_select_state(pressed_face_buttons, 1);
        return;
    }

    if (ape_cancel_for_idle()) {
        g_previous_face_buttons = pressed_face_buttons;
        return;
    }

    just_pressed =
        (uint8_t)(pressed_face_buttons & (uint8_t)~g_previous_face_buttons);
    g_previous_face_buttons = pressed_face_buttons;

    if (g_quick_select_active && ape_right_stick_is_active()) {
        g_last_face_button = g_quick_face_button;
        ape_commit_quick_select(0u);
        return;
    }

    if (just_pressed != 0u) {
        if (!ape_is_single_bit(just_pressed) ||
            pressed_face_buttons != just_pressed) {
            ape_cancel_quick_select(0u);
            g_last_face_button = 0u;
        } else if (g_quick_select_active) {
            if (just_pressed == g_quick_face_button) {
                ape_advance_quick_selected_gadget();
            } else {
                ape_commit_quick_select(just_pressed);
                g_last_face_button = just_pressed;
            }
        } else {
            const uint8_t slot = ape_slot_from_face_button(just_pressed);
            const uint8_t slot_gadget =
                slot < APE_FACE_SLOT_COUNT ? ape_read_slot(slot)
                                           : APE_INVALID_GADGET;
            const uint8_t held =
                psx_mod_read_byte(APE_HELD_GADGET_ADDRESS);

            if (just_pressed == g_last_face_button &&
                slot_gadget < APE_GADGET_COUNT && held == slot_gadget) {
                ape_open_quick_select(just_pressed, slot_gadget);
            } else {
                g_last_face_button = just_pressed;
            }
        }
    } else if (!g_quick_select_active && pressed_face_buttons == 0u &&
               g_last_face_button == 0u) {
        ape_seed_selected_face_from_held_gadget();
    }

    if (g_quick_select_active) {
        const uint8_t unlocked =
            psx_mod_read_byte(APE_UNLOCKED_GADGETS_ADDRESS);
        ape_draw_quick_select_row(unlocked, g_quick_selected_gadget);
    }
}

PSX_MOD_CONSTRUCTOR(ape_register_quick_gadget_select_plugin) {
    (void)psx_mod_register_vblank_plugin(
        "ape.gadgets.quick-select", ape_quick_gadget_select_vblank);
}
