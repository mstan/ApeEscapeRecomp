#include "ape_mouse_gadget_stick.h"
#include "local_mouse_policy.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
namespace {
constexpr uint64_t Ms=1000000;
ape::MouseGadgetStick stick;
unsigned notices=0, captures=0, releases=0;
bool capture(void*,bool on) { on ? ++captures : ++releases; return true; }
void suppress(void*,psx::MouseControl) {}
void notice(void*,const char*) { ++notices; }
int eligible(uint32_t buttons) { return (buttons&9)==9; }
void event(const PSXModMouseEvent* e) {
    switch(e->type) {
    case PSX_MOD_MOUSE_MOTION: stick.motion(e->time_ns,e->time_ns,e->dx,e->dy);break;
    case PSX_MOD_MOUSE_HOLD_PRESS: stick.hold(true);break;
    case PSX_MOD_MOUSE_HOLD_RELEASE: stick.hold(false);break;
    default:stick.reset();break;
    }
}
void sample(uint64_t now,PSXModMouseOutput* out) {
    uint8_t rx,ry;stick.bytes(now,rx,ry);out->override_right=1;out->rx=rx;out->ry=ry;
}
}
int main() {
    psx::LocalMousePolicy policy({nullptr,capture,suppress,notice});
    PSXModMousePolicy callbacks{sizeof callbacks,PSX_MOD_MOUSE_HOLD_RIGHT,eligible,event,sample};
    CHECK(policy.install(&callbacks));
    psx::LocalMouseHost host;host.live=host.focused=host.connected=host.analog=true;
    uint64_t now=10*Ms;
    auto press=[&]() {policy.update(now,host);return policy.control(now,now,psx::MouseControl::Left,true,false);};
    auto release=[&]() {return policy.control(UINT64_MAX,0,psx::MouseControl::Left,false,false);};
    uint8_t x=99,y=77;
    CHECK(press());policy.sample(now,x,y);CHECK(x==128 && y==128);
    policy.motion(now+Ms,now+Ms,48,0);policy.sample(now+Ms,x,y);CHECK(x==255 && y==128);
    CHECK(release());CHECK(!policy.captured());stick.bytes(now+Ms,x,y);CHECK(x==128 && y==128);
    policy.motion(now+2*Ms,now+2*Ms,48,0);stick.bytes(now+2*Ms,x,y);CHECK(x==128 && y==128);
    now+=3*Ms;CHECK(press());policy.sample(now,x,y);CHECK(x==128 && y==128);
    policy.control(now+Ms,now+Ms,psx::MouseControl::Right,true,false);
    policy.motion(now+2*Ms,now+2*Ms,0,48);policy.sample(now+2*Ms,x,y);CHECK(x==128 && y==255);
    now+=200*Ms;policy.update(now,host);policy.sample(now,x,y);CHECK(x==128 && y==255);
    CHECK(release());stick.bytes(now,x,y);CHECK(x==128 && y==128);
    now+=Ms;CHECK(press());policy.sample(now,x,y);CHECK(x==128 && y==128);
    // RIGHT remained down across LEFT release and cannot hold the new session.
    policy.motion(now+Ms,now+Ms,48,0);
    now+=170*Ms;policy.update(now,host);policy.sample(now,x,y);CHECK(x==128 && y==128);
    CHECK(release());policy.control(now,now,psx::MouseControl::Right,false,false);

    // Actual reducer and runtime policy together: useful amplitude and winding
    // in both senses while LEFT stays held, with no continuation on release.
    constexpr double Pi=3.14159265358979323846;
    for(int sense : {-1,1}) {
        now+=10*Ms;CHECK(press());
        double previous_angle=0, winding=0;bool have_angle=false;
        double px=80,py=0;unsigned useful=0;
        for(int i=1;i<=750;++i) {
            now+=2*Ms;policy.update(now,host);
            const double a=sense*2*Pi*double(i)/500.0;
            const double nx=80*std::cos(a),ny=80*std::sin(a);
            policy.motion(now,now,nx-px,ny-py);px=nx;py=ny;
            policy.sample(now,x,y);
            const double vx=int(x)-128,vy=int(y)-128;
            if(std::hypot(vx,vy)>80)++useful;
            const double angle=std::atan2(vy,vx);
            if(i>250 && have_angle)winding+=std::remainder(angle-previous_angle,2*Pi);
            previous_angle=angle;have_angle=true;
            CHECK(policy.captured());
        }
        CHECK(useful>650 && sense*winding>5.5 && sense*winding<7.0);
        CHECK(release());stick.bytes(now,x,y);CHECK(x==128 && y==128);
        now+=100*Ms;stick.bytes(now,x,y);CHECK(x==128 && y==128);
    }
    // Every loss of ownership needs an actual release/repress, not a held edge.
    for(int gate=0;gate<7;++gate) {
        now+=10*Ms;CHECK(press());policy.motion(now+Ms,now+Ms,48,0);
        auto off=host;
        if(gate==0)off.focused=false;if(gate==1)off.live=false;
        if(gate==2)off.connected=false;if(gate==3)off.analog=false;
        if(gate==4)off.native_right=true;if(gate==5)off.start=true;
        if(gate==6)off.buttons=0xFFFE; // Select: guest menu-entry guard
        now+=2*Ms;policy.update(now,off);CHECK(!policy.captured());
        stick.bytes(now,x,y);CHECK(x==128 && y==128);
        now+=Ms;policy.update(now,host);
        CHECK(!policy.control(now,now,psx::MouseControl::Left,true,false));
        policy.motion(now,now,48,0);stick.bytes(now,x,y);CHECK(x==128 && y==128);
        CHECK(release());now+=Ms;CHECK(press());policy.sample(now,x,y);CHECK(x==128 && y==128);
        CHECK(release());
    }
    CHECK(notices==0);
    std::printf("silent LEFT-held reducer/policy integration: %u captures, %u releases, both circle senses and interruption gates pass\n",captures,releases);
    return 0;
}
