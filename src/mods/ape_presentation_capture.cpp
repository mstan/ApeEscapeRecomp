#include "cpu_state.h"
#include "mod_plugins.h"
#include "pgxp.h"
#include "gpu.h"
#include "presentation_gpu.h"
#include "gpu_gl_renderer.h"
#include "gte_presentation.h"
#include "gpu_render.h"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <map>
#include <tuple>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(PSX_MOD_DEVELOPER_CHANNEL) && PSX_MOD_DEVELOPER_CHANNEL
extern "C" int gte_geometry_correction_enabled(void);

/* SCUS-94423 original-code seams, verified against the boot EXE:
 * 80016A74: skeletal projector's descriptor read; a0 is the descriptor.
 * 80028CD0: original DrawOTag's argument read; own the submitted list.
 * 80016B44: after RTPT and before its first SWC2. a1 is the vertex index,
 * v1 the SVECTOR pointer, t3 the bone index, s0 the matrix array.
 * 800161C4/80016260: rigid descriptor read and completed RTPT batch.
 * The captured data is host-only. No guest instructions are replayed and no
 * guest CPU, RAM, GTE, timing, visibility or packet output is changed.
 * Descriptor addresses are provenance, not yet qualified lifetime identities.
 * This development-only producer leaves live presentation disabled until an
 * explicit request enables it. */
