#include "cpu_state.h"
#include "mod_plugins.h"
#include "pgxp.h"
#include <array>
#include <algorithm>
#include <cassert>
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
// The runtime's existing GPU command decoder supplies lengths. This fixture
// exercises polygon commands preceded by a draw-mode word in the same node.
extern "C" int gpu_gp0_command_word_count(uint8_t op) {
    if(op>=0x20 && op<=0x3F) {
        const int n=(op&8)?4:3,stride=1+((op&16)?1:0)+((op&4)?1:0);
        return n*stride+((op&16)?0:1);
    }
    return op==0xE1?1:0;
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
    assert(hooks().size()==37 && hooks().at(0x80016B44).word==0xE88C0000);
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
        cpu.gpr[19]=descriptor;cpu.gpr[20]=m;
        call(0x800156C8,cpu);
        word(m+20,psx_mod_read_word(m+20)+123); // original camera composition overwrites root
        call(0x800156D0,cpu);
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
    assert(report().find("\"root_domain\":0")!=std::string::npos);
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
    sources[block]=gte_source[0];sources[block+8]=gte_source[1];
    cpu.gpr[17]=block;call(0x8001DF6C,cpu);
    const uint64_t midpoint=sources.at(block+72);
    assert(midpoint && midpoint!=sources[block] && midpoint!=sources[block+8]);
    // The next list must own the midpoint and its earlier source dependencies.
    sources[ot+12]=midpoint;sources[ot+20]=gte_source[0];sources[ot+28]=gte_source[1];
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"last_batches\":2")!=std::string::npos);
    assert(report().find("\"subdivision_vertices\":1")!=std::string::npos);
    sources.erase(block+8);sources.erase(block+72);
    call(0x8001DF6C,cpu);
    assert(sources.find(block+72)==sources.end()); // unknown parent cannot be guessed
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
    session_reset();assert(report().find("\"active\":0")!=std::string::npos);
    std::cout<<"Ape explicit projection capture: read-only, distinct instances, packet lineage, bounded lists, reset PASS\n";
}
