#include "gpu_gl_renderer.h"
#include "cpu_state.h"
#include "mod_plugins.h"
#include "pgxp.h"
#include "gpu.h"
#include "presentation_gpu.h"
#include "gte_presentation.h"
#include <array>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
struct Hook { uint32_t word; PSXModFunctionEntryCallback fn; };
std::map<uint32_t, Hook>& hooks() { static std::map<uint32_t, Hook> h; return h; }
PSXModActivationCallback activation, restore, session_reset;
PSXModCommandFn query;
uint64_t gte_source[3]{};
std::map<uint32_t,uint64_t> sources;
std::array<unsigned char, 0x200000> memory{};
std::array<unsigned char, 1024> scratch{};
unsigned char& byte(uint32_t address) {
    address &= 0x1FFFFFFF;
    if(address < memory.size())return memory[address];
    assert(address>=0x1F800000u && address<0x1F800400u);
    return scratch[address-0x1F800000u];
}
bool fake_gpu=false;
bool defer_gpu=false;
bool fake_geometry=false,fake_perspective=false;
struct NativeProjection {uint32_t word;int32_t x,y;uint16_t z;};
std::map<uint32_t,NativeProjection> native_projections;
struct DeferredGpu {PsxPresentationSnapshotReady ready;void* user;};
std::vector<DeferredGpu> deferred_gpu;
int fake_env_y=0;
PsxPresentationProvider provider=nullptr;
void* provider_user=nullptr;
unsigned gpu_refs=0;
uint64_t output_version=0;
double test_focal=100,test_center=100;
std::vector<PsxPresentationTriangle> rendered_triangles;
std::vector<PsxPresentationMaterial> rendered_materials;
void word(uint32_t address, uint32_t value) {
    for (unsigned i=0;i<4;++i) byte(address+i) = uint8_t(value>>(8*i));
}
void call(uint32_t address, CPUState& cpu) {
    CPUState before=cpu;
    const std::vector<unsigned char> old(memory.begin(),memory.end());
    const auto old_scratch=scratch;
    hooks().at(address).fn(&cpu,address);
    assert(std::memcmp(&before,&cpu,sizeof cpu)==0);
    assert(std::equal(old.begin(),old.end(),memory.begin()));
    assert(old_scratch==scratch);
}
std::string report(const char* args="") {
    char out[4096];assert(query(args,out,sizeof out));return out;
}
}
extern "C" int psx_mod_register_activation_plugin(const char*,PSXModActivationCallback fn) {activation=fn;return 1;}
extern "C" int psx_mod_register_session_reset_plugin(const char*,PSXModActivationCallback fn) {session_reset=fn;return 1;}
extern "C" int psx_mod_register_savestate_plugin(const char*,PSXModActivationCallback fn) {restore=fn;return 1;}
extern "C" int pgxp_set_source_tracking(int) {return 1;}
extern "C" int pgxp_tag_gte_source(uint32_t index,uint32_t,uint64_t token) {assert(index<3);gte_source[index]=token;return 1;}
extern "C" uint64_t pgxp_load_source(uint32_t address,uint32_t) {return sources[address];}
extern "C" int pgxp_tag_word_source(uint32_t address,uint32_t expected,uint64_t token) {
    assert(psx_mod_read_word(address)==expected);sources[address]=token;return 1;
}
extern "C" void pgxp_invalidate_all() {sources.clear();}
extern "C" void gpu_get_drawing_environment(GpuDrawingEnvironment* out) {*out={};out->area.right=383;out->area.top=fake_env_y;out->area.bottom=fake_env_y+239;out->area.offset_x=5;out->area.offset_y=fake_env_y+7;}
extern "C" void psx_presentation_gpu_set_provider(PsxPresentationProvider fn,void* user) {provider=fn;provider_user=user;}
extern "C" uint64_t psx_presentation_gpu_snapshot() {if(!fake_gpu)return 0;++gpu_refs;return 101;}
extern "C" void psx_presentation_gpu_acquire() {}
extern "C" int psx_presentation_gpu_snapshot_async(PsxPresentationSnapshotReady ready,void* user) {
    if(defer_gpu) {deferred_gpu.push_back({ready,user});return 1;}
    const uint64_t version=psx_presentation_gpu_snapshot();if(!version)return 0;ready(user,version);return 1;
}
extern "C" int psx_presentation_gpu_retain(uint64_t id) {if(!fake_gpu || id!=101)return 0;++gpu_refs;return 1;}
extern "C" void psx_presentation_gpu_release(uint64_t id) {assert(id==101 && gpu_refs);--gpu_refs;}
extern "C" int psx_presentation_gpu_render(const PsxPresentationTriangle* tri,uint32_t count,const PsxPresentationMaterial* mat,uint32_t materials) {
    rendered_triangles.assign(tri,tri+count);rendered_materials.assign(mat,mat+materials);++output_version;return fake_gpu;
}
extern "C" uint64_t psx_presentation_gpu_output_version() {return output_version;}
extern "C" int psx_presentation_gpu_output_ready(uint64_t version) {return fake_gpu && version==output_version;}
extern "C" int psx_presentation_gpu_write_png(const char*,int,int,int,int) {return fake_gpu;}
extern "C" int psx_presentation_gpu_test_versions() {return 0;}
extern "C" const uint16_t* gpu_get_vram() {static uint16_t vram[1024*512]{};return vram;}
extern "C" int gl_renderer_scale_info(GlScaleInfo* out) { *out={};out->effective=3;return fake_gpu; }
extern "C" int gl_renderer_read_display_hires(int,int,int w,int h,uint32_t* out,int capacity,int* width,int* height) {
    if(!fake_gpu)return 0;
    assert(capacity>=w*h*9);
    *width=*height=2;for(unsigned i=0;i<4;++i)out[i]=0x12345678;return 4;
}
extern "C" int gl_renderer_accurate_blending() {return 1;}
extern "C" int gr_texture_filter() {return 0;}
extern "C" int gpu_texture_correction_enabled() {return fake_perspective;}
extern "C" int gte_geometry_correction_enabled() {return fake_geometry;}
extern "C" int pgxp_load_precise_word(uint32_t address,uint32_t word,int32_t* x,int32_t* y,uint16_t* z) {
    const auto found=native_projections.find(address);
    if(found==native_projections.end() || found->second.word!=word)return 0;
    if(x)*x=found->second.x;
    if(y)*y=found->second.y;
    if(z)*z=found->second.z;
    return 1;
}
extern "C" int pgxp_get_precise_vertex(uint32_t address,uint32_t word,int32_t,int32_t,int32_t* x,int32_t* y,uint16_t* z) {
    return pgxp_load_precise_word(address,word,x,y,z)?PGXP_SRC_DATAFLOW:PGXP_SRC_NATIVE;
}
extern "C" void gpu_ws_prepass_linked_list(uint32_t) {}
extern "C" int gpu_ws_copy_presented_ui(uint32_t,const uint32_t*,uint32_t,int32_t[4],int32_t[4],unsigned,int) {return 0;}
extern "C" void gte_presentation_capture(int) {}
extern "C" int gte_presentation_projection(unsigned,uint32_t,uint32_t,GtePresentationProjection* p) {
    *p={test_focal,test_focal,test_center,test_center,test_focal*.5};return 1;
}
// The runtime's existing GPU command decoder supplies lengths. This fixture
// exercises polygon commands preceded by a draw-mode word in the same node.
extern "C" int gpu_gp0_command_word_count(uint8_t op) {
    if(op>=0x20 && op<=0x3F) {
        const int n=(op&8)?4:3,stride=1+((op&16)?1:0)+((op&4)?1:0);
        return n*stride+((op&16)?0:1);
    }
    return op==0x40?3:op==0x80?4:op==0xE1?1:0;
}
extern "C" int psx_mod_register_instruction_plugin(const char*,uint32_t pc,uint32_t expected,PSXModFunctionEntryCallback fn) {
    assert(hooks().emplace(pc,Hook{expected,fn}).second);return 1;
}
extern "C" int psx_mod_register_command(const char*,PSXModCommandFn fn) {query=fn;return 1;}
extern "C" uint32_t psx_mod_read_word(uint32_t address) {
    return uint32_t(byte(address)) | uint32_t(byte(address+1))<<8 |
        uint32_t(byte(address+2))<<16 | uint32_t(byte(address+3))<<24;
}
extern "C" uint16_t psx_mod_read_half(uint32_t address) {
    return uint16_t(byte(address)) | uint16_t(byte(address+1))<<8;
}
int main() {
    assert(activation && restore && session_reset && query);
    assert(hooks().size()==34 && hooks().at(0x80016B44).word==0xE88C0000);
    assert(report().find("\"active\":0")!=std::string::npos);
    activation();CPUState cpu{};
    const uint32_t d=0x80001000, asset=0x80002000, v=0x80003000, m=0x80004000, spans=0x80005000;
    for (uint32_t descriptor : {d,d+32}) {
        word(descriptor,asset);word(descriptor+12,spans);word(descriptor+16,v);word(descriptor+20,m);
        cpu.gpr[4]=descriptor;cpu.gpr[5]=asset;call(0x80015B34,cpu);
    }
    word(asset+4,1);word(spans,3); // one bone, three vertices, first index zero
    for(unsigned i=0;i<6;++i)word(v+4*i, i+1);
    for (uint32_t descriptor : {d,d+32}) {
        cpu.gpr[4]=0x800E78C0u;cpu.gpr[5]=m;cpu.gpr[6]=m;
        call(0x8002FF50,cpu);
        word(m+20,psx_mod_read_word(m+20)+123); // original camera composition overwrites root
        call(0x800300A4,cpu);
        cpu.gpr[4]=descriptor;call(0x80016A74,cpu);
        cpu.gpr[4]=0x800E7D00;cpu.gpr[3]=v;cpu.gpr[5]=0;
        cpu.gpr[11]=0;cpu.gpr[15]=32;cpu.gpr[16]=m;cpu.gpr[25]=v;
        for(unsigned i=0;i<6;++i)cpu.gte_data[i]=i+1;
        for(unsigned i=0;i<3;++i){cpu.gte_data[12+i]=100+i;cpu.gte_data[17+i]=200+i;}
        call(0x80016B44,cpu);
        // Identical coordinates from another address must not acquire provenance.
        cpu.gpr[3]=v+32;call(0x80016B44,cpu);
        call(0x80016B8C,cpu);
    }
    // Two identical submitted polygons, only one with actual copy lineage.
    const uint32_t ot = 0x6000;
    word(ot,0x08000000u | (ot+48));word(ot+4,0xE1000240u);
    word(ot+48,0x07FFFFFFu);
    for(uint32_t p : {ot+8,ot+52}) {
        word(p,0x24000000u);
        for(unsigned i=0;i<3;++i)word(p+4+i*8,100+i);
    }
    for(unsigned i=0;i<3;++i)sources[ot+12+i*8]=gte_source[i];
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    const std::string first=report("0"),second=report("1");
    assert(first.find("\"batches\":2")!=std::string::npos);
    assert(first.find("\"rejected\":2")!=std::string::npos);
    assert(first.find("\"descriptor\":2147487744")!=std::string::npos);
    assert(second.find("\"descriptor\":2147487776")!=std::string::npos);
    assert(first.find("\"lifetime\":\"0x8000000000000001\"")!=std::string::npos);
    assert(second.find("\"lifetime\":\"0x8000000000000002\"")!=std::string::npos);
    assert(first.find("\"root_domain\":1")!=std::string::npos);
    assert(second.find("\"root_domain\":1")!=std::string::npos);
    assert(first.find("\"world_root_batches\":2")!=std::string::npos);
    assert(first.find("\"root_x\":123,\"world_root_x\":0")!=std::string::npos);
    assert(second.find("\"root_x\":246,\"world_root_x\":123")!=std::string::npos);
    // Reconstructing the same descriptor/header starts a new lifetime; a DMA
    // overwrite invalidates its witness even when the resulting value agrees.
    cpu.gpr[4]=d;cpu.gpr[5]=asset;call(0x80015AC4,cpu);
    assert(sources[d]==(uint64_t(1)<<63)+3);
    sources.erase(d);word(d,asset);
    cpu.gpr[4]=d;call(0x80016A74,cpu);
    cpu.gpr[4]=0x800E7D00;cpu.gpr[3]=v;cpu.gpr[5]=0;
    cpu.gpr[11]=0;cpu.gpr[15]=32;cpu.gpr[16]=m;cpu.gpr[25]=v;
    call(0x80016B44,cpu);
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"lifetime\":\"0x0000000000000000\"")!=std::string::npos);
    // Transform provenance remains valid; the missing lifetime independently
    // prevents this reconstructed instance from interpolating.
    assert(report().find("\"root_domain\":1")!=std::string::npos);
    assert(first.find("\"native_xy\":100")!=std::string::npos);
    assert(first.find("\"lifetime_qualified\":false")!=std::string::npos);
    assert(first.find("\"mapped_polygons\":1")!=std::string::npos);
    assert(first.find("\"unknown_polygons\":1")!=std::string::npos);
    assert(first.find("\"invalid_lists\":0")!=std::string::npos);
    assert(first.find("\"last_packets\":3")!=std::string::npos);
    // Rigid geometry uses a separate descriptor layout and source/output index.
    word(asset+4,3u<<16);word(d+12,v);
    cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;
    call(0x800161C4,cpu);
    cpu.gpr[3]=v;cpu.gpr[7]=0;cpu.gpr[9]=3;
    call(0x80016260,cpu);call(0x8001628C,cpu);
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"batches\":4")!=std::string::npos);
    assert(report().find("\"rigid_batches\":1")!=std::string::npos);
    assert(report().find("\"mapped_polygons\":3")!=std::string::npos); // later lists still own the earlier projections
    assert(report().find("\"rejected\":2")!=std::string::npos);
    // World models have their vertex count at header+18, unlike rigid UI.
    word(asset+16,3u<<16);
    cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;call(0x800160F0,cpu);
    cpu.gpr[3]=v;cpu.gpr[7]=0;cpu.gpr[9]=3;call(0x8001618C,cpu);
    call(0x800161B8,cpu);
    // Authored quarter-turns plus world origin, with explicit expected inputs.
    const int expected[4][3][3]={
        {{101,-48,203},{104,-45,206},{107,-42,209}},
        {{103,-48,199},{106,-45,196},{109,-42,193}},
        {{99,-48,197},{96,-45,194},{93,-42,191}},
        {{97,-48,201},{94,-45,204},{91,-42,207}}};
    const uint32_t projected_pc[4]={0x8001680C,0x80016394,0x80016514,0x80016690};
    for(unsigned i=0;i<3;++i) {
        word(v+i*8,(1+i*3) | ((2+i*3)<<16));word(v+i*8+4,3+i*3);
    }
    word(0x1F8003C8u,100u | (uint32_t(uint16_t(-50))<<16));word(0x1F8003CCu,200);
    for(unsigned q=0;q<4;++q) {
        word(asset+16,(q ? 3u : 6u)<<16);
        word(d+28,q<<10);cpu.gpr[4]=d;cpu.gpr[5]=0x7000;call(0x8001629C,cpu);
        cpu.gpr[7]=v;cpu.gpr[9]=0;cpu.gpr[13]=q ? 3 : 6;
        if (!q) cpu.gpr[4]=v;
        for(unsigned i=0;i<3;++i) {
            cpu.gte_data[i*2]=uint16_t(expected[q][i][0]) | uint32_t(uint16_t(expected[q][i][1]))<<16;
            cpu.gte_data[i*2+1]=uint16_t(expected[q][i][2]);
        }
        call(projected_pc[q],cpu);
        if (!q) {
            // The zero-turn loop advances a0, while a3 becomes scratch data.
            // A fixture that tested only the first batch missed this case.
            cpu.gpr[4]=v+24;cpu.gpr[7]=0;cpu.gpr[9]=3;cpu.gpr[5]=0x7018;
            for(unsigned i=0;i<3;++i) {
                word(v+24+i*8,(10+i*3) | ((11+i*3)<<16));word(v+28+i*8,12+i*3);
                cpu.gte_data[i*2]=(110+i*3) | uint32_t(uint16_t(-39+int(i)*3))<<16;
                cpu.gte_data[i*2+1]=212+i*3;
            }
            call(projected_pc[0],cpu);
        }
    }
    cpu.gte_data[0]^=1;call(projected_pc[3],cpu); // altered input cannot keep lineage
    // Terrain sources use authored height-array indices, including the
    // secondary cell-centre array and the fourth corner's separate RTPS.
    word(0x1F8002A4u,4u | 4u<<16);word(0x800EBFB4u,0x9000);
    for(unsigned i=0;i<41;++i){byte(0x9000+i*2)=uint8_t(1000+i);byte(0x9001+i*2)=uint8_t((1000+i)>>8);}
    auto terrain_input=[&](unsigned i,int x,int y,int z) {
        const uint32_t xy=uint16_t(x) | uint32_t(uint16_t(y))<<16;
        word(0x1F800200u+i*8,xy);word(0x1F800204u+i*8,uint16_t(z));
        cpu.gte_data[i*2]=xy;cpu.gte_data[i*2+1]=uint16_t(z);
    };
    cpu.gpr[16]=0;
    terrain_input(0,0,1000,0);terrain_input(1,512,1001,0);terrain_input(2,1024,1002,0);
    call(0x80043E68,cpu);
    cpu.gpr[16]=25;
    terrain_input(0,256,1025,256);terrain_input(1,768,1026,256);terrain_input(2,256,1029,768);
    call(0x800440C4,cpu);
    word(0x1F800208u,768u | 1030u<<16);word(0x1F80020Cu,768);
    cpu.gte_data[0]=768u | 1030u<<16;cpu.gte_data[1]=768;
    const uint64_t old_first=gte_source[0],old_second=gte_source[1],old_third=gte_source[2];
    call(0x8004412C,cpu);
    assert(gte_source[0]==old_first && gte_source[1]==old_second && gte_source[2]>old_third);
    byte(0x9000+30*2)^=1;call(0x8004412C,cpu); // same coordinates cannot repair a wrong source index
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"world_batches\":1")!=std::string::npos);
    assert(report().find("\"typed_batches\":5")!=std::string::npos);
    assert(report().find("\"terrain_vertices\":7")!=std::string::npos);
    assert(report().find("\"rejected\":4")!=std::string::npos);
    const uint32_t block=0x1F800280u;
    word(block,100);word(block+8,101);word(block+72,100);word(block+76,200);
    word(block+4,200);word(block+12,201);
    sources[block]=gte_source[0];sources[block+8]=gte_source[1];
    cpu.gpr[17]=block;call(0x8001DF70,cpu);
    const uint64_t midpoint=sources.at(block+72);
    assert(midpoint && midpoint!=sources[block] && midpoint!=sources[block+8]);
    // The next list must own the midpoint and its earlier source dependencies.
    sources[ot+12]=midpoint;sources[ot+20]=gte_source[0];sources[ot+28]=gte_source[1];
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"last_batches\":2")!=std::string::npos);
    assert(report().find("\"subdivision_vertices\":1")!=std::string::npos);
    sources.erase(block+8);sources.erase(block+72);
    call(0x8001DF70,cpu);
    assert(sources.find(block+72)==sources.end()); // unknown parent cannot be guessed
    // The recursive child sees the fully written midpoint before its first
    // vertex load. Observing only the parent's return was too late.
    sources[block+8]=gte_source[0];sources[block+16]=gte_source[1];
    word(block+8,100);word(block+12,200);word(block+16,101);word(block+20,201);
    word(block+88,100);word(block+92,200);
    cpu.gpr[31]=0x8001E20Cu;cpu.gpr[6]=block+64;call(0x8001DB74,cpu);
    assert(sources.count(block+88) && sources[block+88]!=sources[block+8]);
    // The terrain caller creates the first midpoint before the recursive
    // routine. Its source token must exist before the child's first load too.
    const uint32_t seed=0x1F80033Cu;
    word(seed,100);word(seed+4,200);word(seed+24,101);word(seed+28,201);
    word(seed+8,100);word(seed+12,200);
    sources[seed]=gte_source[0];sources[seed+24]=gte_source[1];
    cpu.gpr[31]=0x80045BCCu;cpu.gpr[6]=seed;cpu.gpr[7]=1;cpu.gpr[14]=0x1F800000u;cpu.gpr[12]=seed;
    call(0x8001DB74u,cpu);
    const uint64_t seed_source=sources.at(seed+8);
    assert(seed_source && seed_source!=sources[seed] && seed_source!=sources[seed+24]);
    cpu.gpr[11]=seed;cpu.gpr[12]=200;call(0x8001DB74u,cpu);
    assert(sources.at(seed+8)!=seed_source); // the other triangle half has the same parent recipe
    const uint64_t other_seed=sources.at(seed+8);
    cpu.gpr[11]=500;call(0x8001DB74u,cpu);
    assert(sources.at(seed+8)==other_seed); // an ordinary quad cannot be reinterpreted as a split
    cpu.gpr[12]=seed;
    sources.erase(seed+8);cpu.gpr[7]=2;call(0x8001DB74u,cpu);
    assert(!sources.count(seed+8)); // a different call shape cannot borrow this recipe
    cpu.gpr[7]=1;sources.erase(seed+24);call(0x8001DB74u,cpu);
    assert(!sources.count(seed+8)); // missing lineage is never reconstructed by value
    cpu.gpr[31]=0;
    for(uint32_t pc : {0x80015BA8u,0x80015C3Cu,0x80015D14u}) {
        const uint32_t descriptor=d+64;
        word(descriptor,asset);cpu.gpr[5]=asset;
        const bool a2=pc!=0x80015D14u;
        cpu.gpr[4]=a2 ? 0 : descriptor;cpu.gpr[6]=a2 ? descriptor : 0;
        call(pc,cpu);
        assert(sources[descriptor]>>(63));
    }
    // A DrawOTag submission and a nested projection on another original guest
    // stack must not erase the suspended outer projector's descriptor.
    word(asset+4,3u<<16);word(d+12,v);word(d+32+12,v);
    cpu.gpr[29]=0x100;cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;
    call(0x800161C4,cpu);
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    cpu.gpr[29]=0x200;cpu.gpr[31]=0x1234;cpu.gpr[4]=d+32;cpu.gpr[5]=m;cpu.gpr[6]=0x7100;
    call(0x800161C4,cpu);
    cpu.gpr[3]=v;cpu.gpr[7]=0;cpu.gpr[9]=3;
    for(unsigned i=0;i<6;++i)cpu.gte_data[i]=psx_mod_read_word(v+i*4);
    call(0x80016260,cpu);call(0x8001628C,cpu);
    cpu.gpr[29]=0x100;cpu.gpr[31]=0;cpu.gpr[6]=0x7000;
    call(0x80016260,cpu);call(0x8001628C,cpu);
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"rejected\":4")!=std::string::npos);
    assert(report().find("\"rigid_batches\":3")!=std::string::npos);
    // A cyclic list must be bounded and fail closed without changing the guest.
    word(ot,ot);cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"invalid_lists\":1")!=std::string::npos);
    restore();assert(report().find("\"last_batches\":0")!=std::string::npos);
    // Native material decoding is independent of geometry correspondence.
    // Verify one semi-transparent textured quad, its environment/CLUT and
    // owned command bytes, then keep it while the original RAM changes.
    fake_gpu=true;report("gpu on");
    word(ot,0x0EFFFFFFu);word(ot+4,0xE1000200u);word(ot+8,0x2E808080u);
    const uint32_t xy[4]={10u|(20u<<16),30u|(20u<<16),10u|(40u<<16),30u|(40u<<16)};
    const uint32_t uv[4]={4u|(8u<<8)|(0x1C04u<<16),24u|(8u<<8)|(0x0140u<<16),4u|(28u<<8),24u|(28u<<8)};
    for(unsigned i=0;i<4;++i) {word(ot+12+i*8,xy[i]);word(ot+16+i*8,uv[i]);}
    word(ot+44,0x80000000u);word(ot+48,608u|(128u<<16));
    word(ot+52,624u|(128u<<16));word(ot+56,16u|(16u<<16));
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);assert(gpu_refs==1);
    const auto native_quad=report("gpu render unused.png");
    assert(native_quad.find("\"native_image_unchanged\":true")!=std::string::npos);
    assert(native_quad.find("\"resource_only_copies\":1")!=std::string::npos);
    assert(rendered_triangles.size()==2 && rendered_materials.size()==1);
    assert(rendered_triangles[0].vertices[0].x==15 && rendered_triangles[0].vertices[0].y==27);
    const auto material=rendered_materials[0];
    assert(material.texpage==0x140 && material.clut_x==64 && material.clut_y==112 && material.semi==2);
    assert(material.texture_version==101 && material.uv_limits[0]==4 && material.uv_limits[3]==28);
    assert((material.flags&(PSX_PRESENTATION_TEXTURED|PSX_PRESENTATION_DITHER))==(PSX_PRESENTATION_TEXTURED|PSX_PRESENTATION_DITHER));
    // Native geometry and texture correction are owned with the packet,
    // independently of whether any vertex qualifies for temporal sampling.
    fake_geometry=fake_perspective=true;
    for(unsigned j=0;j<4;++j) {
        const uint32_t address=ot+12+j*8,xy=psx_mod_read_word(address);
        native_projections[address]={xy,int32_t(int16_t(xy))*65536+16384,
            int32_t(int16_t(xy>>16))*65536+32768,uint16_t(100*(j+1))};
    }
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);report("gpu render unused.png");
    assert(rendered_triangles[0].vertices[0].x==15.25 && rendered_triangles[0].vertices[0].y==27.5);
    assert(!(rendered_triangles[0].flags&PSX_PRESENTATION_AFFINE_UV));
    assert(std::abs(rendered_triangles[0].vertices[0].reciprocal_z-.01)<1e-9);
    fake_geometry=fake_perspective=false;native_projections.clear();
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    report("gpu freeze");assert(gpu_refs==3); // current, adjacent, explicitly held
    word(ot+12,0);report("gpu render-held unused.png");
    assert(rendered_triangles[0].vertices[0].x==15 && rendered_triangles[0].vertices[0].y==27);
    // An offscreen write that a later material reads cannot use the resource
    // version captured before the write. Reject the whole private image.
    const uint32_t copy_head=ot+128;
    word(copy_head,0x04000000u|ot);word(copy_head+4,0x80000000u);
    word(copy_head+8,608u|(8u<<16));word(copy_head+12,452u|(8u<<16));word(copy_head+16,16u|(16u<<16));
    word(ot+24,(uv[1]&65535u)|(0x147u<<16));
    // An atlas update outside this primitive's authored UV footprint is safe
    // even when the texpage base is shared.
    word(copy_head+12,624u|(128u<<16));
    cpu.gpr[4]=copy_head;call(0x80028CD0,cpu);
    assert(report("gpu render unused.png").find("\"resource_only_copies\":2")!=std::string::npos);
    word(copy_head+12,452u|(8u<<16));
    cpu.gpr[4]=copy_head;call(0x80028CD0,cpu);
    char invalid_feedback[512];
    assert(!query("gpu render unused.png",invalid_feedback,sizeof invalid_feedback));
    assert(std::strstr(invalid_feedback,"in-frame texture feedback"));
    // An unsupported visible primitive rejects the complete private image;
    // it must never disappear while the supported polygons interpolate.
    const uint32_t line=ot+96;
    word(line,0x03000000u|ot);word(line+4,0x4000FF00u);
    word(line+8,10u|(20u<<16));word(line+12,30u|(40u<<16));
    cpu.gpr[4]=line;call(0x80028CD0,cpu);
    assert(!query("gpu render unused.png",invalid_feedback,sizeof invalid_feedback));
    assert(std::strstr(invalid_feedback,"whole-image fallback"));
    // Original camera composition includes an authored 3040/4096 Y lens.
    // Pair explicit source instances and verify world motion at the halfway
    // phase with that lens, rather than accepting it as camera rotation.
    const uint32_t lens[8]={4096,0,3040,0,4096,0,0,0};
    for(unsigned i=0;i<8;++i){word(0x800AF0A0u+i*4,lens[i]);word(0x800E78C0u+i*4,lens[i]);}
    word(asset+4,1);word(spans,3);word(d,asset);word(d+12,spans);word(d+16,v);word(d+20,m);
    cpu.gpr[4]=d;cpu.gpr[5]=asset;call(0x80015B34,cpu);
    const int16_t points[3][3]={{-30,-20,0},{30,-20,0},{0,20,0}};
    for(unsigned frame=0;frame<2;++frame) {
        uint32_t root[8]={4096,0,4096,0,4096,frame*20,0,500};
        for(unsigned i=0;i<8;++i)word(m+i*4,root[i]);
        cpu.gpr[4]=0x800E78C0u;cpu.gpr[5]=m;cpu.gpr[6]=m;call(0x8002FF50,cpu);
        root[2]=3040;
        for(unsigned i=0;i<8;++i){word(m+i*4,root[i]);cpu.gte_ctrl[i]=root[i];}
        call(0x800300A4,cpu);
        cpu.gte_ctrl[24]=100u<<16;cpu.gte_ctrl[25]=100u<<16;cpu.gte_ctrl[26]=100;
        cpu.gpr[4]=d;call(0x80016A74,cpu);
        cpu.gpr[4]=0x800E7D00;cpu.gpr[3]=v;cpu.gpr[5]=0;
        cpu.gpr[11]=0;cpu.gpr[15]=32;cpu.gpr[16]=m;cpu.gpr[25]=v;
        for(unsigned i=0;i<3;++i) {
            cpu.gte_data[i*2]=uint16_t(points[i][0]) | uint32_t(uint16_t(points[i][1]))<<16;
            cpu.gte_data[i*2+1]=uint16_t(points[i][2]);
            word(v+i*8,cpu.gte_data[i*2]);word(v+i*8+4,cpu.gte_data[i*2+1]);
            cpu.gte_data[12+i]=xy[i];cpu.gte_data[17+i]=500;
        }
        if(frame) {
            // A projected but unsubmitted camera domain must not override
            // the actual packet's domain, even when observed first.
            cpu.gte_ctrl[26]=90;call(0x80016B44,cpu);cpu.gte_ctrl[26]=100;
        }
        call(0x80016B44,cpu);call(0x80016B8C,cpu);
        word(ot,0x04FFFFFFu);word(ot+4,0x20808080u);
        for(unsigned i=0;i<3;++i){word(ot+8+i*4,xy[i]);sources[ot+8+i*4]=gte_source[i];}
        cpu.gpr[4]=ot;call(0x80028CD0,cpu);
        if(!frame)report("gpu freeze");
    }
    report("gpu pair");
    const auto middle=report("gpu interpolate 0.5 unused.png");
    assert(middle.find("\"paired_vertices\":3")!=std::string::npos);
    assert(rendered_triangles.size()==1);
    assert(std::abs(rendered_triangles[0].vertices[0].x-101)<1e-5);
    assert(std::abs(rendered_triangles[0].vertices[0].y-104.03125)<1e-5);
    // Rigid world pieces use the original general camera composer, rather
    // than the skeleton-only call site. Keep its exact write-invalidated
    // output witness; identical matrix bytes alone cannot qualify a binding.
    restore();word(asset+4,3u<<16);word(d,asset);word(d+12,v);
    word(0x800F447Cu,3);
    cpu.gpr[4]=d;cpu.gpr[5]=asset;call(0x80015B34,cpu);
    for(unsigned frame=0;frame<2;++frame) {
        fake_env_y=int(frame)*256;
        uint32_t root[8]={4096,0,4096,0,4096,frame*20,0,500};
        for(unsigned i=0;i<8;++i)word(m+i*4,root[i]);
        cpu.gpr[4]=0x800E78C0u;cpu.gpr[5]=m;cpu.gpr[6]=m;
        call(0x8002FF50,cpu);
        root[2]=3040;
        for(unsigned i=0;i<8;++i){word(m+i*4,root[i]);cpu.gte_ctrl[i]=root[i];}
        call(0x800300A4,cpu);
        assert(sources[m]>>62);
        cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;call(0x800161C4,cpu);
        cpu.gpr[3]=v;cpu.gpr[7]=0;cpu.gpr[9]=3;
        call(0x80016260,cpu);call(0x8001628C,cpu);
        word(block,xy[0]);word(block+4,500);word(block+8,xy[1]);word(block+12,500);
        word(block+72,20u|(20u<<16));word(block+76,500);
        sources[block]=gte_source[0];sources[block+8]=gte_source[1];
        cpu.gpr[17]=block;call(0x8001DF70,cpu);
        for(unsigned i=0;i<3;++i)sources[ot+8+i*4]=gte_source[i];
        sources[ot+8]=sources.at(block+72);
        cpu.gpr[4]=ot;call(0x80028CD0,cpu);
        assert(report().find("\"root_domain\":1")!=std::string::npos);
        if(!frame)report("gpu freeze");
    }
    report("gpu pair-adjacent");
    assert(report("gpu interpolate 0.5 unused.png").find("\"paired_vertices\":3")!=std::string::npos);
    assert(rendered_triangles.size()==1 && std::abs(rendered_triangles[0].vertices[0].x-107)<1e-5);
    report("gpu live on");int output[4]={};
    assert(provider && !provider(provider_user,0,0,384,240,0,output));
    const std::vector<unsigned char> guest_before(memory.begin(),memory.end());const CPUState cpu_before=cpu;
    assert(provider(provider_user,0,0,384,240,1,output));
    assert(output[0]==0 && output[1]==256 && output[2]==384 && output[3]==240);
    assert(std::equal(guest_before.begin(),guest_before.end(),memory.begin()) && !std::memcmp(&cpu,&cpu_before,sizeof cpu));
    assert(report("gpu live").find("\"generated\":1")!=std::string::npos);
    const auto diagnostic=report("gpu probe 0.5 107 104 0");
    assert(diagnostic.find("\"available\":true")!=std::string::npos);
    const auto source_diagnostic=report(("gpu source "+std::to_string(gte_source[0])).c_str());
    assert(source_diagnostic.find("\"descriptor\":"+std::to_string(d))!=std::string::npos);
    assert(std::equal(guest_before.begin(),guest_before.end(),memory.begin()) && !std::memcmp(&cpu,&cpu_before,sizeof cpu));
    report("gpu live off");
    assert(!provider(provider_user,0,0,384,240,1,output));
    // Actual camera basis that was rejected when row-normalization error
    // was mistaken for column-scale shear during a camera turn.
    const int16_t turned[10]={2933,0,2866,651,2887,-668,-2722,1256,2785,0};
    uint32_t view[8]={},world[8]={4096,0,4096,0,4096,40,0,500};
    for(unsigned i=0;i<5;++i)view[i]=uint16_t(turned[i*2]) | uint32_t(uint16_t(turned[i*2+1]))<<16;
    for(unsigned i=0;i<8;++i){word(0x800E78C0u+i*4,view[i]);word(m+i*4,world[i]);}
    cpu.gpr[4]=0x800E78C0u;cpu.gpr[5]=m;cpu.gpr[6]=m;call(0x8002FF50,cpu);
    for(unsigned row=0;row<3;++row) {
        double sum=0;for(unsigned col=0;col<3;++col)sum+=double(turned[row*3+col])*int32_t(world[5+col]);
        view[5+row]=uint32_t(int32_t(std::floor(sum/4096)));
    }
    for(unsigned i=0;i<8;++i){word(m+i*4,view[i]);cpu.gte_ctrl[i]=view[i];}
    call(0x800300A4,cpu);
    cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;call(0x800161C4,cpu);
    cpu.gpr[3]=v;cpu.gpr[7]=0;cpu.gpr[9]=3;call(0x80016260,cpu);call(0x8001628C,cpu);
    for(unsigned i=0;i<3;++i)sources[ot+8+i*4]=gte_source[i];
    fake_env_y=0;cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    report("gpu pair-adjacent");
    assert(report("gpu interpolate 0.5 unused.png").find("\"paired_vertices\":3")!=std::string::npos);
    sources.erase(m); // native/untracked same-value write invalidates the witness
    cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;call(0x800161C4,cpu);
    cpu.gpr[3]=v;cpu.gpr[7]=0;cpu.gpr[9]=3;
    call(0x80016260,cpu);call(0x8001628C,cpu);
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"root_domain\":0")!=std::string::npos);
    // Queue ownership survives later writes to the original command buffer.
    defer_gpu=true;cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(deferred_gpu.size()==1);
    const uint32_t original_xy=psx_mod_read_word(ot+8);word(ot+8,0);
    auto pending=deferred_gpu.back();deferred_gpu.clear();
    pending.ready(pending.user,psx_presentation_gpu_snapshot());
    report("gpu render unused.png");
    assert(rendered_triangles[0].vertices[0].x==int(original_xy&2047)+5);
    // A late completion from before restore must release its texture and
    // never resurrect a pre-restore scene in the new epoch.
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);assert(deferred_gpu.size()==1);
    restore();assert(!gpu_refs);
    pending=deferred_gpu.back();deferred_gpu.clear();
    pending.ready(pending.user,psx_presentation_gpu_snapshot());
    assert(!gpu_refs && report().find("\"gpu_snapshot\":false")!=std::string::npos);
    defer_gpu=false;
    // Restore observes existing live descriptors without waiting for the game
    // to reconstruct them. Continuity starts at this new epoch's first read.
    cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;call(0x800161C4,cpu);
    const uint64_t restored_lifetime=sources[d];assert(restored_lifetime>>63);
    call(0x800161C4,cpu);assert(sources[d]==restored_lifetime);
    // A same-value untracked overwrite within the epoch must still fail closed.
    sources.erase(d);word(d,asset);call(0x800161C4,cpu);assert(!sources[d]);
    // Native look-at normalization can produce a non-unit camera matrix.
    // Bind its authored input at the existing lens-composer seam, keep a
    // proper rotation, and preserve native projection at the exact endpoint.
    {
        activation();cpu={};fake_gpu=true;report("gpu on");
        word(asset+4,3u<<16);word(d,asset);word(d+12,v);
        cpu.gpr[4]=d;cpu.gpr[5]=asset;call(0x80015B34u,cpu);
        const uint32_t stack=0x80008000u,eye=0x80009000u;
        word(eye,100);word(eye+4,uint16_t(-500));
        word(stack+16,uint32_t(-1124));word(stack+20,870);word(stack+24,3867);
        for(unsigned i=0;i<8;++i)word(0x800AF0A0u+i*4,lens[i]);
        cpu.gpr[29]=stack;cpu.gpr[31]=0x80046888u;
        cpu.gpr[4]=0x800AF0A0u;cpu.gpr[5]=cpu.gpr[6]=cpu.gpr[19]=0x800E78C0u;
        cpu.gpr[21]=eye;cpu.gpr[22]=0;call(0x8002FF50u,cpu);
        const int16_t approximate[10]={3953,0,1148,180,2973,-624,-1124,870,3867,0};
        uint32_t cam_words[8]={};
        for(unsigned i=0;i<5;++i)cam_words[i]=uint16_t(approximate[i*2])|uint32_t(uint16_t(approximate[i*2+1]))<<16;
        const double eye_position[3]={100,0,-500};
        for(unsigned row=0;row<3;++row) {
            double t=0;for(unsigned col=0;col<3;++col)t-=approximate[row*3+col]*eye_position[col]/4096.;
            cam_words[5+row]=uint32_t(int32_t(std::floor(t)));
        }
        for(unsigned i=0;i<8;++i)word(0x800E78C0u+i*4,cam_words[i]);
        call(0x800300A4u,cpu);assert(sources[0x800E78C0u]&(uint64_t(1)<<61));
        double endpoint[2]={};
        for(unsigned frame=0;frame<2;++frame) {
            fake_env_y=int(frame)*256;
            uint32_t root[8]={4096,0,4096,0,4096,frame*20,0,500};
            for(unsigned i=0;i<8;++i)word(m+i*4,root[i]);
            cpu.gpr[31]=0;cpu.gpr[4]=0x800E78C0u;cpu.gpr[5]=cpu.gpr[6]=m;call(0x8002FF50u,cpu);
            uint32_t combined[8];std::copy(cam_words,cam_words+8,combined);
            for(unsigned row=0;row<3;++row) {
                double t=int32_t(cam_words[5+row]);
                for(unsigned col=0;col<3;++col)t+=approximate[row*3+col]*int32_t(root[5+col])/4096.;
                combined[5+row]=uint32_t(int32_t(std::floor(t)));
            }
            for(unsigned i=0;i<8;++i){word(m+i*4,combined[i]);cpu.gte_ctrl[i]=combined[i];}
            call(0x800300A4u,cpu);
            for(unsigned i=0;i<3;++i) {
                cpu.gte_data[i*2]=uint16_t(points[i][0])|uint32_t(uint16_t(points[i][1]))<<16;
                cpu.gte_data[i*2+1]=0;word(v+i*8,cpu.gte_data[i*2]);word(v+i*8+4,0);
                double projected[3];
                for(unsigned row=0;row<3;++row){projected[row]=int32_t(combined[5+row]);
                    for(unsigned col=0;col<3;++col)projected[row]+=approximate[row*3+col]*points[i][col]/4096.;}
                const double sx=100+100*projected[0]/projected[2],sy=100+100*projected[1]/projected[2];
                cpu.gte_data[12+i]=uint16_t(int(sx))|uint32_t(uint16_t(int(sy)))<<16;
                cpu.gte_data[17+i]=uint32_t(projected[2]);
                if(!i){endpoint[0]=sx+5;endpoint[1]=sy+7+fake_env_y;}
            }
            cpu.gte_ctrl[24]=cpu.gte_ctrl[25]=100u<<16;cpu.gte_ctrl[26]=100;
            cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;call(0x800161C4u,cpu);
            cpu.gpr[3]=v;cpu.gpr[7]=0;cpu.gpr[9]=3;call(0x80016260u,cpu);call(0x8001628Cu,cpu);
            word(ot,0x04FFFFFFu);word(ot+4,0x20808080u);
            for(unsigned i=0;i<3;++i){word(ot+8+i*4,cpu.gte_data[12+i]);sources[ot+8+i*4]=gte_source[i];}
            cpu.gpr[4]=ot;call(0x80028CD0u,cpu);
        }
        report("gpu pair-adjacent");report("gpu interpolate 1 unused.png");
        assert(rendered_triangles.size()==1);
        assert(std::abs(rendered_triangles[0].vertices[0].x-endpoint[0])<1e-8);
        assert(std::abs(rendered_triangles[0].vertices[0].y-endpoint[1])<1e-8);
        report("gpu interpolate 0.5 unused.png");
        assert(report(("gpu source "+std::to_string(gte_source[0])).c_str()).find("\"camera_authored\":true")!=std::string::npos);
        const uint64_t camera_witness=sources.at(0x800E78C0u);
        for(unsigned i=0;i<8;++i)assert(sources.at(0x800E78C0u+i*4)==camera_witness);
        // A same-value write to any camera word cuts its constructor binding.
        sources.erase(0x800E78C4u);word(0x800E78C4u,cam_words[1]);
        cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;call(0x800161C4u,cpu);
        cpu.gpr[3]=v;cpu.gpr[7]=0;cpu.gpr[9]=3;call(0x80016260u,cpu);call(0x8001628Cu,cpu);
        for(unsigned i=0;i<3;++i)sources[ot+8+i*4]=gte_source[i];
        cpu.gpr[4]=ot;call(0x80028CD0u,cpu);report("gpu pair-adjacent");
        assert(report(("gpu source "+std::to_string(gte_source[0])).c_str()).find("\"camera_authored\":false")!=std::string::npos);
        char rejected_camera[1024];assert(!query("gpu interpolate 0.5 unused.png",rejected_camera,sizeof rejected_camera));
    }
    // Mailboxes share one template, but the original caller owns separate
    // placement records. Identical local vertices must retain distinct lives.
    activation();cpu={};
    const uint32_t mailbox=0x800BBC54u,placements=0x800BBD34u,stack=0x80008000u;
    word(mailbox,asset);word(mailbox+12,spans);word(mailbox+16,v);word(mailbox+20,m);
    word(asset+4,1);word(spans,3);word(0x800BBC50u,2);
    for(unsigned i=0;i<6;++i)word(v+4*i,i+1);
    uint64_t mailbox_lives[2]{};
    for(unsigned object=0;object<2;++object) {
        const uint32_t owner=placements+object*24;
        word(owner,0x08000000u);word(stack+4,owner);
        cpu.gpr[29]=stack;cpu.gpr[31]=0x80071C28u;cpu.gpr[4]=mailbox;
        call(0x80016A74u,cpu);mailbox_lives[object]=sources[owner];
        assert(mailbox_lives[object]>>63);
        cpu.gpr[4]=0x800E7D00u;cpu.gpr[3]=v;cpu.gpr[5]=0;
        cpu.gpr[11]=0;cpu.gpr[15]=32;cpu.gpr[16]=m;cpu.gpr[25]=v;
        for(unsigned i=0;i<6;++i)cpu.gte_data[i]=i+1;
        for(unsigned i=0;i<3;++i){cpu.gte_data[12+i]=100+i;cpu.gte_data[17+i]=200+i;}
        call(0x80016B44u,cpu);call(0x80016B8Cu,cpu);
    }
    assert(mailbox_lives[0]!=mailbox_lives[1]);
    cpu.gpr[4]=ot;call(0x80028CD0u,cpu);
    assert(report("0").find("\"owner\":"+std::to_string(placements))!=std::string::npos);
    assert(report("1").find("\"owner\":"+std::to_string(placements+24))!=std::string::npos);
    assert(report("1").find("\"last_batches\":2")!=std::string::npos);
    // A same-value untracked owner write cuts continuity independently of
    // the shared template. A malformed record cannot become an instance.
    sources.erase(placements);word(placements,0x08000000u);word(stack+4,placements);
    cpu.gpr[4]=mailbox;call(0x80016A74u,cpu);assert(!sources[placements]);
    word(stack+4,placements+1);call(0x80016A74u,cpu);
    cpu.gpr[4]=ot;call(0x80028CD0u,cpu);
    assert(report().find("\"last_rejected\":1")!=std::string::npos);
    session_reset();assert(report().find("\"active\":0")!=std::string::npos);
    assert(!gpu_refs);
    std::cout<<"Ape explicit projection capture: read-only, distinct instances, packet lineage, bounded lists, reset PASS\n";
}
