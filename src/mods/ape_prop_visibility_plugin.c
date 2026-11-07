#include "cpu_state.h"
#include "mod_plugins.h"

/* SCUS-94423's mailbox renderer projects each placement before its fixed
 * 3936-unit distance test. Animation counters precede this branch; dialogue
 * and interaction follow the placement loop. Only admit additional rendering
 * inside the current resident-world far plane, retaining its screen bounds.
 * SZ3 still belongs to that placement's RTPS at this instruction. */
static void ape_prop_distance(CPUState* cpu, uint32_t address) {
    const uint32_t depth = cpu->gte_data[19] & 0xFFFFu;
    const uint32_t original = (depth - 1u) < 3936u;
    uint32_t far;
    if (address != 0x80071B80u || cpu->gpr[2] != original || original)
        return;
    far = psx_mod_read_word(0x800EBFCCu);
    /* Zero, negative and out-of-range values are not a resident-world bound.
     * A saturated GTE depth cannot establish that the prop is inside it. */
    if (far <= 3936u || far > 65535u || depth == 65535u)
        return;
    if ((depth - 1u) < far) {
        cpu->gpr[2] = 1;
        psx_mod_counter_add("ape.prop-visibility.extended", 1);
    }
}

PSX_MOD_CONSTRUCTOR(ape_register_prop_visibility) {
    (void)psx_mod_register_instruction_plugin("ape.fix.prop-visibility",
        0x80071B80u, 0x10400034u, ape_prop_distance);
}
