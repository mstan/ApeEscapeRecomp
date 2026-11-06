#include "cpu_state.h"
#include "mod_plugins.h"
#include "pgxp.h"
#include "gpu.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
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
enum SourceKind : uint32_t { Skeletal, Rigid, World, TypedWorld, Terrain, Subdivision };
struct Batch {
    uint64_t source;
    uint64_t lifetime;
    uint32_t descriptor, asset, bone, vertex;
    uint32_t local[6], matrix[8], root[8], camera[8];
    uint32_t native_xy[3], native_z[3], projection[3];
    uint32_t input[6], kind, source_vertex[3], vertex_count;
    uint64_t parents[2]; /* Subdivision: original edge sources, depth guard 164 */
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
    uint32_t projected = 0;
    Frame() {
        batches.reserve(MaxBatches); commands.reserve(MaxWords);
        packets.reserve(MaxNodes);
    }
    void clear(uint64_t id) {
        serial = id; skeletons = rejected = overflow = 0; batches.clear();
        commands.clear(); packets.clear(); mapped = partial = unknown = invalid_list = 0;
        projected = 0;
    }
};
struct Scope {
    uint64_t lifetime = 0;
    uint32_t descriptor = 0, asset = 0, vertices = 0;
    uint32_t matrices = 0, spans = 0, bones = 0;
    uint32_t output = 0, count = 0;
    bool rigid = false;
    SourceKind kind = Skeletal;
    uint32_t quarter_turn = 0;
    int16_t origin[3]{};
};
std::unique_ptr<Frame> writing, published;
struct SourceRecord {
    Batch batch{};
    uint64_t copied_submission = UINT64_MAX;
};
std::unique_ptr<SourceRecord[]> source_records;
std::mutex published_mutex;
Scope scope;
std::atomic<bool> active{false};
uint64_t serial = 0;
uint64_t next_source = 4;
uint64_t next_lifetime = 1;
struct Lifetime { uint64_t token; uint32_t asset, constructor; };
std::unordered_map<uint32_t,Lifetime> lifetimes;
std::atomic<uint64_t> constructions{0}, lifetime_batches{0};
std::atomic<uint64_t> submissions{0}, batches{0}, rejected{0}, overflows{0};
std::atomic<uint64_t> mapped{0}, partial{0}, unknown{0}, invalid_lists{0}, tag_failures{0};
std::atomic<uint64_t> rigid_batches{0};
std::atomic<uint64_t> world_batches{0}, typed_batches{0};
std::atomic<uint64_t> terrain_vertices{0};
std::atomic<uint64_t> subdivision_vertices{0};
bool open = false;
bool reported_tuple[5]{}, reported_typed = false, reported_height = false;

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
    lifetimes.clear();
    if (writing) writing->clear(0);
    std::lock_guard<std::mutex> lock(published_mutex);
    if (published) published->clear(0);
    if (source_records) for (size_t i = 0; i < MaxBatches; ++i) source_records[i] = {};
    pgxp_invalidate_all();
}
void deactivate() {
    active = false; reset(); pgxp_set_source_tracking(0);
}
void activate() {
    active = false; open = false;
    writing = std::make_unique<Frame>();
    source_records = std::make_unique<SourceRecord[]>(MaxBatches);
    {
        std::lock_guard<std::mutex> lock(published_mutex);
        published = std::make_unique<Frame>();
    }
    submissions = batches = rejected = overflows = 0;
    mapped = partial = unknown = invalid_lists = tag_failures = 0;
    rigid_batches = 0;
    world_batches = typed_batches = 0;
    terrain_vertices = 0;
    subdivision_vertices = 0;
    constructions = lifetime_batches = 0;
    for (bool& reported : reported_tuple) reported = false;
    reported_typed = reported_height = false;
    if (!pgxp_set_source_tracking(1)) return;
    active = true; reset();
}
void constructed(CPUState* cpu, uint32_t pc) {
    if (!open) return;
    // Observe AFTER the original constructor's header store. Keep the witness
    // on that exact word using the existing write-invalidated token transport.
    // DMA/untracked overwrite drops it even if the new word has the same value.
    const uint32_t descriptor = cpu->gpr[(pc==0x80015BA8u || pc==0x80015C3Cu) ? 6 : 4];
    const uint32_t asset = cpu->gpr[5];
    if (!ram(descriptor,24,4) || !ram(asset,20,2) ||
        psx_mod_read_word(descriptor)!=asset) { ++writing->rejected; return; }
    const uint32_t key = descriptor & 0x1FFFFFFFu;
    if (lifetimes.size()==4096 && lifetimes.find(key)==lifetimes.end()) {
        ++writing->overflow; return;
    }
    const uint64_t token = (uint64_t(1)<<63) | next_lifetime++;
    if (!pgxp_tag_word_source(descriptor,asset,token)) { ++tag_failures; return; }
    lifetimes[key] = {token,asset,pc}; ++constructions;
}
uint64_t lifetime(uint32_t descriptor, uint32_t asset) {
    const auto entry=lifetimes.find(descriptor & 0x1FFFFFFFu);
    return entry!=lifetimes.end() && entry->second.asset==asset &&
        pgxp_load_source(descriptor,asset)==entry->second.token ? entry->second.token : 0;
}
bool retain_source(uint64_t token, unsigned depth = 0) {
    if (!token || depth > 8) return false;
    SourceRecord& record = source_records[(token >> 2) % MaxBatches];
    const Batch& b = record.batch;
    if (b.source != (token & ~uint64_t(3)) || (token & 3) >= b.vertex_count) return false;
    if (record.copied_submission == writing->serial) return true;
    if (b.kind == Subdivision &&
        (!retain_source(b.parents[0], depth+1) || !retain_source(b.parents[1], depth+1))) return false;
    if (writing->batches.size() == MaxBatches) { ++writing->overflow; return false; }
    writing->batches.push_back(b); record.copied_submission = writing->serial;
    return true;
}
void capture_list(uint32_t head) {
    // Original linked-list addresses establish order. No polygon matching.
    unsigned char visited[0x10000]{};
    uint32_t address = head & 0xFFFFFFu;
    for (size_t hops = 0; address != 0xFFFFFFu; ++hops) {
        if (hops == MaxNodes || !ram(address, 4, 4)) { ++writing->invalid_list; return; }
        const uint32_t bit = address / 4;
        if (visited[bit / 8] & (1u << (bit % 8))) { ++writing->invalid_list; return; }
        visited[bit / 8] |= uint8_t(1u << (bit % 8));
        const uint32_t link = psx_mod_read_word(address), count = link >> 24;
        if (count && !ram(address + 4, count * 4, 4)) { ++writing->invalid_list; return; }
        if (writing->commands.size() + count > MaxWords) { ++writing->overflow; return; }
        if (count) {
            const uint32_t first = uint32_t(writing->commands.size());
            for (uint32_t i = 0; i < count; ++i)
                writing->commands.push_back(psx_mod_read_word(address + 4 + 4 * i));
            for (uint32_t offset = 0; offset < count;) {
                const unsigned op = writing->commands[first + offset] >> 24;
                int length = gpu_gp0_command_word_count(uint8_t(op));
                if (length < 0) {
                    // Polyline terminators occur at a vertex (mono) or colour
                    // (shaded) boundary, after the first complete segment.
                    const bool shaded = (op & 16u) != 0;
                    uint32_t end = offset + (shaded ? 4u : 3u);
                    for (; end < count; end += shaded ? 2u : 1u)
                        if ((writing->commands[first+end] & 0xF000F000u) == 0x50005000u) break;
                    length = end < count ? int(end-offset+1) : 0;
                }
                if (length > 0 && (op & 0xE0u) == 0xA0u && offset + 3 <= count) {
                    const uint32_t size = writing->commands[first+offset+2];
                    const uint32_t w = ((size-1)&1023u)+1, h = (((size>>16)-1)&511u)+1;
                    length += int((w*h+1)/2);
                }
                if (length <= 0 || uint32_t(length) > count-offset) { ++writing->invalid_list; return; }
                if (writing->packets.size() == MaxNodes) { ++writing->overflow; return; }
                Packet p{}; p.address = address + 4 + offset * 4;
                p.first = first + offset; p.words = uint32_t(length);
                if ((op & 0xE0u) == 0x20u) {
                    const unsigned n = (op & 8u) ? 4u : 3u;
                    const unsigned stride = 1u + ((op & 16u) ? 1u : 0u) + ((op & 4u) ? 1u : 0u);
                    unsigned valid = 0;
                    if (p.words == n * stride + ((op & 16u) ? 0u : 1u)) {
                        for (unsigned i = 0; i < n; ++i) {
                            const unsigned offset = 1 + i * stride;
                            const uint64_t token = pgxp_load_source(p.address + 4 * offset,
                                writing->commands[p.first + offset]);
                            if (retain_source(token)) { p.source[i] = token; ++valid; }
                        }
                    }
                    if (valid == n) ++writing->mapped;
                    else if (valid) ++writing->partial;
                    else ++writing->unknown;
                }
                writing->packets.push_back(p);
                offset += p.words;
            }
        }
        address = link & 0xFFFFFFu;
    }
}
void finish(CPUState* cpu, uint32_t) {
    if (!open) return;
    open = false; scope = {};
    capture_list(cpu->gpr[4]);
    batches += writing->projected; rejected += writing->rejected;
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
    s.lifetime = lifetime(descriptor,header);
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
bool capture(CPUState* cpu, uint32_t bone, uint32_t vertex, uint32_t local,
             const uint32_t* input = nullptr, unsigned count = 3,
             const uint32_t* indices = nullptr, unsigned fifo = 0) {
    Batch b{};
    b.source = next_source;
    b.descriptor = scope.descriptor; b.asset = scope.asset;
    if (scope.lifetime && pgxp_load_source(scope.descriptor,scope.asset)==scope.lifetime)
        b.lifetime = scope.lifetime;
    b.bone = bone; b.vertex = vertex;
    b.kind = scope.kind; b.vertex_count = count;
    read_words(local, b.local, count * 2);
    for (unsigned i = 0; i < count * 2; ++i) b.input[i] = input ? input[i] : b.local[i];
    for (unsigned i = 0; i < count; ++i) {
        b.source_vertex[i] = indices ? indices[i] : vertex + i;
        if (cpu->gte_data[2*i] != b.input[2*i] ||
            uint16_t(cpu->gte_data[2*i+1]) != uint16_t(b.input[2*i+1])) {
            if (!reported_tuple[b.kind]) {
                reported_tuple[b.kind] = true;
                std::fprintf(stderr, "Ape capture tuple mismatch kind=%u vertex=%u corner=%u expected=%08X/%04X actual=%08X/%04X\n",
                    b.kind, vertex, i, b.input[2*i], uint16_t(b.input[2*i+1]),
                    cpu->gte_data[2*i], uint16_t(cpu->gte_data[2*i+1]));
            }
            ++writing->rejected; return false;
        }
        b.native_xy[i] = cpu->gte_data[12+fifo+i]; b.native_z[i] = cpu->gte_data[17+fifo+i];
    }
    for (unsigned i = 0; i < 3; ++i) b.projection[i] = cpu->gte_ctrl[24+i];
    for (unsigned i = 0; i < 8; ++i) b.matrix[i] = cpu->gte_ctrl[i];
    read_words(scope.matrices, b.root, 8);
    if (scope.kind == TypedWorld) {
        // This source bakes an authored quarter-turn and origin into its GTE
        // inputs. Retain the separate world-from-asset matrix as well.
        const int16_t c[4] = {4096,0,-4096,0}, s[4] = {0,4096,0,-4096};
        const uint32_t q = scope.quarter_turn;
        const int16_t m[10] = {c[q],0,s[q],0,4096,0,int16_t(-s[q]),0,c[q],0};
        for (unsigned i = 0; i < 5; ++i)
            b.root[i] = uint16_t(m[2*i]) | uint32_t(uint16_t(m[2*i+1])) << 16;
        for (unsigned i = 0; i < 3; ++i) b.root[5+i] = uint32_t(int32_t(scope.origin[i]));
    }
    read_words(0x800E78C0u, b.camera, 8);
    if (writing->batches.size() == MaxBatches) { ++writing->overflow; return false; }
    for (unsigned i = 0; i < count; ++i)
        if (!pgxp_tag_gte_source(fifo+i, b.native_xy[i], next_source + i)) ++tag_failures;
    SourceRecord& record = source_records[(next_source >> 2) % MaxBatches];
    record.batch = b; record.copied_submission = writing->serial;
    next_source += 4;
    writing->batches.push_back(b);
    ++writing->projected;
    if (b.lifetime) ++lifetime_batches;
    return true;
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
void enter_rigid(CPUState* cpu, uint32_t pc) {
    scope = {};
    if (!open) return;
    const uint32_t descriptor = cpu->gpr[4];
    if (!ram(descriptor, 16, 4)) { ++writing->rejected; return; }
    const uint32_t header = psx_mod_read_word(descriptor);
    const bool world = pc == 0x800160F0u;
    if (!ram(header, world ? 20 : 8, 2)) { ++writing->rejected; return; }
    Scope s;
    s.rigid = true; s.descriptor = descriptor; s.asset = header;
    s.lifetime = lifetime(descriptor,header);
    s.kind = world ? World : Rigid;
    s.vertices = psx_mod_read_word(descriptor + 12); s.matrices = cpu->gpr[5];
    s.output = cpu->gpr[6]; s.count = psx_mod_read_half(header + (world ? 18 : 6));
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
    if (capture(cpu, UINT32_MAX, vertex, local)) {
        if (scope.kind == World) ++world_batches; else ++rigid_batches;
    }
}
void enter_typed(CPUState* cpu, uint32_t) {
    scope = {};
    if (!open) return;
    const uint32_t descriptor = cpu->gpr[4];
    if (!ram(descriptor, 32, 4)) { ++writing->rejected; return; }
    const uint32_t header = psx_mod_read_word(descriptor);
    if (!ram(header, 20, 2)) { ++writing->rejected; return; }
    Scope s;
    s.kind = TypedWorld; s.descriptor = descriptor; s.asset = header;
    s.lifetime = lifetime(descriptor,header);
    s.vertices = psx_mod_read_word(descriptor + 12); s.output = cpu->gpr[5];
    s.count = psx_mod_read_half(header + 18); s.matrices = 0x800E78C0;
    s.quarter_turn = (psx_mod_read_word(descriptor + 28) >> 10) & 3;
    for (unsigned i = 0; i < 3; ++i) s.origin[i] = int16_t(psx_mod_read_half(0x1F8003C8u + i*2));
    if (!s.count) return;
    const uint32_t padded = (s.count + 2) / 3 * 3;
    if (s.count > 8192 || !ram(s.vertices, padded * 8, 4) || !ram(s.output, padded * 8, 4)) {
        ++writing->rejected; return;
    }
    scope = s;
}
void projected_typed(CPUState* cpu, uint32_t pc) {
    if (!open) return;
    const uint32_t vertex = cpu->gpr[9];
    const uint32_t turn = pc == 0x80016394u ? 1u : pc == 0x80016514u ? 2u :
        pc == 0x80016690u ? 3u : 0u;
    const uint32_t local = cpu->gpr[turn ? 7 : 4];
    if (!scope.descriptor || scope.kind != TypedWorld || turn != scope.quarter_turn ||
        vertex >= scope.count || vertex % 3 || local != scope.vertices + vertex * 8 ||
        cpu->gpr[5] != scope.output + vertex * 8 || cpu->gpr[13] != scope.count || !ram(local, 24, 4)) {
        if (!reported_typed) {
            reported_typed = true;
            std::fprintf(stderr, "Ape typed scope mismatch pc=%08X turn=%u expected_turn=%u vertex=%u count=%u/%u source=%08X/%08X output=%08X/%08X\n",
                pc, turn, scope.quarter_turn, vertex, cpu->gpr[13], scope.count,
                local, scope.vertices+vertex*8, cpu->gpr[5], scope.output+vertex*8);
        }
        ++writing->rejected; return;
    }
    uint32_t input[6];
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t xy = psx_mod_read_word(local + i*8);
        const int32_t x = int16_t(xy), y = int16_t(xy >> 16);
        const int32_t z = int16_t(psx_mod_read_half(local + i*8 + 4));
        const int32_t xs[4] = {x,z,-x,-z}, zs[4] = {z,-x,-z,x};
        input[i*2] = uint16_t(scope.origin[0] + xs[turn]) |
            uint32_t(uint16_t(scope.origin[1] + y)) << 16;
        input[i*2+1] = uint16_t(scope.origin[2] + zs[turn]);
    }
    if (capture(cpu, UINT32_MAX-1, vertex, local, input)) ++typed_batches;
}
void projected_terrain(CPUState* cpu, uint32_t pc) {
    if (!open) return;
    // Original height-array index, not an identity derived from coordinates.
    const uint32_t cols = psx_mod_read_half(0x1F8002A4u);
    const uint32_t rows = psx_mod_read_half(0x1F8002A6u);
    const uint64_t total = uint64_t(cols+1) * (rows+1) + uint64_t(cols) * rows;
    const uint32_t height = psx_mod_read_word(0x800EBFB4u);
    const bool single = pc == 0x8004412Cu, coarse = pc == 0x800440C4u;
    const unsigned count = single ? 1u : 3u;
    const uint32_t base = cpu->gpr[16];
    const uint32_t indices[3] = {base + (single ? cols + 1 : 0), base+1,
                                 base + (coarse ? cols : 2)};
    const uint32_t local = single ? 0x1F800208u : 0x1F800200u;
    if (!cols || !rows || total > 0x100000u || !ram(height, uint32_t(total)*2, 2)) {
        ++writing->rejected; return;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (indices[i] >= total || psx_mod_read_half(local+i*8+2) !=
            psx_mod_read_half(height+indices[i]*2)) {
            if (!reported_height) {
                reported_height = true;
                std::fprintf(stderr, "Ape terrain height mismatch pc=%08X base=%u index=%u dims=%u/%u height=%08X expected=%04X actual=%04X\n",
                    pc, base, indices[i], cols, rows, height,
                    indices[i] < total ? psx_mod_read_half(height+indices[i]*2) : 0,
                    psx_mod_read_half(local+i*8+2));
            }
            ++writing->rejected; return;
        }
    }
    Scope previous = scope;
    scope = {}; scope.kind = Terrain; scope.descriptor = scope.asset = height;
    scope.matrices = 0x800E78C0u;
    if (capture(cpu, UINT32_MAX-2, indices[0], local, nullptr, count, indices, single ? 2 : 0))
        terrain_vertices += count;
    scope = previous;
}
void subdivided(CPUState* cpu, uint32_t pc) {
    if (!open) return;
    // Original recursive GT4 subdivision: s1 is the four-corner input block.
    // Bind after both output coordinate halves were written. These are native
    // projective midpoint recipes (depth guard 164), not fitted 3D positions.
    unsigned a=0,b=0,out=0;
    switch (pc) {
        case 0x8001DF6C: a=0;b=8;out=72;break;
        case 0x8001E0CC: a=0;b=16;out=80;break;
        case 0x8001E20C: a=8;b=16;out=88;break;
        case 0x8001E408: a=8;b=24;out=88;break;
        case 0x8001E5FC: a=16;b=24;out=80;break;
        case 0x8001E7F4: a=0;b=16;out=64;break;
        default:return;
    }
    const uint32_t block=cpu->gpr[17];
    if (!ram(block,128,4)) { ++writing->rejected; return; }
    const uint64_t p0=pgxp_load_source(block+a,psx_mod_read_word(block+a));
    const uint64_t p1=pgxp_load_source(block+b,psx_mod_read_word(block+b));
    if (!p0 || !p1) return; // unknown parent stays unknown; never fill by position
    Batch d{};
    d.source=next_source;d.kind=Subdivision;d.vertex_count=1;
    d.parents[0]=p0;d.parents[1]=p1;
    d.native_xy[0]=psx_mod_read_word(block+out);
    d.native_z[0]=psx_mod_read_half(block+out+4);
    SourceRecord& record=source_records[(next_source>>2)%MaxBatches];
    record.batch=d;record.copied_submission=UINT64_MAX;
    if (!pgxp_tag_word_source(block+out,d.native_xy[0],next_source)) { ++tag_failures; return; }
    next_source+=4;++subdivision_vertices;
}
int command(const char* args, char* reply, int cap) {
    std::lock_guard<std::mutex> lock(published_mutex);
    const Frame* f = published.get();
    if (args && std::strcmp(args, "missing") == 0) {
        if (f) for (const Packet& p : f->packets) {
            const unsigned op = f->commands[p.first] >> 24;
            if ((op & 0xE0u) != 0x20u) continue;
            const unsigned n = (op & 8u) ? 4u : 3u;
            unsigned known = 0;
            for (unsigned i = 0; i < n; ++i) known += p.source[i] != 0;
            if (!known || known == n) continue;
            uint32_t kinds[4]{}, descriptors[4]{};
            for (unsigned i = 0; i < n; ++i) for (const Batch& b : f->batches)
                if (p.source[i] && b.source == (p.source[i] & ~uint64_t(3))) {
                    kinds[i] = b.kind; descriptors[i] = b.descriptor; break;
                }
            std::snprintf(reply, size_t(cap),
                "{\"available\":true,\"submission\":%llu,\"address\":%u,\"opcode\":%u,\"word_count\":%u,"
                "\"source\":[%llu,%llu,%llu,%llu],\"kind\":[%u,%u,%u,%u],"
                "\"descriptor\":[%u,%u,%u,%u]}",
                (unsigned long long)f->serial,p.address,op,p.words,
                (unsigned long long)p.source[0],(unsigned long long)p.source[1],
                (unsigned long long)p.source[2],(unsigned long long)p.source[3],
                kinds[0],kinds[1],kinds[2],kinds[3],
                descriptors[0],descriptors[1],descriptors[2],descriptors[3]);
            return 1;
        }
        std::snprintf(reply, size_t(cap), "{\"available\":false}"); return 1;
    }
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
        "\"world_batches\":%llu,\"typed_batches\":%llu,"
        "\"terrain_vertices\":%llu,\"kind\":%u,\"vertex_count\":%u,"
        "\"subdivision_vertices\":%llu,"
        "\"constructors\":%llu,\"lifetime_batches\":%llu,\"lifetime\":%llu,"
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
        f ? unsigned(f->commands.size()) : 0, (unsigned long long)rigid_batches.load(),
        (unsigned long long)world_batches.load(), (unsigned long long)typed_batches.load(),
        (unsigned long long)terrain_vertices.load(), b ? b->kind : 0, b ? b->vertex_count : 0,
        (unsigned long long)subdivision_vertices.load(),
        (unsigned long long)constructions.load(), (unsigned long long)lifetime_batches.load(),
        b ? (unsigned long long)b->lifetime : 0);
    return 1;
}
}
PSX_MOD_CONSTRUCTOR(ape_register_presentation_capture) {
    psx_mod_register_activation_plugin(Plugin, activate);
    psx_mod_register_session_reset_plugin(Plugin, deactivate);
    psx_mod_register_savestate_plugin(Plugin, reset);
    psx_mod_register_instruction_plugin(Plugin, 0x80015AC4u, 0x94A20008u, constructed);
    psx_mod_register_instruction_plugin(Plugin, 0x80015B34u, 0x94A20008u, constructed);
    psx_mod_register_instruction_plugin(Plugin, 0x80015BA8u, 0x94A20008u, constructed);
    psx_mod_register_instruction_plugin(Plugin, 0x80015C3Cu, 0x8CA70000u, constructed);
    psx_mod_register_instruction_plugin(Plugin, 0x80015D14u, 0x94A20006u, constructed);
    psx_mod_register_instruction_plugin(Plugin, 0x80028CD0u, 0x00808021u, finish);
    psx_mod_register_instruction_plugin(Plugin, 0x80016A74u, 0x8C900014u, enter);
    psx_mod_register_instruction_plugin(Plugin, 0x80016B44u, 0xE88C0000u, projected);
    psx_mod_register_instruction_plugin(Plugin, 0x80016B8Cu, 0x03E00008u, leave);
    psx_mod_register_instruction_plugin(Plugin, 0x800161C4u, 0x8C83000Cu, enter_rigid);
    psx_mod_register_instruction_plugin(Plugin, 0x80016260u, 0xE8CC0000u, projected_rigid);
    psx_mod_register_instruction_plugin(Plugin, 0x8001628Cu, 0x03E00008u, leave);
    psx_mod_register_instruction_plugin(Plugin, 0x800160F0u, 0x8C820000u, enter_rigid);
    psx_mod_register_instruction_plugin(Plugin, 0x8001618Cu, 0xE8CC0000u, projected_rigid);
    psx_mod_register_instruction_plugin(Plugin, 0x800161B8u, 0x03E00008u, leave);
    psx_mod_register_instruction_plugin(Plugin, 0x8001629Cu, 0x8C820000u, enter_typed);
    psx_mod_register_instruction_plugin(Plugin, 0x80016394u, 0x24E40018u, projected_typed);
    psx_mod_register_instruction_plugin(Plugin, 0x80016514u, 0x94E20018u, projected_typed);
    psx_mod_register_instruction_plugin(Plugin, 0x80016690u, 0x24E40018u, projected_typed);
    psx_mod_register_instruction_plugin(Plugin, 0x8001680Cu, 0x24830018u, projected_typed);
    psx_mod_register_instruction_plugin(Plugin, 0x80043E68u, 0x26040001u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x80043F18u, 0x26040001u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x80043FC8u, 0x26240250u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x800440C4u, 0x26220268u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x8004412Cu, 0x26220280u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x80044820u, 0x26040001u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x800448DCu, 0x26040001u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x80044998u, 0x26240250u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x8001DF6Cu, 0x241800A4u, subdivided);
    psx_mod_register_instruction_plugin(Plugin, 0x8001E0CCu, 0x241900A4u, subdivided);
    psx_mod_register_instruction_plugin(Plugin, 0x8001E20Cu, 0x00409021u, subdivided);
    psx_mod_register_instruction_plugin(Plugin, 0x8001E408u, 0x00409021u, subdivided);
    psx_mod_register_instruction_plugin(Plugin, 0x8001E5FCu, 0x00409021u, subdivided);
    psx_mod_register_instruction_plugin(Plugin, 0x8001E7F4u, 0x00409021u, subdivided);
    psx_mod_register_command("ape.presentation.capture", command);
}
#endif
