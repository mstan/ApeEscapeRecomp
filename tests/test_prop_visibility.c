#include "cpu_state.h"
#include "mod_plugins.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static PSXModFunctionEntryCallback hook;
static uint32_t far_plane, admitted;

int psx_mod_register_instruction_plugin(const char* id, uint32_t pc,
        uint32_t expected, PSXModFunctionEntryCallback callback) {
    assert(strcmp(id, "ape.fix.prop-visibility") == 0);
    assert(pc == 0x80071B80u && expected == 0x10400034u && !hook);
    hook = callback;
    return 1;
}
uint32_t psx_mod_read_word(uint32_t address) {
    assert(address == 0x800EBFCCu);
    return far_plane;
}
void psx_mod_counter_add(const char* name, uint32_t delta) {
    assert(strcmp(name, "ape.prop-visibility.extended") == 0 && delta == 1);
    admitted += delta;
}

static void check(uint32_t depth, uint32_t far, uint32_t branch,
        uint32_t expected, uint32_t pc) {
    CPUState cpu, before;
    memset(&cpu, 0xA5, sizeof cpu);
    cpu.gte_data[19] = depth;
    cpu.gpr[2] = branch;
    before = cpu;
    far_plane = far;
    hook(&cpu, pc);
    assert(cpu.gpr[2] == expected);
    before.gpr[2] = expected;
    assert(memcmp(&before, &cpu, sizeof cpu) == 0);
}
int main(void) {
    assert(hook);
    /* Exhaust the entire hardware depth range at native and extended bounds. */
    for (uint32_t depth = 0; depth < 65536u; ++depth) {
        const uint32_t original = (depth - 1u) < 3936u;
        check(depth, 3936, original, original, 0x80071B80u);
        check(depth, 9024, original, (depth - 1u) < 9024u, 0x80071B80u);
        check(depth, 0, original, original, 0x80071B80u);
        check(depth, 0xFFFFFFFFu, original, original, 0x80071B80u);
    }
    check(5000, 9024, 2, 2, 0x80071B80u); /* wrong incoming predicate */
    check(5000, 9024, 0, 0, 0x80071B84u); /* wrong instruction */
    check(0, 65535, 0, 0, 0x80071B80u);
    check(65534, 65535, 0, 1, 0x80071B80u);
    check(65535, 65535, 0, 0, 0x80071B80u); /* saturated depth */
    check(5000, 65536, 0, 0, 0x80071B80u);
    check(5000, 2000, 0, 0, 0x80071B80u);
    assert(admitted == 9024u - 3936u + 1u);
    puts("Mailbox culling follows the resident far plane; other CPU state unchanged");
    return 0;
}
