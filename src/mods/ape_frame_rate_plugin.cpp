#include "render_pass_replay.hpp"

/* Main-loop capture follows the buffer reset (5DF48); replay stops before
 * VSync(1). The loop interleaves effect/pose updates and drawing, so restoring
 * the section's original RAM is essential. 5DEE0 waits for the preceding
 * VBlank submission, then arms the next one. Extra passes run outside IRQ. */
namespace {
constexpr uint32_t Start=0x8005C078, Stop=0x8005C294, Submit=0x8005DEE0;
constexpr uint32_t Base=0x800EC360, Stride=0x4078;
constexpr uint32_t PutDrawEnv=0x80028D30, DrawOTag=0x80028CC0, DrawSync=0x800286CC;
PSXDrawReplay replay;
uint32_t buffer=0, ot=0, vblanks=0;
void tick(){if(!g_psx_render_pass_active)++vblanks;}
void begin(CPUState* cpu,uint32_t) {
    if(g_psx_render_pass_active)return;
    const uint8_t mode=psx_mod_read_byte(0x800F447C);
    const uint32_t index=psx_mod_read_word(0x800EC35C);
    if(index>1 || mode==3 || mode==4 || mode==5 || mode==6) { replay.invalidate(); return; }
    buffer=Base+Stride*index; ot=buffer+0x70+0x3FFC;
    replay.capture(cpu,Start,Stop);
}
int pass(CPUState* cpu,void*,uint32_t alpha) {
    if(!replay.restore(cpu,alpha))return 0;
    const uint32_t other=buffer==Base ? Base+Stride : Base;
    for(uint32_t i=0;i<0x5C;i+=4)psx_mod_write_word(buffer+i,psx_mod_read_word(other+i));
    if(!replay.draw(cpu)){psx_mod_counter_add("ape.fr.draw_failed",1);return 0;}
    PSXDrawReplay::call(cpu,PutDrawEnv,buffer);
    PSXDrawReplay::call(cpu,DrawOTag,ot);
    PSXDrawReplay::call(cpu,DrawSync,0);
    psx_mod_counter_add("ape.fr.interpolated_projections",replay.stats.replayed);
    return 1;
}
void finish(CPUState* cpu,uint32_t) {
    if(g_psx_render_pass_active || cpu->gpr[31]!=0x8005C4A4)return;
    if(cpu->gpr[4]!=buffer || cpu->gpr[5]!=buffer+0x5C || cpu->gpr[6]!=ot){replay.invalidate();return;}
    // This is the game's own wait, which the original function will also
    // execute. Calling it here establishes the pending frame before planning.
    PSXDrawReplay::call(cpu,0x8005DE8C);
    const bool ready=replay.prepare(vblanks);
    psx_mod_counter_add("ape.fr.frames",1);
    psx_mod_counter_add("ape.fr.projections",replay.stats.captured);
    psx_mod_counter_add("ape.fr.matched",replay.stats.matched);
    psx_mod_counter_add("ape.fr.moving",replay.stats.changed);
    if(!ready)return;
    const uint32_t disp=buffer+0x5C, other=buffer==Base ? Base+Stride : Base;
    if(psx_mod_read_word(disp)!=psx_mod_read_word(other) ||
       psx_mod_read_word(disp+4)!=psx_mod_read_word(other+4))return;
    PSXModRenderPassFrame frame{};frame.struct_size=sizeof frame;frame.period_vblanks=replay.ticks;
    frame.x=psx_mod_read_half(disp);frame.y=psx_mod_read_half(disp+2);
    frame.w=psx_mod_read_half(disp+4);frame.h=psx_mod_read_half(disp+6);
    psx_mod_counter_add("ape.fr.passes",psx_mod_render_pass_frame(cpu,&frame,pass,nullptr));
}
void activate(unsigned fps){replay.invalidate();vblanks=0;PSXDrawReplay::rate(fps,PSX_MOD_RENDER_PASS_FLIP_PENDING);}
void display(){activate(0);}void rate60(){activate(60);}void rate90(){activate(90);}
void rate120(){activate(120);}void rate144(){activate(144);}void rate165(){activate(165);}void rate240(){activate(240);}
}
PSX_MOD_CONSTRUCTOR(ape_register_frame_rate_plugins){
    // One declared plugin per rate. Retain the original IDs for existing
    // rates; persisted package/feature/option selections remain unchanged.
    const char* ids[]={"ape.framerate.uncapped","ape.framerate.60","ape.frame-smoothing.90",
        "ape.framerate.120","ape.framerate.144","ape.framerate.165","ape.frame-smoothing.240"};
    void(*callbacks[])()={display,rate60,rate90,rate120,rate144,rate165,rate240};
    for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);++i){
        psx_mod_register_activation_plugin(ids[i],callbacks[i]);psx_mod_register_vblank_plugin(ids[i],tick);
        psx_mod_register_instruction_plugin(ids[i],Start,0x0C01F54C,begin);
        psx_mod_register_function_entry_plugin(ids[i],Submit,finish);
    }
}
