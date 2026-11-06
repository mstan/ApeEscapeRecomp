#include "cpu_state.h"
#include "mod_plugins.h"
#include "pgxp.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#if defined(PSX_MOD_DEVELOPER_CHANNEL) && PSX_MOD_DEVELOPER_CHANNEL

/* SCUS-94423 original-code seams, verified against the boot EXE:
 * 80016A74: skeletal projector's descriptor read; a0 is the descriptor.
 * 80028CD0: original DrawOTag's argument read; own the submitted list.
 * 80016B44: after RTPT and before its first SWC2. a1 is the vertex index,
 * v1 the SVECTOR pointer, t3 the bone index, s0 the matrix array.
 * 800161C4/80016260: rigid descriptor read and completed RTPT batch.
 * The captured data is host-only. No guest instructions are replayed and no
 * guest CPU, RAM, GTE, timing, visibility or packet output is changed.
 * Descriptor addresses are provenance, not yet qualified lifetime identities.
 * This development-only producer does not enable presentation interpolation. */
namespace {
constexpr const char* Plugin = "ape.presentation.capture";
constexpr size_t MaxBatches = 8192;
constexpr size_t MaxWords = 262144, MaxNodes = 16384;
struct Batch {
    uint64_t source;
    uint32_t descriptor, asset, bone, vertex;
    uint32_t local[6], matrix[8], root[8], camera[8];
    uint32_t native_xy[3], native_z[3], projection[3];
};
struct Packet {
    uint32_t address, first, words;
    uint64_t source[4]{};
};
struct Frame {
    uint64_t serial = 0;
    uint32_t skeletons = 0, rejected = 0, overflow = 0;
    std::vector<Batch> batches;
    std::vector<uint32_t> commands;
    std::vector<Packet> packets;
    uint32_t mapped = 0, partial = 0, unknown = 0, invalid_list = 0;
    Frame() {
        batches.reserve(MaxBatches); commands.reserve(MaxWords);
        packets.reserve(MaxNodes);
    }
    void clear(uint64_t id) {
        serial = id; skeletons = rejected = overflow = 0; batches.clear();
        commands.clear(); packets.clear(); mapped = partial = unknown = invalid_list = 0;
    }
};
struct Scope {
    uint32_t descriptor = 0, asset = 0, vertices = 0;
    uint32_t matrices = 0, spans = 0, bones = 0;
    uint32_t output = 0, count = 0;
    bool rigid = false;
};
std::unique_ptr<Frame> writing, published;
std::mutex published_mutex;
Scope scope;
std::atomic<bool> active{false};
uint64_t serial = 0;
uint64_t next_source = 1;
std::atomic<uint64_t> submissions{0}, batches{0}, rejected{0}, overflows{0};
std::atomic<uint64_t> mapped{0}, partial{0}, unknown{0}, invalid_lists{0}, tag_failures{0};
std::atomic<uint64_t> rigid_batches{0};
bool open = false;

bool ram(uint32_t address, uint32_t bytes, uint32_t alignment) {
    const uint32_t p = address & 0x1FFFFFFFu;
    if (address & (alignment - 1)) return false;
    return (p < 0x200000u && bytes <= 0x200000u - p) ||
        (p >= 0x1F800000u && p < 0x1F800400u && bytes <= 0x1F800400u - p);
}
void read_words(uint32_t address, uint32_t* out, unsigned count) {
    for (unsigned i = 0; i < count; ++i) out[i] = psx_mod_read_word(address + i * 4);
}
void reset() {
    scope = {}; open = active.load(); serial = 0;
    if (writing) writing->clear(0);
    std::lock_guard<std::mutex> lock(published_mutex);
    if (published) published->clear(0);
    pgxp_invalidate_all();
}
void deactivate() {
    active = false; reset(); pgxp_set_source_tracking(0);
}
void activate() {
    active = false; open = false;
    writing = std::make_unique<Frame>();
    {
        std::lock_guard<std::mutex> lock(published_mutex);
        published = std::make_unique<Frame>();
    }
    submissions = batches = rejected = overflows = 0;
    mapped = partial = unknown = invalid_lists = tag_failures = 0;
    rigid_batches = 0;
    if (!pgxp_set_source_tracking(1)) return;
    active = true; reset();
}
void capture_list(uint32_t head) {
    // Original linked-list addresses establish order. No polygon matching.
    unsigned char visited[0x10000]{};
    uint32_t address = head & 0xFFFFFFu;
    const uint64_t first = writing->batches.empty() ? next_source : writing->batches.front().source;
    for (size_t hops = 0; address != 0xFFFFFFu; ++hops) {
        if (hops == MaxNodes || !ram(address, 4, 4)) { ++writing->invalid_list; return; }
        const uint32_t bit = address / 4;
        if (visited[bit / 8] & (1u << (bit % 8))) { ++writing->invalid_list; return; }
        visited[bit / 8] |= uint8_t(1u << (bit % 8));
        const uint32_t link = psx_mod_read_word(address), count = link >> 24;
        if (count && !ram(address + 4, count * 4, 4)) { ++writing->invalid_list; return; }
        if (writing->commands.size() + count > MaxWords) { ++writing->overflow; return; }
        if (count) {
            Packet p{}; p.address = address + 4;
            p.first = uint32_t(writing->commands.size()); p.words = count;
            for (uint32_t i = 0; i < count; ++i)
                writing->commands.push_back(psx_mod_read_word(p.address + 4 * i));
            const unsigned op = writing->commands[p.first] >> 24;
            if ((op & 0xE0u) == 0x20u) {
                const unsigned n = (op & 8u) ? 4u : 3u;
                const unsigned stride = 1u + ((op & 16u) ? 1u : 0u) + ((op & 4u) ? 1u : 0u);
                unsigned valid = 0;
                if (count == n * stride + ((op & 16u) ? 0u : 1u)) {
                    for (unsigned i = 0; i < n; ++i) {
                        const unsigned offset = 1 + i * stride;
                        const uint64_t token = pgxp_load_source(p.address + 4 * offset,
                            writing->commands[p.first + offset]);
                        if (token >= first && token < next_source) { p.source[i] = token; ++valid; }
                    }
                }
                if (valid == n) ++writing->mapped;
                else if (valid) ++writing->partial;
                else ++writing->unknown;
            }
            writing->packets.push_back(p);
        }
        address = link & 0xFFFFFFu;
    }
}
void finish(CPUState* cpu, uint32_t) {
    if (!open) return;
    open = false; scope = {};
    capture_list(cpu->gpr[4]);
    batches += writing->batches.size(); rejected += writing->rejected;
    overflows += writing->overflow; ++submissions;
    mapped += writing->mapped; partial += writing->partial;
    unknown += writing->unknown; invalid_lists += writing->invalid_list;
    std::lock_guard<std::mutex> lock(published_mutex);
    writing.swap(published);
    writing->clear(++serial); open = true;
}
void enter(CPUState* cpu, uint32_t) {
    scope = {};
    if (!open) return;
    ++writing->skeletons;
    const uint32_t descriptor = cpu->gpr[4];
    if (!ram(descriptor, 24, 4)) { ++writing->rejected; return; }
    const uint32_t header = psx_mod_read_word(descriptor);
    if (!ram(header, 10, 2)) { ++writing->rejected; return; }
    Scope s;
    s.descriptor = descriptor; s.asset = header;
    s.bones = psx_mod_read_half(header + 4);
    s.vertices = psx_mod_read_word(descriptor + 16);
    s.matrices = psx_mod_read_word(descriptor + 20);
    s.spans = psx_mod_read_word(descriptor + 12);
    if (!s.bones) return; // the original empty-model path performs no projections
    if (s.bones > 128 || !ram(s.matrices, (s.bones + 1) * 32, 4) ||
        !ram(s.spans, s.bones * 12, 2) || !ram(s.vertices, 8, 4)) {
        ++writing->rejected; return;
    }
    scope = s;
}
void leave(CPUState*, uint32_t) { scope = {}; }
void capture(CPUState* cpu, uint32_t bone, uint32_t vertex, uint32_t local) {
    Batch b{};
    b.source = next_source;
    b.descriptor = scope.descriptor; b.asset = scope.asset;
    b.bone = bone; b.vertex = vertex;
    read_words(local, b.local, 6);
    for (unsigned i = 0; i < 3; ++i) {
        if (cpu->gte_data[2*i] != b.local[2*i] ||
            uint16_t(cpu->gte_data[2*i+1]) != uint16_t(b.local[2*i+1])) {
            ++writing->rejected; return;
        }
        b.native_xy[i] = cpu->gte_data[12+i]; b.native_z[i] = cpu->gte_data[17+i];
        b.projection[i] = cpu->gte_ctrl[24+i];
    }
    for (unsigned i = 0; i < 8; ++i) b.matrix[i] = cpu->gte_ctrl[i];
    read_words(scope.matrices, b.root, 8);
    read_words(0x800E78C0u, b.camera, 8);
    if (writing->batches.size() == MaxBatches) { ++writing->overflow; return; }
    for (unsigned i = 0; i < 3; ++i)
        if (!pgxp_tag_gte_source(i, b.native_xy[i], next_source + i)) ++tag_failures;
    next_source += 3;
    writing->batches.push_back(b);
}
void projected(CPUState* cpu, uint32_t) {
    if (!open) return;
    if (!scope.descriptor || scope.rigid) { ++writing->rejected; return; }
    const uint32_t bone = cpu->gpr[11], vertex = cpu->gpr[5];
    const uint32_t local = cpu->gpr[3];
    if (bone >= scope.bones || vertex > 8192 ||
        cpu->gpr[16] != scope.matrices || cpu->gpr[25] != scope.vertices ||
        cpu->gpr[15] != (bone + 1) * 32 ||
        local != scope.vertices + vertex * 8 || !ram(local, 24, 4) ||
        cpu->gpr[4] != 0x800E7D00u + vertex * 8) {
        ++writing->rejected; return;
    }
    const uint32_t span = scope.spans + bone * 12;
    const uint32_t count = psx_mod_read_half(span), start = psx_mod_read_half(span + 2);
    if (vertex < start || vertex >= start + count || (vertex - start) % 3) {
        ++writing->rejected; return;
    }
    capture(cpu, bone, vertex, local);
}
void enter_rigid(CPUState* cpu, uint32_t) {
    scope = {};
    if (!open) return;
    const uint32_t descriptor = cpu->gpr[4];
    if (!ram(descriptor, 16, 4)) { ++writing->rejected; return; }
    const uint32_t header = psx_mod_read_word(descriptor);
    if (!ram(header, 8, 2)) { ++writing->rejected; return; }
    Scope s;
    s.rigid = true; s.descriptor = descriptor; s.asset = header;
    s.vertices = psx_mod_read_word(descriptor + 12); s.matrices = cpu->gpr[5];
    s.output = cpu->gpr[6]; s.count = psx_mod_read_half(header + 6);
    if (!s.count) return;
    const uint32_t padded = (s.count + 2) / 3 * 3;
    if (s.count > 8192 || !ram(s.vertices, padded * 8, 4) ||
        !ram(s.output, padded * 8, 4) || !ram(s.matrices, 32, 4)) {
        ++writing->rejected; return;
    }
    scope = s;
}
void projected_rigid(CPUState* cpu, uint32_t) {
    if (!open) return;
    const uint32_t vertex = cpu->gpr[7], local = cpu->gpr[3];
    if (!scope.descriptor || !scope.rigid || vertex >= scope.count || vertex % 3 ||
        local != scope.vertices + vertex * 8 || cpu->gpr[6] != scope.output + vertex * 8 ||
        cpu->gpr[9] != scope.count || !ram(local, 24, 4)) {
        ++writing->rejected; return;
    }
    capture(cpu, UINT32_MAX, vertex, local); ++rigid_batches;
}
int command(const char* args, char* reply, int cap) {
    std::lock_guard<std::mutex> lock(published_mutex);
    const Frame* f = published.get();
    char* end = nullptr;
    unsigned long index = args && *args ? std::strtoul(args, &end, 10) : 0;
    if (end && *end) { std::snprintf(reply, size_t(cap), "invalid batch index"); return 0; }
    const Batch* b = f && index < f->batches.size() ? &f->batches[index] : nullptr;
    std::snprintf(reply, size_t(cap),
        "{\"active\":%d,\"submissions\":%llu,\"batches\":%llu,\"rejected\":%llu,"
        "\"overflow\":%llu,\"last_submission\":%llu,\"last_skeletons\":%u,"
        "\"last_batches\":%u,\"last_rejected\":%u,\"last_overflow\":%u,"
        "\"descriptor\":%u,\"asset\":%u,\"bone\":%u,"
        "\"vertex\":%u,\"native_xy\":%u,\"local_xy\":%u,"
        "\"mapped_polygons\":%llu,\"partial_polygons\":%llu,\"unknown_polygons\":%llu,"
        "\"invalid_lists\":%llu,\"tag_failures\":%llu,\"last_packets\":%u,\"last_words\":%u,"
        "\"rigid_batches\":%llu,"
        "\"lifetime_qualified\":false}",
        int(active.load()), (unsigned long long)submissions.load(),
        (unsigned long long)batches.load(), (unsigned long long)rejected.load(),
        (unsigned long long)overflows.load(), f ? (unsigned long long)f->serial : 0,
        f ? f->skeletons : 0, f ? unsigned(f->batches.size()) : 0,
        f ? f->rejected : 0, f ? f->overflow : 0,
        b ? b->descriptor : 0, b ? b->asset : 0, b ? b->bone : 0, b ? b->vertex : 0,
        b ? b->native_xy[0] : 0, b ? b->local[0] : 0,
        (unsigned long long)mapped.load(), (unsigned long long)partial.load(),
        (unsigned long long)unknown.load(), (unsigned long long)invalid_lists.load(),
        (unsigned long long)tag_failures.load(), f ? unsigned(f->packets.size()) : 0,
        f ? unsigned(f->commands.size()) : 0, (unsigned long long)rigid_batches.load());
    return 1;
}
}
PSX_MOD_CONSTRUCTOR(ape_register_presentation_capture) {
    psx_mod_register_activation_plugin(Plugin, activate);
    psx_mod_register_session_reset_plugin(Plugin, deactivate);
    psx_mod_register_savestate_plugin(Plugin, reset);
    psx_mod_register_instruction_plugin(Plugin, 0x80028CD0u, 0x00808021u, finish);
    psx_mod_register_instruction_plugin(Plugin, 0x80016A74u, 0x8C900014u, enter);
    psx_mod_register_instruction_plugin(Plugin, 0x80016B44u, 0xE88C0000u, projected);
    psx_mod_register_instruction_plugin(Plugin, 0x80016B8Cu, 0x03E00008u, leave);
    psx_mod_register_instruction_plugin(Plugin, 0x800161C4u, 0x8C83000Cu, enter_rigid);
    psx_mod_register_instruction_plugin(Plugin, 0x80016260u, 0xE8CC0000u, projected_rigid);
    psx_mod_register_instruction_plugin(Plugin, 0x8001628Cu, 0x03E00008u, leave);
    psx_mod_register_command("ape.presentation.capture", command);
}
#endif