namespace {
constexpr const char* Plugin = "ape.presentation.capture";
constexpr size_t MaxBatches = 8192;
constexpr size_t MaxWords = 262144, MaxNodes = 16384;
enum SourceKind : uint32_t { Skeletal, Rigid, World, TypedWorld, Terrain, Subdivision };
struct Batch {
    uint64_t source;
    uint64_t lifetime;
    uint32_t descriptor, asset, bone, vertex;
    uint32_t owner; // original instance record; templates may serve several owners
    uint32_t local[6], matrix[8], root[8], camera[8], lens[8];
    uint32_t world_root[8], root_domain; // 0 unknown, 1 original pre-camera, 2 authored typed-world
    uint32_t native_xy[3], native_z[3], projection[3];
    GtePresentationProjection presentation[3]{};
    uint32_t presentation_valid=0;
    uint32_t input[6], kind, source_vertex[3], vertex_count;
    uint64_t parents[2]; /* Subdivision: original edge sources, depth guard 164 */
    uint32_t edge_style; /* original raster-edge bias: 0 none, 1 forward, 2 reverse */
};
struct Packet {
    uint32_t address, first, words;
    uint64_t source[4]{};
    int32_t ui_x[4]{},ui_y[4]{};
    bool ui=false;
    double q[4]{},fraction_x[4]{},fraction_y[4]{};
};
struct Frame {
    uint64_t serial = 0;
    uint64_t epoch = 1;
    uint32_t phase=0;
    uint32_t skeletons = 0, rejected = 0, overflow = 0;
    std::vector<Batch> batches;
    std::vector<uint32_t> commands;
    std::vector<Packet> packets;
    uint32_t mapped = 0, partial = 0, unknown = 0, invalid_list = 0;
    uint32_t projected = 0;
    uint64_t texture_version = 0;
    GpuDrawingEnvironment environment{};
    uint8_t filter=0;
    bool perspective=false,geometry=false;
    Frame() {
        batches.reserve(MaxBatches); commands.reserve(MaxWords);
        packets.reserve(MaxNodes);
    }
    void clear(uint64_t id) {
        if(texture_version)psx_presentation_gpu_release(texture_version);
        texture_version=0;environment={};filter=0;perspective=geometry=false;
        serial = id; skeletons = rejected = overflow = 0; batches.clear();
        commands.clear(); packets.clear(); mapped = partial = unknown = invalid_list = 0;
        projected = 0;
    }
    ~Frame() { if(texture_version)psx_presentation_gpu_release(texture_version); }
};
struct Scope {
    uint64_t lifetime = 0;
    uint32_t descriptor = 0, asset = 0, vertices = 0;
    uint32_t owner = 0, owner_word = 0;
    uint32_t matrices = 0, spans = 0, bones = 0;
    uint32_t output = 0, count = 0;
    bool rigid = false;
    SourceKind kind = Skeletal;
    uint32_t quarter_turn = 0;
    int16_t origin[3]{};
};
std::unique_ptr<Frame> writing, published;
std::unique_ptr<Frame> previous_gpu_frame;
std::unique_ptr<Frame> held_gpu_frame;
std::unique_ptr<Frame> pair_gpu_old,pair_gpu_new;
struct SourceRecord {
    Batch batch{};
    uint64_t copied_submission = UINT64_MAX;
};
std::unique_ptr<SourceRecord[]> source_records;
std::mutex published_mutex;
Scope scope;
uint32_t scope_event = 0;
// These leaf projectors may be suspended by a guest interrupt. Their live
// descriptor belongs to the original guest call frame, not the last hook run.
// Stack/return registers only delimit capture activation; vertex/object identities
// remain explicit source indices and constructor lifetimes.
std::unordered_map<uint64_t,Scope> projection_scopes;
std::atomic<bool> active{false};
bool gpu_capture = false;
std::atomic<bool> live_enabled{false};
std::atomic<uint64_t> live_frames{0},live_misses{0},live_cpu_ns{0},live_max_ns{0};
std::atomic<uint64_t> live_prepared{0};
std::atomic<uint64_t> live_reasons[6]{};
std::atomic<const char*> live_failure{""};
bool reported_live_scene=false;
std::atomic<uint64_t> gpu_dropped{0},gpu_dropped_serial{0};
struct PreparedGeometry {
    uint64_t output_version=0;
    uint64_t source_serial=0;
    GpuDrawArea front{},back{};
} prepared_geometry;
void prepare_live(); // caller owns published_mutex and the GL context
CPUState* observed_cpu = nullptr;
uint64_t serial = 0;
std::atomic<uint64_t> source_epoch{1};
uint64_t next_source = 4;
uint64_t next_lifetime = 1;
struct Lifetime { uint64_t token; uint32_t asset, constructor; };
std::unordered_map<uint32_t,Lifetime> lifetimes;
std::atomic<uint64_t> constructions{0}, lifetime_batches{0};
struct CameraComposition {
    uint64_t witness=0;
    uint32_t pointer=0,world[8]{},view[8]{},camera[8]{};
};
std::unordered_map<uint64_t,CameraComposition> pending_compositions;
std::unordered_map<uint32_t,CameraComposition> camera_compositions;
uint64_t next_composition=1;
std::atomic<uint64_t> composition_count{0},rigid_world_batches{0};
std::atomic<uint64_t> world_root_batches{0};
std::atomic<uint64_t> submissions{0}, batches{0}, rejected{0}, overflows{0};
std::atomic<uint64_t> mapped{0}, partial{0}, unknown{0}, invalid_lists{0}, tag_failures{0};
std::atomic<uint64_t> rigid_batches{0};
std::atomic<uint64_t> world_batches{0}, typed_batches{0};
std::atomic<uint64_t> terrain_vertices{0};
std::atomic<uint64_t> subdivision_vertices{0};
bool open = false;
bool reported_tuple[5]{}, reported_typed = false, reported_height = false,reported_edge=false;

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
    scope = {}; scope_event = 0; open = active.load(); serial = 0;
    ++source_epoch;
    psx_presentation_gpu_acquire();
    gpu_dropped_serial=0;
    observed_cpu = nullptr;
    lifetimes.clear();
    projection_scopes.clear();
    pending_compositions.clear();camera_compositions.clear();
    if (writing) writing->clear(0);
    std::lock_guard<std::mutex> lock(published_mutex);
    prepared_geometry={};
    held_gpu_frame.reset();
    pair_gpu_old.reset();pair_gpu_new.reset();
    if (published) published->clear(0);
    if(previous_gpu_frame)previous_gpu_frame->clear(0);
    if (source_records) for (size_t i = 0; i < MaxBatches; ++i) source_records[i] = {};
    pgxp_invalidate_all();
    gte_presentation_capture(active.load());
}
void deactivate() {
    live_enabled=false;active = false; gpu_capture=false; reset(); pgxp_set_source_tracking(0);
}
void activate() {
    active = false; open = false;
    psx_presentation_gpu_acquire();
    live_enabled=false;live_frames=live_misses=live_cpu_ns=live_max_ns=0;
    live_prepared=0;
    reported_live_scene=false;
    gpu_dropped=0;
    for(auto& reason:live_reasons)reason=0;
    writing = std::make_unique<Frame>();
    source_records = std::make_unique<SourceRecord[]>(MaxBatches);
    {
        std::lock_guard<std::mutex> lock(published_mutex);
        published = std::make_unique<Frame>();
        previous_gpu_frame=std::make_unique<Frame>();
    }
    submissions = batches = rejected = overflows = 0;
    mapped = partial = unknown = invalid_lists = tag_failures = 0;
    rigid_batches = 0;
    world_batches = typed_batches = 0;
    terrain_vertices = 0;
    subdivision_vertices = 0;
    constructions = lifetime_batches = 0;
    world_root_batches = 0;
    composition_count=rigid_world_batches=0;
    for (bool& reported : reported_tuple) reported = false;
    reported_typed = reported_height = reported_edge = false;
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
    const uint32_t key=descriptor & 0x1FFFFFFFu;
    auto entry=lifetimes.find(key);
    if(entry==lifetimes.end()) {
        // Activation/restore starts an empty epoch. An already live descriptor
        // may predate capture, so establish continuity at its first observed
        // projector read. Every later header write still invalidates this tag;
        // an existing invalid entry can only be renewed by a constructor.
        if(lifetimes.size()==4096) {++writing->overflow;return 0;}
        const uint64_t token=(uint64_t(1)<<63)|next_lifetime++;
        if(!pgxp_tag_word_source(descriptor,asset,token)) {++tag_failures;return 0;}
        entry=lifetimes.emplace(key,Lifetime{token,asset,0}).first;
    }
    return entry!=lifetimes.end() && entry->second.asset==asset &&
        pgxp_load_source(descriptor,asset)==entry->second.token ? entry->second.token : 0;
}
uint64_t projection_frame(const CPUState* cpu) {
    return uint64_t(cpu->gpr[29])<<32 | cpu->gpr[31];
}
void camera_composition_before(CPUState* cpu,uint32_t) {
    const uint64_t frame=projection_frame(cpu);pending_compositions.erase(frame);
    if(!open || (cpu->gpr[4]&0x1FFFFFFFu)!=0xE78C0u)return;
    if(!ram(cpu->gpr[5],32,4) || !ram(cpu->gpr[6],32,4)) {++writing->rejected;return;}
    if(pending_compositions.size()==256) {++writing->overflow;return;}
    CameraComposition op;op.pointer=cpu->gpr[6];
    read_words(cpu->gpr[4],op.camera,8);read_words(cpu->gpr[5],op.world,8);
    pending_compositions.emplace(frame,op);
}
void camera_composition_after(CPUState* cpu,uint32_t) {
    const auto pending=pending_compositions.find(projection_frame(cpu));
    if(!open || pending==pending_compositions.end())return;
    CameraComposition op=pending->second;pending_compositions.erase(pending);
    if(cpu->gpr[6]!=op.pointer) {++writing->rejected;return;}
    const uint32_t key=op.pointer&0x1FFFFFFFu;
    if(camera_compositions.size()==4096 && !camera_compositions.count(key)) {++writing->overflow;return;}
    read_words(op.pointer,op.view,8);
    op.witness=(uint64_t(1)<<62)|next_composition++;
    if(!pgxp_tag_word_source(op.pointer,op.view[0],op.witness)) {++tag_failures;return;}
    camera_compositions[key]=op;++composition_count;
}
void scope_begin(CPUState* cpu,uint32_t pc) {
    projection_scopes.erase(projection_frame(cpu));scope={};scope_event=pc;
}
void scope_bind(CPUState* cpu) {
    if (projection_scopes.size()==256) { ++writing->overflow; return; }
    projection_scopes[projection_frame(cpu)]=scope;
}
void scope_resume(CPUState* cpu) {
    const auto current=projection_scopes.find(projection_frame(cpu));
    scope=current!=projection_scopes.end() ? current->second : Scope{};
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
                            const uint32_t word=writing->commands[p.first+offset];
                            const int x=(int(word&2047)^1024)-1024,y=(int((word>>16)&2047)^1024)-1024;
                            int32_t px=0,py=0;uint16_t depth=0;
                            if(writing->geometry && pgxp_get_precise_vertex(p.address+4*offset,word,x,y,&px,&py,&depth)!=PGXP_SRC_NATIVE) {
                                p.fraction_x[i]=px/65536.0-x;p.fraction_y[i]=py/65536.0-y;
                            }
                            if(writing->perspective && pgxp_load_precise_word(p.address+4*offset,word,&px,&py,&depth) && depth)
                                p.q[i]=1.0/depth;
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
void gpu_frame_ready(void* data,uint64_t version) {
    std::unique_ptr<Frame> frame(static_cast<Frame*>(data));frame->texture_version=version;
    if(frame->epoch!=source_epoch.load())return;
    {
        std::lock_guard<std::mutex> lock(published_mutex);
        previous_gpu_frame=std::move(published);published=std::move(frame);
    }
    // GPU ownership serializes preparation with presents. Immutable frames
    // can be inspected without holding the emulation-side publication lock.
    if(live_enabled)prepare_live();
}
void capture_ui(Frame& frame) {
    int offset_x=frame.environment.area.offset_x,offset_y=frame.environment.area.offset_y;
    for(auto& packet:frame.packets) {
        const auto* w=frame.commands.data()+packet.first;const unsigned op=w[0]>>24;
        if(op==0xE5) {
            offset_x=(int(w[0]&2047)^1024)-1024;
            offset_y=(int((w[0]>>11)&2047)^1024)-1024;continue;
        }
        unsigned n=0;bool rectangle=false;
        if((op&0xE0u)==0x20u) {
            n=(op&8)?4:3;const unsigned stride=1+unsigned((op&4)!=0)+unsigned((op&16)!=0);
            for(unsigned i=0;i<n;++i) {
                packet.ui_x[i]=(int(w[1+i*stride]&2047)^1024)-1024;
                packet.ui_y[i]=(int((w[1+i*stride]>>16)&2047)^1024)-1024;
            }
        } else if((op&0xE0u)==0x60u) {
            rectangle=true;n=4;const bool texture=(op&4)!=0;const unsigned size=(op>>3)&3;
            const unsigned fixed[4]={0,1,8,16};const uint32_t wh=size?0:w[texture?3:2];
            const unsigned width=size?fixed[size]:(texture?(wh&1023):std::min(wh&65535u,1023u));
            const unsigned height=size?fixed[size]:(texture?((wh>>16)&511):std::min(wh>>16,511u));
            if(!width || !height)continue;
            const int x=(int(w[1]&2047)^1024)-1024,y=(int((w[1]>>16)&2047)^1024)-1024;
            for(unsigned i=0;i<n;++i) {packet.ui_x[i]=x+((i&1)?width:0);packet.ui_y[i]=y+((i&2)?height:0);}
        }
        if(n)packet.ui=gpu_ws_copy_presented_ui(packet.address,w,packet.words,packet.ui_x,packet.ui_y,n,rectangle)!=0;
        if(packet.ui)for(unsigned i=0;i<n;++i) {packet.ui_x[i]+=offset_x;packet.ui_y[i]+=offset_y;}
    }
}
void finish(CPUState* cpu, uint32_t) {
    if (!open) return;
    observed_cpu=cpu;
    open = false; scope = {}; scope_event = 0x80028CD0u;
    if(gpu_capture) {
        gpu_get_drawing_environment(&writing->environment);writing->filter=uint8_t(gr_texture_filter());
        writing->perspective=gpu_texture_correction_enabled()!=0;
        writing->geometry=gte_geometry_correction_enabled()!=0;
        // Reuse the ordinary host UI prepass so copied presentation commands
        // own the same widget placement. This never executes guest drawing.
        gpu_ws_prepass_linked_list(cpu->gpr[4]);
    }
    capture_list(cpu->gpr[4]);
    if(gpu_capture)capture_ui(*writing);
    writing->epoch=source_epoch;
    writing->phase=psx_mod_read_word(0x800F447Cu)&255;
    batches += writing->projected; rejected += writing->rejected;
    overflows += writing->overflow; ++submissions;
    mapped += writing->mapped; partial += writing->partial;
    unknown += writing->unknown; invalid_lists += writing->invalid_list;
    if(gpu_capture) {
        Frame* completed=writing.release();writing=std::make_unique<Frame>();
        writing->clear(++serial);open=true;
        if(!psx_presentation_gpu_snapshot_async(gpu_frame_ready,completed)) {
            gpu_dropped_serial=completed->serial+1;++gpu_dropped;delete completed;
        }
        return;
    }
    std::lock_guard<std::mutex> lock(published_mutex);
    writing.swap(published);
    writing->clear(++serial); open = true;
}
void enter(CPUState* cpu, uint32_t pc) {
    scope_begin(cpu,pc);
    if (!open) return;
    ++writing->skeletons;
    const uint32_t descriptor = cpu->gpr[4];
    if (!ram(descriptor, 24, 4)) { ++writing->rejected; return; }
    const uint32_t header = psx_mod_read_word(descriptor);
    if (!ram(header, 10, 2)) { ++writing->rejected; return; }
    Scope s;
    s.descriptor = descriptor; s.asset = header;
    if(cpu->gpr[31]==0x80071C28u) {
        // The original mailbox funnel passes a shared model template. Its
        // placement pointer is the caller's saved s1, not a model identity.
        // 80016A60 saves s1 at sp+4 before replacing it with the output arena.
        const uint32_t count=psx_mod_read_word(0x800BBC50u);
        if(!ram(cpu->gpr[29],8,4) || !count || count>4) {++writing->rejected;return;}
        const uint32_t owner=psx_mod_read_word(cpu->gpr[29]+4);
        if(owner<0x800BBD34u || (owner-0x800BBD34u)%24 ||
            (owner-0x800BBD34u)/24>=count) {++writing->rejected;return;}
        const uint32_t owner_word=psx_mod_read_word(owner),type=owner_word&0xFFFFu;
        if(type>2 || descriptor!=(type<2 ? 0x800BBC54u : 0x800BBCC4u)) {
            ++writing->rejected;return;
        }
        s.owner=owner;s.owner_word=owner_word;
    }
    s.lifetime=lifetime(s.owner ? s.owner : descriptor,s.owner ? s.owner_word : header);
    s.bones = psx_mod_read_half(header + 4);
    s.vertices = psx_mod_read_word(descriptor + 16);
    s.matrices = psx_mod_read_word(descriptor + 20);
    s.spans = psx_mod_read_word(descriptor + 12);
    if (!s.bones) return; // the original empty-model path performs no projections
    if (s.bones > 128 || !ram(s.matrices, (s.bones + 1) * 32, 4) ||
        !ram(s.spans, s.bones * 12, 2) || !ram(s.vertices, 8, 4)) {
        ++writing->rejected; return;
    }
    scope = s;scope_bind(cpu);
}
void leave(CPUState* cpu, uint32_t pc) { projection_scopes.erase(projection_frame(cpu));scope={};scope_event=pc; }
bool capture(CPUState* cpu, uint32_t bone, uint32_t vertex, uint32_t local,
             const uint32_t* input = nullptr, unsigned count = 3,
             const uint32_t* indices = nullptr, unsigned fifo = 0) {
    Batch b{};
    b.source = next_source;
    b.descriptor = scope.descriptor; b.asset = scope.asset;
    b.owner=scope.owner ? scope.owner : scope.descriptor;
    const uint32_t owner_word=scope.owner ? scope.owner_word : scope.asset;
    if (scope.lifetime && pgxp_load_source(b.owner,owner_word)==scope.lifetime)
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
        if(gte_presentation_projection(fifo+i,b.native_xy[i],b.native_z[i],&b.presentation[i]))
            b.presentation_valid|=1u<<i;
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
    // Original camera construction premultiplies by this authored projection
    // matrix via 8002FF50. Its Y scale belongs to the lens, not camera rotation.
    read_words(0x800AF0A0u, b.lens, 8);
    if (scope.kind==TypedWorld) {
        std::memcpy(b.world_root,b.root,sizeof b.root);b.root_domain=2;
    } else {
        const auto composed=camera_compositions.find(scope.matrices&0x1FFFFFFFu);
        if(composed!=camera_compositions.end() &&
            pgxp_load_source(scope.matrices,b.root[0])==composed->second.witness &&
            !std::memcmp(composed->second.view,b.root,sizeof b.root) &&
            !std::memcmp(composed->second.camera,b.camera,sizeof b.camera)) {
            std::memcpy(b.world_root,composed->second.world,sizeof b.world_root);b.root_domain=1;
            if(scope.kind==Rigid)++rigid_world_batches;
            if(scope.kind==Skeletal)++world_root_batches;
        }
    }
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
    scope_resume(cpu);
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
    scope_begin(cpu,pc);
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
    scope = s;scope_bind(cpu);
}
void projected_rigid(CPUState* cpu, uint32_t) {
    if (!open) return;
    scope_resume(cpu);
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
void enter_typed(CPUState* cpu, uint32_t pc) {
    scope_begin(cpu,pc);
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
    scope = s;scope_bind(cpu);
}
void projected_typed(CPUState* cpu, uint32_t pc) {
    if (!open) return;
    scope_resume(cpu);
    const uint32_t vertex = cpu->gpr[9];
    const uint32_t turn = pc == 0x80016394u ? 1u : pc == 0x80016514u ? 2u :
        pc == 0x80016690u ? 3u : 0u;
    const uint32_t local = cpu->gpr[turn ? 7 : 4];
    if (!scope.descriptor || scope.kind != TypedWorld || turn != scope.quarter_turn ||
        vertex >= scope.count || vertex % 3 || local != scope.vertices + vertex * 8 ||
        cpu->gpr[5] != scope.output + vertex * 8 || cpu->gpr[13] != scope.count || !ram(local, 24, 4)) {
        if (!reported_typed) {
            reported_typed = true;
            std::fprintf(stderr, "Ape typed scope mismatch pc=%08X turn=%u expected_turn=%u vertex=%u count=%u/%u source=%08X/%08X output=%08X/%08X scope_event=%08X ra=%08X\n",
                pc, turn, scope.quarter_turn, vertex, cpu->gpr[13], scope.count,
                local, scope.vertices+vertex*8, cpu->gpr[5], scope.output+vertex*8,
                scope_event, cpu->gpr[31]);
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
    unsigned a=0,b=0,out=0,style=0;
    uint32_t block=cpu->gpr[17];
    const bool terrain_seed=pc==0x8001DB74u && cpu->gpr[31]==0x80045BCCu;
    if(terrain_seed) {
        // 80044EF4 first splits a terrain triangle into a GT4. Its original
        // 80045BBC/45BC0 stores produce the midpoint of input corners 0/3.
        // The call has a fixed scratch block and depth, before any child load.
        if(cpu->gpr[6]!=0x1F80033Cu || cpu->gpr[7]!=1 || cpu->gpr[14]!=0x1F800000u)return;
        // Both triangle paths retain their scratch base in t3 or t4. The
        // ordinary quad path instead holds native depth/SXY there and jumps
        // directly to the shared call: its corner 1 is an existing vertex.
        if(cpu->gpr[11]!=0x1F80033Cu && cpu->gpr[12]!=0x1F80033Cu)return;
        block=cpu->gpr[6];a=0;b=24;out=8;style=1;
    } else {
        if(pc==0x8001DB74u) {
            // The last half is written in the recursive JAL delay slot.
            if(cpu->gpr[6]!=cpu->gpr[17]+64)return;
            pc=cpu->gpr[31];
        }
        switch (pc) {
            case 0x8001DF70: a=0;b=8;out=72;style=1;break;
            case 0x8001E0D0: a=0;b=16;out=80;style=2;break;
            case 0x8001E20C: a=8;b=16;out=88;break;
            case 0x8001E408: a=8;b=24;out=88;style=1;break;
            case 0x8001E5FC: a=16;b=24;out=80;style=2;break;
            case 0x8001E7F4: a=0;b=16;out=64;style=2;break;
            default:return;
        }
    }
    if (!ram(block,128,4)) { ++writing->rejected; return; }
    const uint64_t p0=pgxp_load_source(block+a,psx_mod_read_word(block+a));
    const uint64_t p1=pgxp_load_source(block+b,psx_mod_read_word(block+b));
    if (!p0 || !p1) return; // unknown parent stays unknown; never fill by position
    const auto& s0=source_records[(p0>>2)%MaxBatches].batch;
    const auto& s1=source_records[(p1>>2)%MaxBatches].batch;
    if(s0.source!=(p0&~uint64_t(3)) || s1.source!=(p1&~uint64_t(3)) ||
        (p0&3)>=s0.vertex_count || (p1&3)>=s1.vertex_count)return;
    if(terrain_seed && (s0.kind!=Terrain || s1.kind!=Terrain))return;
    const uint32_t xy0=psx_mod_read_word(block+a),xy1=psx_mod_read_word(block+b);
    const uint16_t depth0=psx_mod_read_half(block+a+4),depth1=psx_mod_read_half(block+b+4);
    if(s0.native_xy[p0&3]!=xy0 || s1.native_xy[p1&3]!=xy1 ||
        s0.native_z[p0&3]!=depth0 || s1.native_z[p1&3]!=depth1) {
        if(!reported_edge)std::fprintf(stderr,"Ape edge parent mismatch pc=%08X xy=%08X/%08X expected=%08X/%08X z=%u/%u expected=%u/%u\n",
            pc,xy0,xy1,s0.native_xy[p0&3],s1.native_xy[p1&3],depth0,depth1,s0.native_z[p0&3],s1.native_z[p1&3]);
        reported_edge=true;++writing->rejected;return;
    }
    const int za=std::max(164,int(int16_t(depth0))),zb=std::max(164,int(int16_t(depth1))),sum=za+zb;
    const int xa=int16_t(xy0),xb=int16_t(xy1),ya=int16_t(xy0>>16),yb=int16_t(xy1>>16);
    const int bias_x=style==1 ? ya<yb : style==2 ? yb<ya : 0;
    const int bias_y=style==1 ? xb<xa : style==2 ? xa<xb : 0;
    const uint32_t expected=uint16_t((xa*za+xb*zb+sum*bias_x)/sum) |
        uint32_t(uint16_t((ya*za+yb*zb+sum*bias_y)/sum))<<16;
    if(psx_mod_read_word(block+out)!=expected || psx_mod_read_half(block+out+4)!=sum/2) {
        if(!reported_edge)std::fprintf(stderr,"Ape edge recipe mismatch pc=%08X xy=%08X expected=%08X z=%u expected=%u\n",
            pc,psx_mod_read_word(block+out),expected,psx_mod_read_half(block+out+4),sum/2);
        reported_edge=true;
        ++writing->rejected;return;
    }
    Batch d{};
    d.source=next_source;d.kind=Subdivision;d.vertex_count=1;
    d.parents[0]=p0;d.parents[1]=p1;
    d.edge_style=style;
    d.native_xy[0]=psx_mod_read_word(block+out);
    d.native_z[0]=psx_mod_read_half(block+out+4);
    SourceRecord& record=source_records[(next_source>>2)%MaxBatches];
    record.batch=d;record.copied_submission=UINT64_MAX;
    if (!pgxp_tag_word_source(block+out,d.native_xy[0],next_source)) { ++tag_failures; return; }
    next_source+=4;++subdivision_vertices;
}
#include "ape_presentation_sample.inc"
struct GeometryDraws {
    std::vector<PsxPresentationTriangle> triangles;
    std::vector<PsxPresentationMaterial> materials;
    uint32_t skipped=0,sampled_triangles=0;
    uint32_t resource_copies=0;
    uint8_t skipped_ops[8]{};
    const char* error="";
};
bool build_geometry_draws(const Frame& f,const GeometrySamples* geometry,GeometryDraws& draws) {
    if(!f.texture_version) {draws.error="no GPU snapshot";return false;}
    auto& triangles=draws.triangles;auto& materials=draws.materials;
    struct ResourceWrite {int x,y,w,h;};
    std::vector<ResourceWrite> resource_writes;
    const auto overlaps=[](const ResourceWrite& a,const ResourceWrite& b) {
        return a.x<b.x+b.w && b.x<a.x+a.w && a.y<b.y+b.h && b.y<a.y+a.h;
    };
    const auto valid_texture_reads=[&](const PsxPresentationMaterial& m) {
        if(!(m.flags&PSX_PRESENTATION_TEXTURED))return true;
        const unsigned mode=(m.texpage>>7)&3;
        const unsigned shift=mode<2 ? 2-mode : 0;
        unsigned u0=0,u1=255,v0=0,v1=255;
        // The ordinary shader clamps every no-window fetch, including filter
        // footprints, to the authored UV bounds. A different atlas region in
        // the same page is not a read dependency of this primitive.
        if(!(m.texture_window&1023u)) {
            u0=m.uv_limits[0];u1=m.uv_limits[2];v0=m.uv_limits[1];v1=m.uv_limits[3];
        }
        ResourceWrite page={int((m.texpage&15)*64+(u0>>shift)),int(((m.texpage>>4)&1)*256+v0),
            int((u1>>shift)-(u0>>shift)+1),int(v1-v0+1)};
        ResourceWrite clut={int(m.clut_x),int(m.clut_y),mode?256:16,1};
        if(page.x+page.w>1024) {page.x=0;page.w=1024;}
        if(page.y+page.h>512) {page.y=0;page.h=512;}
        if(clut.x+clut.w>1024) {clut.x=0;clut.w=1024;}
        for(const auto& write:resource_writes)
            if(overlaps(write,page) || (mode<2 && overlaps(write,clut)))return false;
        return true;
    };
    triangles.reserve(f.packets.size()*2);materials.reserve(f.packets.size());
    GpuDrawingEnvironment env=f.environment;
    const float color5=gl_renderer_accurate_blending()?8.0f/255.0f:1.0f/31.0f;
    auto& skipped=draws.skipped;auto& sampled_triangles=draws.sampled_triangles;
    std::unique_ptr<PsxPresentationDraw,decltype(&psx_presentation_draw_destroy)> clipped(psx_presentation_draw_create(3,6),psx_presentation_draw_destroy);
    if(geometry && !clipped) {draws.error="geometry workspace unavailable";return false;}
    for(const Packet& p:f.packets) {
        const uint32_t* w=f.commands.data()+p.first;const unsigned op=w[0]>>24;
        switch(op) {
        case 0xE1:env.mode=(env.mode&~0x83FFu)|(w[0]&0x3FFu)|((w[0]&(1u<<11))<<4);continue;
        case 0xE2:env.texture_window=w[0]&0xFFFFFu;continue;
        case 0xE3:env.area.left=w[0]&1023;env.area.top=(w[0]>>10)&511;continue;
        case 0xE4:env.area.right=w[0]&1023;env.area.bottom=(w[0]>>10)&511;continue;
        case 0xE5:{const int32_t a=int32_t(w[0]&0x7FF),b=int32_t((w[0]>>11)&0x7FF);
            env.area.offset_x=(a^1024)-1024;env.area.offset_y=(b^1024)-1024;continue;}
        case 0xE6:env.mode=(env.mode&~0x1800u)|((w[0]&3u)<<11);continue;
        default:break;
        }
        if((op&0xE0u)==0x80u) {
            const ResourceWrite destination={int(w[2]&1023),int((w[2]>>16)&511),
                int(((w[3]-1)&1023)+1),int((((w[3]>>16)-1)&511)+1)};
            const auto& area=f.environment.area;
            const ResourceWrite display={int(area.left),int(area.top),int(area.right-area.left+1),int(area.bottom-area.top+1)};
            if(destination.x+destination.w>1024 || destination.y+destination.h>512 || overlaps(destination,display)) {
                draws.error="visible or wrapped VRAM copy needs its own presentation source";return false;
            }
            // Native execution performs the resource update. It is irrelevant
            // to this image only if no later draw reads the written texture or
            // CLUT. The next immutable snapshot owns the native updated data.
            resource_writes.push_back(destination);++draws.resource_copies;continue;
        }
        if((op&0xE0u)==0x60u) {
            const bool texture=(op&4)!=0;const unsigned size=(op>>3)&3;
            const unsigned fixed[4]={0,1,8,16};
            const uint32_t wh=size?0:w[texture?3:2];
            const unsigned width=size?fixed[size]:(texture?(wh&1023):std::min(wh&65535u,1023u));
            const unsigned height=size?fixed[size]:(texture?((wh>>16)&511):std::min(wh>>16,511u));
            if(!width || !height)continue;
            PsxPresentationMaterial m{};m.texture_version=f.texture_version;m.texture_window=env.texture_window;
            m.texpage=uint16_t(env.mode&511);m.semi=(op&2)?int16_t((env.mode>>5)&3):-1;
            m.filter=f.filter;
            m.clip[0]=int16_t(env.area.left);m.clip[1]=int16_t(env.area.top);
            m.clip[2]=int16_t(env.area.right);m.clip[3]=int16_t(env.area.bottom);
            if(env.mode&(1u<<11))m.flags|=PSX_PRESENTATION_MASK_SET;
            if(env.mode&(1u<<12))m.flags|=PSX_PRESENTATION_MASK_CHECK;
            unsigned u=0,vv=0;
            if(texture) {
                m.flags|=PSX_PRESENTATION_TEXTURED;if(op&1)m.flags|=PSX_PRESENTATION_RAW_TEXTURE;
                const unsigned clut=w[2]>>16;m.clut_x=uint16_t((clut&63)*16);m.clut_y=uint16_t((clut>>6)&511);
                u=w[2]&255;vv=(w[2]>>8)&255;
                m.uv_limits[0]=uint16_t(u+width<=256?u:0);m.uv_limits[2]=uint16_t(u+width<=256?u+width-1:255);
                m.uv_limits[1]=uint16_t(vv+height<=256?vv:0);m.uv_limits[3]=uint16_t(vv+height<=256?vv+height-1:255);
            }
            const int x=((int(w[1]&2047)^1024)-1024)+env.area.offset_x;
            const int y=((int((w[1]>>16)&2047)^1024)-1024)+env.area.offset_y;
            PsxPresentationDrawVertex vertices[4]{};
            for(unsigned j=0;j<4;++j) {
                vertices[j].x=p.ui?p.ui_x[j]:x+((j&1)?width:0);vertices[j].y=p.ui?p.ui_y[j]:y+((j&2)?height:0);
                vertices[j].uv[0]=float(u+((j&1)?width:0));vertices[j].uv[1]=float(vv+((j&2)?height:0));
                for(unsigned k=0;k<3;++k)vertices[j].color[k]=texture?float((w[0]>>(k*8))&255)/255.0f:
                    float((w[0]>>(k*8+3))&31)*color5;
                vertices[j].color[3]=1;
            }
            if(!valid_texture_reads(m)) {draws.error="in-frame texture feedback needs a later resource version";return false;}
            const uint32_t resource=uint32_t(materials.size());materials.push_back(m);
            const unsigned indices[2][3]={{0,1,2},{1,2,3}};
            for(unsigned j=0;j<2;++j) {
                PsxPresentationTriangle t{};t.identity=p.address;t.resource=resource;t.flags=PSX_PRESENTATION_AFFINE_UV;
                for(unsigned k=0;k<3;++k)t.vertices[k]=vertices[indices[j][k]];
                triangles.push_back(t);
            }
            continue;
        }
        if((op&0xE0u)!=0x20u) { if(op!=0 && op!=1) {if(skipped<8)draws.skipped_ops[skipped]=uint8_t(op);++skipped;}continue; }
        const bool texture=(op&4)!=0,shaded=(op&16)!=0;
        const unsigned n=(op&8)?4:3,stride=1+unsigned(texture)+unsigned(shaded);
        if(p.words!=n*stride+unsigned(!shaded)) { ++skipped;continue; }
        PsxPresentationMaterial material{};material.texture_version=f.texture_version;
        material.filter=f.filter;
        material.texture_window=env.texture_window;
        if(texture) {
            material.texpage=uint16_t(w[2+stride]>>16);
            env.mode=(env.mode&~511u)|(material.texpage&511u);
            const uint32_t clut=w[2]>>16;material.clut_x=uint16_t((clut&63)*16);material.clut_y=uint16_t(clut>>6);
            material.flags|=PSX_PRESENTATION_TEXTURED;
            if(op&1)material.flags|=PSX_PRESENTATION_RAW_TEXTURE;
        }
        material.semi=(op&2)?int16_t((env.mode>>5)&3):-1;
        if(env.mode&(1u<<11))material.flags|=PSX_PRESENTATION_MASK_SET;
        if(env.mode&(1u<<12))material.flags|=PSX_PRESENTATION_MASK_CHECK;
        if((env.mode&(1u<<9)) && ((texture && !(op&1)) || shaded))material.flags|=PSX_PRESENTATION_DITHER;
        material.clip[0]=int16_t(env.area.left);material.clip[1]=int16_t(env.area.top);
        material.clip[2]=int16_t(env.area.right);material.clip[3]=int16_t(env.area.bottom);
        material.uv_limits[0]=material.uv_limits[1]=255;
        PsxPresentationDrawVertex v[4]{};
        for(unsigned j=0;j<n;++j) {
            const uint32_t xy=w[1+j*stride];
            v[j].x=((int32_t(xy&2047)^1024)-1024)+env.area.offset_x;
            v[j].y=((int32_t((xy>>16)&2047)^1024)-1024)+env.area.offset_y;
            if(p.ui) {v[j].x=p.ui_x[j];v[j].y=p.ui_y[j];}
            v[j].x+=p.fraction_x[j];v[j].y+=p.fraction_y[j];v[j].reciprocal_z=p.q[j];
            const uint32_t color=shaded?w[j*stride]:w[0];
            for(unsigned k=0;k<3;++k)v[j].color[k]=texture?float((color>>(k*8))&255)/255.0f:
                float((color>>(k*8+3))&31)*color5;
            v[j].color[3]=1;
            if(texture) {
                const uint32_t uv=w[2+j*stride];v[j].uv[0]=float(uv&255);v[j].uv[1]=float((uv>>8)&255);
                for(unsigned k=0;k<2;++k) {
                    material.uv_limits[k]=uint16_t(std::min(unsigned(material.uv_limits[k]),unsigned(v[j].uv[k])));
                    material.uv_limits[k+2]=uint16_t(std::max(unsigned(material.uv_limits[k+2]),unsigned(v[j].uv[k])));
                }
            }
        }
        if(!valid_texture_reads(material)) {draws.error="in-frame texture feedback needs a later resource version";return false;}
        const uint32_t resource=uint32_t(materials.size());materials.push_back(material);
        const unsigned indices[2][3]={{0,1,2},{1,2,3}};
        for(unsigned j=0;j<(n==4?2u:1u);++j) {
            PsxPresentationTriangle t{};t.identity=p.address;t.resource=resource;t.flags=PSX_PRESENTATION_AFFINE_UV;
            for(unsigned k=0;k<3;++k)t.vertices[k]=v[indices[j][k]];
            if(texture && f.perspective && t.vertices[0].reciprocal_z>0 &&
                t.vertices[1].reciprocal_z>0 && t.vertices[2].reciprocal_z>0)t.flags=0;
            PsxPresentationSample sampled[3]{};bool qualified=geometry && clipped && !p.ui;
            for(unsigned k=0;k<3 && qualified;++k) {
                const unsigned vertex=indices[j][k];
                const auto point=geometry->vertices.find(p.source[vertex]);
                if(point==geometry->vertices.end()) {qualified=false;break;}
                sampled[k]=point->second;sampled[k].identity=uint64_t(p.address)+1;
                sampled[k].resource=resource;sampled[k].flags=t.flags;
                sampled[k].rgba=0;
                for(unsigned component=0;component<4;++component)
                    sampled[k].rgba|=uint32_t(std::lround(std::max(0.0f,std::min(1.0f,v[vertex].color[component]))*255.0f))<<(component*8);
                sampled[k].uv[0]=v[vertex].uv[0];sampled[k].uv[1]=v[vertex].uv[1];
            }
            if(qualified) {
                auto camera=geometry->camera;camera.center_x+=env.area.offset_x;camera.center_y+=env.area.offset_y;
                const PsxPresentationViewport viewport={double(material.clip[0]),double(material.clip[1]),double(material.clip[2])+1,double(material.clip[3])+1};
                if(!psx_presentation_draw_samples(clipped.get(),sampled,3,&camera,&viewport)) {
                    draws.error="invalid sampled geometry";return false;
                }
                uint32_t count=0;const auto* output=psx_presentation_draw_triangles(clipped.get(),&count);
                triangles.insert(triangles.end(),output,output+count);++sampled_triangles;continue;
            }
            triangles.push_back(t);
        }
    }
    if(skipped) {draws.error="unsupported native command requires whole-image fallback";return false;}
    return true;
}
void prepare_live() {
    prepared_geometry={};
    const Frame* old=previous_gpu_frame.get(),*now=published.get();
    if(!old || !now || !old->texture_version || !now->texture_version ||
        old->serial+1!=now->serial || old->epoch!=now->epoch || now->serial+1<=gpu_dropped_serial.load() ||
        old->rejected || old->overflow || old->invalid_list || now->rejected || now->overflow || now->invalid_list) {
        live_failure="pair";++live_reasons[1];return;
    }
    if(old->phase!=3 || now->phase!=3) {live_failure="phase";return;}
    const auto& a=old->environment.area;const auto& b=now->environment.area;
    if((a.left==b.left && a.top==b.top) || a.right-a.left!=b.right-b.left || a.bottom-a.top!=b.bottom-b.top) {
        live_failure="display binding";++live_reasons[2];return;
    }
    const auto start=std::chrono::steady_clock::now();
    GeometrySamples samples;GeometryDraws draws;
    if(!geometry_samples(*old,*now,.5,samples) || !samples.paired) {
        live_failure=samples.error;++live_reasons[3];
        if(!reported_live_scene) {
            reported_live_scene=true;
            std::fprintf(stderr,"Ape presentation source %llu/%llu: %s; camera old/new/lens",
                (unsigned long long)old->serial,(unsigned long long)now->serial,samples.error);
            for(const auto* matrix:{samples.old_camera_linear,samples.camera_linear,samples.lens_linear}) {
                std::fputs(" [",stderr);for(unsigned i=0;i<9;++i)std::fprintf(stderr,"%s%d",i?",":"",int(matrix[i]));std::fputs("]",stderr);
            }
            std::fputc('\n',stderr);
        }
        return;
    }
    if(!build_geometry_draws(*now,&samples,draws) || !draws.sampled_triangles) {
        live_failure=draws.error;++live_reasons[4];return;
    }
    if(!psx_presentation_gpu_render(draws.triangles.data(),uint32_t(draws.triangles.size()),
        draws.materials.data(),uint32_t(draws.materials.size()))) {live_failure="GPU render";++live_reasons[5];return;}
    const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count());
    live_cpu_ns+=elapsed;if(elapsed>live_max_ns.load())live_max_ns=elapsed;
    ++live_prepared;live_failure="";
    prepared_geometry.front=a;prepared_geometry.back=b;
    prepared_geometry.source_serial=now->serial+1;
    prepared_geometry.output_version=psx_presentation_gpu_output_version();
}
int live_present(void*,int display_x,int display_y,int width,int height,int repeated,int output[4]) {
    if(!live_enabled.load() || !repeated)return 0;
    const auto& a=prepared_geometry.front;const auto& b=prepared_geometry.back;
    if(!prepared_geometry.output_version || prepared_geometry.source_serial<=gpu_dropped_serial.load() || int(a.left)!=display_x || int(a.top)!=display_y ||
        int(a.right-a.left+1)!=width || int(a.bottom-a.top+1)!=height ||
        !psx_presentation_gpu_output_ready(prepared_geometry.output_version)) {++live_misses;return 0;}
    ++live_frames;
    output[0]=int(b.left);output[1]=int(b.top);output[2]=width;output[3]=height;return 1;
}
int render_native_geometry(const Frame& f,const char* path,char* reply,int cap,const GeometrySamples* geometry=nullptr) {
    GeometryDraws draws;
    if(!build_geometry_draws(f,geometry,draws)) {std::snprintf(reply,size_t(cap),"{\"error\":\"%s\"}",draws.error);return 0;}
    const auto& triangles=draws.triangles;const auto& materials=draws.materials;
    const uint32_t skipped=draws.skipped,sampled_triangles=draws.sampled_triangles;
    char skipped_ops[64]{};unsigned skipped_pos=0;
    for(unsigned i=0;i<std::min(skipped,8u);++i)skipped_pos+=unsigned(std::snprintf(skipped_ops+skipped_pos,sizeof(skipped_ops)-skipped_pos,"%s%u",i?",":"",unsigned(draws.skipped_ops[i])));
    const GpuDrawArea& area=f.environment.area;
    // One emulation-thread command boundary: guest execution cannot advance
    // between the before/after observations. Export checks are deliberately
    // separate from cadence/performance measurement.
    const CPUState cpu_before=observed_cpu?*observed_cpu:CPUState{};
    std::vector<uint32_t> ram_before(0x200000/4);
    for(uint32_t i=0;i<ram_before.size();++i)ram_before[i]=psx_mod_read_word(i*4);
    const uint16_t* vram=gpu_get_vram();
    const std::vector<uint16_t> vram_before(vram,vram+1024*512);
    GlScaleInfo scale{};
    if(!gl_renderer_scale_info(&scale) || scale.effective<1 || scale.effective>64) {
        std::snprintf(reply,size_t(cap),"{\"error\":\"native raster scale unavailable\"}");return 0;
    }
    const size_t image_capacity=size_t(area.right-area.left+1)*size_t(area.bottom-area.top+1)*
        size_t(scale.effective)*size_t(scale.effective);
    if(!image_capacity || image_capacity>size_t(INT32_MAX))return 0;
    std::vector<uint32_t> image_before(image_capacity),image_after(image_capacity);
    int ow=0,oh=0,aw=0,ah=0;
    const int before_pixels=gl_renderer_read_display_hires(int(area.left),int(area.top),int(area.right-area.left+1),int(area.bottom-area.top+1),
        image_before.data(),int(image_before.size()),&ow,&oh);
    const bool rendered=psx_presentation_gpu_render(triangles.data(),uint32_t(triangles.size()),materials.data(),uint32_t(materials.size()))!=0;
    const int after_pixels=gl_renderer_read_display_hires(int(area.left),int(area.top),int(area.right-area.left+1),int(area.bottom-area.top+1),
        image_after.data(),int(image_after.size()),&aw,&ah);
    const bool cpu_same=observed_cpu && !std::memcmp(&cpu_before,observed_cpu,sizeof cpu_before);
    bool ram_same=true;
    for(uint32_t i=0;i<ram_before.size();++i)if(ram_before[i]!=psx_mod_read_word(i*4)) { ram_same=false;break; }
    const bool vram_same=!std::memcmp(vram_before.data(),gpu_get_vram(),vram_before.size()*sizeof(uint16_t));
    const bool image_same=before_pixels>0 && before_pixels==after_pixels && ow==aw && oh==ah &&
        !std::memcmp(image_before.data(),image_after.data(),size_t(before_pixels)*sizeof(uint32_t));
    const bool written=rendered && psx_presentation_gpu_write_png(path,int(area.left),int(area.top),int(area.right-area.left+1),int(area.bottom-area.top+1));
    std::snprintf(reply,size_t(cap),"{\"rendered\":%s,\"written\":%s,\"triangles\":%u,\"skipped_commands\":%u,\"resource_only_copies\":%u,\"skipped_opcodes\":[%s],"
        "\"cpu_unchanged\":%s,\"ram_unchanged\":%s,\"vram_unchanged\":%s,\"native_image_unchanged\":%s,"
        "\"sampled_triangles\":%u,\"paired_vertices\":%u,\"derived_vertices\":%u,\"alpha\":%.6f,\"mode\":\"%s\"}",
        rendered?"true":"false",written?"true":"false",unsigned(triangles.size()),skipped,draws.resource_copies,skipped_ops,
        cpu_same?"true":"false",ram_same?"true":"false",vram_same?"true":"false",image_same?"true":"false",
        sampled_triangles,geometry?geometry->paired:0,geometry?geometry->derived:0,geometry?geometry->alpha:1,
        geometry?"geometry interpolation":"native geometry endpoint");
    return written && cpu_same && ram_same && vram_same && image_same;
}
// Diagnostic screen-space selection only. These observations never supply
// identities or correspondence to the sampler or the live renderer.
int probe_geometry(double alpha,double x,double y,unsigned rank,char* reply,int cap) {
    if(!pair_gpu_old || !pair_gpu_new)return 0;
    GeometrySamples samples;GeometryDraws draws;
    if(!geometry_samples(*pair_gpu_old,*pair_gpu_new,alpha,samples) ||
        !build_geometry_draws(*pair_gpu_new,&samples,draws)) {
        std::snprintf(reply,size_t(cap),"{\"error\":\"%s\"}",samples.error?samples.error:draws.error);return 0;
    }
    const auto& frame=*pair_gpu_new;
    x+=frame.environment.area.left;y+=frame.environment.area.top;
    std::vector<std::pair<double,size_t>> edges;
    for(size_t i=0;i<draws.triangles.size();++i) {
        double distance=1e30;const auto& t=draws.triangles[i];
        for(unsigned j=0;j<3;++j) {
            const auto& a=t.vertices[j];const auto& b=t.vertices[(j+1)%3];
            const double dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;
            const double u=length?std::max(0.0,std::min(1.0,((x-a.x)*dx+(y-a.y)*dy)/length)):0;
            distance=std::min(distance,std::hypot(x-a.x-u*dx,y-a.y-u*dy));
        }
        if(distance<=2)edges.emplace_back(distance,i);
    }
    std::sort(edges.begin(),edges.end());
    if(rank>=edges.size()) {
        std::snprintf(reply,size_t(cap),"{\"count\":%u,\"available\":false}",unsigned(edges.size()));return 1;
    }
    const auto& t=draws.triangles[edges[rank].second];const Packet* packet=nullptr;
    for(const auto& p:frame.packets)if(p.address==(t.identity&~uint64_t(3))) {packet=&p;break;}
    const auto& m=draws.materials[t.resource];
    const uint64_t zero[4]={};const auto* ids=packet?packet->source:zero;
    std::snprintf(reply,size_t(cap),"{\"available\":true,\"count\":%u,\"index\":%u,\"distance\":%.8f,\"packet\":%u,\"opcode\":%u,"
        "\"old\":%llu,\"new\":%llu,\"texpage\":%u,\"clut\":[%u,%u],\"xy\":[[%.8f,%.8f],[%.8f,%.8f],[%.8f,%.8f]],"
        "\"uv\":[[%.3f,%.3f],[%.3f,%.3f],[%.3f,%.3f]],\"sources\":[%llu,%llu,%llu,%llu]}",
        unsigned(edges.size()),unsigned(edges[rank].second),edges[rank].first,packet?packet->address:0,
        packet?frame.commands[packet->first]>>24:0,(unsigned long long)pair_gpu_old->serial,(unsigned long long)frame.serial,
        unsigned(m.texpage),unsigned(m.clut_x),unsigned(m.clut_y),
        t.vertices[0].x-frame.environment.area.left,t.vertices[0].y-frame.environment.area.top,
        t.vertices[1].x-frame.environment.area.left,t.vertices[1].y-frame.environment.area.top,
        t.vertices[2].x-frame.environment.area.left,t.vertices[2].y-frame.environment.area.top,
        t.vertices[0].uv[0],t.vertices[0].uv[1],t.vertices[1].uv[0],t.vertices[1].uv[1],t.vertices[2].uv[0],t.vertices[2].uv[1],
        (unsigned long long)ids[0],(unsigned long long)ids[1],(unsigned long long)ids[2],(unsigned long long)ids[3]);return 1;
}
int command(const char* args, char* reply, int cap) {
    if(args && (!std::strcmp(args,"capture on") || !std::strcmp(args,"capture off"))) {
        if(!std::strcmp(args,"capture on"))activate();else deactivate();
        std::snprintf(reply,size_t(cap),"{\"capture\":%s}",active?"true":"false");return 1;
    }
    if(args && !std::strncmp(args,"gpu ",4) && std::strcmp(args,"gpu live")) {
        if(!std::strcmp(args,"gpu off") || !std::strcmp(args,"gpu live off"))live_enabled=false;
        // Drain before locking: a queued GPU completion may publish a frame.
        psx_presentation_gpu_acquire();
    }
    std::lock_guard<std::mutex> lock(published_mutex);
    const Frame* f = published.get();
    if(args && !std::strncmp(args,"gpu probe ",10)) {
        double alpha,x,y;unsigned rank;
        if(std::sscanf(args+10,"%lf %lf %lf %u",&alpha,&x,&y,&rank)!=4 ||
            !std::isfinite(alpha) || alpha<0 || alpha>1 || !std::isfinite(x) || !std::isfinite(y) ||
            std::abs(x)>2048 || std::abs(y)>1024) {std::snprintf(reply,size_t(cap),"{}");return 0;}
        return probe_geometry(alpha,x,y,rank,reply,cap);
    }
    if(args && !std::strncmp(args,"gpu source ",11)) {
        char* end=nullptr;const uint64_t token=std::strtoull(args+11,&end,0);
        if(!end || *end || !pair_gpu_new) {std::snprintf(reply,size_t(cap),"{}");return 0;}
        for(const auto& b:pair_gpu_new->batches)if(b.source==(token&~uint64_t(3))) {
            std::snprintf(reply,size_t(cap),"{\"source\":%llu,\"kind\":%u,\"descriptor\":%u,\"owner\":%u,\"asset\":%u,"
                "\"lifetime\":%llu,\"vertex\":%u,\"count\":%u,\"source_vertex\":[%u,%u,%u],\"parents\":[%llu,%llu],"
                "\"input\":[%u,%u,%u,%u,%u,%u],\"matrix\":[%u,%u,%u,%u,%u,%u,%u,%u],\"root\":[%u,%u,%u,%u,%u,%u,%u,%u]}",
                (unsigned long long)b.source,unsigned(b.kind),b.descriptor,b.owner,b.asset,(unsigned long long)b.lifetime,
                b.vertex,b.vertex_count,b.source_vertex[0],b.source_vertex[1],b.source_vertex[2],
                (unsigned long long)b.parents[0],(unsigned long long)b.parents[1],
                b.input[0],b.input[1],b.input[2],b.input[3],b.input[4],b.input[5],
                b.matrix[0],b.matrix[1],b.matrix[2],b.matrix[3],b.matrix[4],b.matrix[5],b.matrix[6],b.matrix[7],
                b.root[0],b.root[1],b.root[2],b.root[3],b.root[4],b.root[5],b.root[6],b.root[7]);return 1;
        }
        std::snprintf(reply,size_t(cap),"{}");return 0;
    }
    if(args && !std::strcmp(args,"gpu live on")) {gpu_capture=true;live_enabled=true;prepare_live();std::snprintf(reply,size_t(cap),"{\"live\":true}");return 1;}
    if(args && !std::strcmp(args,"gpu live off")) {live_enabled=false;std::snprintf(reply,size_t(cap),"{\"live\":false}");return 1;}
    if(args && !std::strcmp(args,"gpu live")) {
        const auto frames=live_frames.load();
        const auto prepared=live_prepared.load();
        std::snprintf(reply,size_t(cap),"{\"live\":%s,\"generated\":%llu,\"prepared\":%llu,\"misses\":%llu,\"mean_cpu_ms\":%.3f,\"max_cpu_ms\":%.3f,\"failure\":\"%s\","
            "\"miss_reasons\":{\"busy\":%llu,\"pair\":%llu,\"binding\":%llu,\"scene\":%llu,\"draw\":%llu,\"gpu\":%llu}}",
            live_enabled?"true":"false",(unsigned long long)frames,(unsigned long long)prepared,(unsigned long long)live_misses.load(),
            prepared?double(live_cpu_ns.load())/prepared/1e6:0,double(live_max_ns.load())/1e6,live_failure.load(),
            (unsigned long long)live_reasons[0].load(),(unsigned long long)live_reasons[1].load(),(unsigned long long)live_reasons[2].load(),
            (unsigned long long)live_reasons[3].load(),(unsigned long long)live_reasons[4].load(),(unsigned long long)live_reasons[5].load());return 1;
    }
    if(args && !std::strcmp(args,"gpu on")) { gpu_capture=true;std::snprintf(reply,size_t(cap),"{\"gpu_capture\":true}");return 1; }
    if(args && !std::strcmp(args,"gpu off")) { live_enabled=false;gpu_capture=false;previous_gpu_frame->clear(0);std::snprintf(reply,size_t(cap),"{\"gpu_capture\":false}");return 1; }
    if(args && !std::strcmp(args,"gpu resource-test")) {
        const int ok=psx_presentation_gpu_test_versions();
        std::snprintf(reply,size_t(cap),"{\"retained_texture_test\":%s}",ok?"true":"false");return ok;
    }
    if(args && (!std::strcmp(args,"gpu pair") || !std::strcmp(args,"gpu pair-adjacent"))) {
        const Frame* earlier=!std::strcmp(args,"gpu pair-adjacent")?previous_gpu_frame.get():held_gpu_frame.get();
        if(!f || !f->texture_version || !earlier || earlier->serial>=f->serial) { std::snprintf(reply,size_t(cap),"{\"paired\":false}");return 0; }
        auto a=copy_gpu_frame(*earlier),b=copy_gpu_frame(*f);
        if(!a || !b) { std::snprintf(reply,size_t(cap),"{\"paired\":false}");return 0; }
        pair_gpu_old=std::move(a);pair_gpu_new=std::move(b);
        std::snprintf(reply,size_t(cap),"{\"paired\":true,\"old_submission\":%llu,\"new_submission\":%llu}",
            (unsigned long long)pair_gpu_old->serial,(unsigned long long)pair_gpu_new->serial);return 1;
    }
    if(args && !std::strncmp(args,"gpu interpolate ",16)) {
        char* end=nullptr;const double alpha=std::strtod(args+16,&end);
        if(!pair_gpu_old || !pair_gpu_new || !end || *end!=' ' || !std::isfinite(alpha) || alpha<0 || alpha>1) {
            std::snprintf(reply,size_t(cap),"{\"error\":\"need a retained pair, alpha in [0,1], and output path\"}");return 0;
        }
        GeometrySamples sampled;
        if(!geometry_samples(*pair_gpu_old,*pair_gpu_new,alpha,sampled)) {
            const auto& m=sampled.camera_linear;
            const auto& o=sampled.old_camera_linear;const auto& l=sampled.lens_linear;
            std::snprintf(reply,size_t(cap),"{\"error\":\"%s\",\"camera_linear\":[%d,%d,%d,%d,%d,%d,%d,%d,%d],"
                "\"old_camera_linear\":[%d,%d,%d,%d,%d,%d,%d,%d,%d],\"lens_linear\":[%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
                sampled.error,m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],
                o[0],o[1],o[2],o[3],o[4],o[5],o[6],o[7],o[8],l[0],l[1],l[2],l[3],l[4],l[5],l[6],l[7],l[8]);return 0;
        }
        return render_native_geometry(*pair_gpu_new,end+1,reply,cap,&sampled);
    }
    if(args && !std::strcmp(args,"gpu freeze")) {
        if(!f || !f->texture_version) { std::snprintf(reply,size_t(cap),"{\"frozen\":false}");return 0; }
        auto copy=std::make_unique<Frame>();*copy=*f;copy->texture_version=0;
        if(!psx_presentation_gpu_retain(f->texture_version)) { std::snprintf(reply,size_t(cap),"{\"frozen\":false}");return 0; }
        copy->texture_version=f->texture_version;held_gpu_frame=std::move(copy);
        std::snprintf(reply,size_t(cap),"{\"frozen\":true,\"submission\":%llu}",(unsigned long long)f->serial);return 1;
    }
    if(args && !std::strncmp(args,"gpu render-held ",16)) {
        if(!held_gpu_frame) { std::snprintf(reply,size_t(cap),"{\"error\":\"no retained frame\"}");return 0; }
        return render_native_geometry(*held_gpu_frame,args+16,reply,cap);
    }
    if(args && !std::strncmp(args,"gpu render ",11))return f?render_native_geometry(*f,args+11,reply,cap):0;
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
        "\"descriptor\":%u,\"owner\":%u,\"asset\":%u,\"bone\":%u,"
        "\"vertex\":%u,\"native_xy\":%u,\"local_xy\":%u,"
        "\"mapped_polygons\":%llu,\"partial_polygons\":%llu,\"unknown_polygons\":%llu,"
        "\"invalid_lists\":%llu,\"tag_failures\":%llu,\"last_packets\":%u,\"last_words\":%u,"
        "\"rigid_batches\":%llu,"
        "\"world_batches\":%llu,\"typed_batches\":%llu,"
        "\"terrain_vertices\":%llu,\"kind\":%u,\"vertex_count\":%u,"
        "\"subdivision_vertices\":%llu,"
        "\"constructors\":%llu,\"lifetime_batches\":%llu,\"lifetime\":\"0x%016llX\","
        "\"world_root_batches\":%llu,\"root_domain\":%u,"
        "\"camera_compositions\":%llu,\"rigid_world_batches\":%llu,"
        "\"root_x\":%d,\"world_root_x\":%d,"
        "\"gpu_snapshot\":%s,"
        "\"gpu_dropped\":%llu,"
        "\"lifetime_qualified\":false}",
        int(active.load()), (unsigned long long)submissions.load(),
        (unsigned long long)batches.load(), (unsigned long long)rejected.load(),
        (unsigned long long)overflows.load(), f ? (unsigned long long)f->serial : 0,
        f ? f->skeletons : 0, f ? unsigned(f->batches.size()) : 0,
        f ? f->rejected : 0, f ? f->overflow : 0,
        b ? b->descriptor : 0, b ? b->owner : 0, b ? b->asset : 0, b ? b->bone : 0, b ? b->vertex : 0,
        b ? b->native_xy[0] : 0, b ? b->local[0] : 0,
        (unsigned long long)mapped.load(), (unsigned long long)partial.load(),
        (unsigned long long)unknown.load(), (unsigned long long)invalid_lists.load(),
        (unsigned long long)tag_failures.load(), f ? unsigned(f->packets.size()) : 0,
        f ? unsigned(f->commands.size()) : 0, (unsigned long long)rigid_batches.load(),
        (unsigned long long)world_batches.load(), (unsigned long long)typed_batches.load(),
        (unsigned long long)terrain_vertices.load(), b ? b->kind : 0, b ? b->vertex_count : 0,
        (unsigned long long)subdivision_vertices.load(),
        (unsigned long long)constructions.load(), (unsigned long long)lifetime_batches.load(),
        b ? (unsigned long long)b->lifetime : 0,
        (unsigned long long)world_root_batches.load(), b ? b->root_domain : 0,
        (unsigned long long)composition_count.load(),(unsigned long long)rigid_world_batches.load(),
        b ? int32_t(b->root[5]) : 0, b ? int32_t(b->world_root[5]) : 0,
        f && f->texture_version?"true":"false",(unsigned long long)gpu_dropped.load());
    return 1;
}
}
PSX_MOD_CONSTRUCTOR(ape_register_presentation_capture) {
    psx_presentation_gpu_set_provider(live_present,nullptr);
    psx_mod_register_activation_plugin(Plugin, activate);
    psx_mod_register_session_reset_plugin(Plugin, deactivate);
    psx_mod_register_savestate_plugin(Plugin, reset);
    psx_mod_register_instruction_plugin(Plugin, 0x8002FF50u, 0x8C880000u, camera_composition_before);
    psx_mod_register_instruction_plugin(Plugin, 0x800300A4u, 0x00C01021u, camera_composition_after);
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
    psx_mod_register_instruction_plugin(Plugin, 0x800168C0u, 0x03E00008u, leave);
    psx_mod_register_instruction_plugin(Plugin, 0x80043E68u, 0x26040001u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x80043F18u, 0x26040001u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x80043FC8u, 0x26240250u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x800440C4u, 0x26220268u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x8004412Cu, 0x26220280u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x80044820u, 0x26040001u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x800448DCu, 0x26040001u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x80044998u, 0x26240250u, projected_terrain);
    psx_mod_register_instruction_plugin(Plugin, 0x8001DF70u, 0x48994800u, subdivided);
    psx_mod_register_instruction_plugin(Plugin, 0x8001E0D0u, 0x48994800u, subdivided);
    psx_mod_register_instruction_plugin(Plugin, 0x8001DB74u, 0x27BDFFC0u, subdivided);
    psx_mod_register_command("ape.presentation.capture", command);
}
#endif
