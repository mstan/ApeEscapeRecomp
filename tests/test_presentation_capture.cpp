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
void word(uint32_t address, uint32_t value) {
    address &= 0x1FFFFFFF;assert(address + 4 <= memory.size());
    for (unsigned i=0;i<4;++i) memory[address+i] = uint8_t(value>>(8*i));
}
void call(uint32_t address, CPUState& cpu) {
    CPUState before=cpu;
    const std::vector<unsigned char> old(memory.begin(),memory.end());
    hooks().at(address).fn(&cpu,address);
    assert(std::memcmp(&before,&cpu,sizeof cpu)==0);
    assert(std::equal(old.begin(),old.end(),memory.begin()));
}
std::string report(const char* args="") {
    char out[1024];assert(query(args,out,sizeof out));return out;
}
}
extern "C" int psx_mod_register_activation_plugin(const char*,PSXModActivationCallback fn) {activation=fn;return 1;}
extern "C" int psx_mod_register_session_reset_plugin(const char*,PSXModActivationCallback fn) {session_reset=fn;return 1;}
extern "C" int psx_mod_register_savestate_plugin(const char*,PSXModActivationCallback fn) {restore=fn;return 1;}
extern "C" int pgxp_set_source_tracking(int) {return 1;}
extern "C" int pgxp_tag_gte_source(uint32_t index,uint32_t,uint64_t token) {assert(index<3);gte_source[index]=token;return 1;}
extern "C" uint64_t pgxp_load_source(uint32_t address,uint32_t) {return sources[address];}
extern "C" void pgxp_invalidate_all() {sources.clear();}
extern "C" int psx_mod_register_instruction_plugin(const char*,uint32_t pc,uint32_t expected,PSXModFunctionEntryCallback fn) {
    assert(hooks().emplace(pc,Hook{expected,fn}).second);return 1;
}
extern "C" int psx_mod_register_command(const char*,PSXModCommandFn fn) {query=fn;return 1;}
extern "C" uint32_t psx_mod_read_word(uint32_t address) {
    address &= 0x1FFFFFFF;assert(address+4<=memory.size());
    return uint32_t(memory[address]) | uint32_t(memory[address+1])<<8 |
        uint32_t(memory[address+2])<<16 | uint32_t(memory[address+3])<<24;
}
extern "C" uint16_t psx_mod_read_half(uint32_t address) {
    address &= 0x1FFFFFFF;assert(address+2<=memory.size());
    return uint16_t(memory[address]) | uint16_t(memory[address+1])<<8;
}
int main() {
    assert(activation && restore && session_reset && query);
    assert(hooks().size()==7 && hooks().at(0x80016B44).word==0xE88C0000);
    assert(report().find("\"active\":0")!=std::string::npos);
    activation();CPUState cpu{};
    const uint32_t d=0x80001000, asset=0x80002000, v=0x80003000, m=0x80004000, spans=0x80005000;
    for (uint32_t descriptor : {d,d+32}) {
        word(descriptor,asset);word(descriptor+12,spans);word(descriptor+16,v);word(descriptor+20,m);
    }
    word(asset+4,1);word(spans,3); // one bone, three vertices, first index zero
    for(unsigned i=0;i<6;++i)word(v+4*i, i+1);
    for (uint32_t descriptor : {d,d+32}) {
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
    word(ot,0x07000000u | (ot+32));word(ot+32,0x07FFFFFFu);
    for(uint32_t p : {ot,ot+32}) {
        word(p+4,0x24000000u);
        for(unsigned i=0;i<3;++i)word(p+8+i*8,100+i);
    }
    for(unsigned i=0;i<3;++i)sources[ot+8+i*8]=gte_source[i];
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    const std::string first=report("0"),second=report("1");
    assert(first.find("\"batches\":2")!=std::string::npos);
    assert(first.find("\"rejected\":2")!=std::string::npos);
    assert(first.find("\"descriptor\":2147487744")!=std::string::npos);
    assert(second.find("\"descriptor\":2147487776")!=std::string::npos);
    assert(first.find("\"native_xy\":100")!=std::string::npos);
    assert(first.find("\"lifetime_qualified\":false")!=std::string::npos);
    assert(first.find("\"mapped_polygons\":1")!=std::string::npos);
    assert(first.find("\"unknown_polygons\":1")!=std::string::npos);
    assert(first.find("\"invalid_lists\":0")!=std::string::npos);
    // Rigid geometry uses a separate descriptor layout and source/output index.
    word(asset+4,3u<<16);word(d+12,v);
    cpu.gpr[4]=d;cpu.gpr[5]=m;cpu.gpr[6]=0x7000;
    call(0x800161C4,cpu);
    cpu.gpr[3]=v;cpu.gpr[7]=0;cpu.gpr[9]=3;
    call(0x80016260,cpu);call(0x8001628C,cpu);
    cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"batches\":3")!=std::string::npos);
    assert(report().find("\"rigid_batches\":1")!=std::string::npos);
    assert(report().find("\"rejected\":2")!=std::string::npos);
    // A cyclic list must be bounded and fail closed without changing the guest.
    word(ot,ot);cpu.gpr[4]=ot;call(0x80028CD0,cpu);
    assert(report().find("\"invalid_lists\":1")!=std::string::npos);
    restore();assert(report().find("\"last_batches\":0")!=std::string::npos);
    session_reset();assert(report().find("\"active\":0")!=std::string::npos);
    std::cout<<"Ape explicit projection capture: read-only, distinct instances, packet lineage, bounded lists, reset PASS\n";
}
