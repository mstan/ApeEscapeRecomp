#include "ape_mouse_gadget_stick.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
constexpr double Pi = 3.14159265358979323846;
constexpr uint64_t Ms = 1000000;
struct Event { uint64_t t; double x, y; };
double magnitude(ape::MouseGadgetVector v) { return std::hypot(v.x, v.y); }
double angle_difference(double a, double b) { return std::remainder(a-b, 2*Pi); }
std::vector<Event> circle(int hz, double turns, int sign, double duration = 3.0) {
    std::vector<Event> events;
    double px = 80, py = 0;
    for (int i = 1; i <= int(duration * hz); ++i) {
        const double t = double(i)/hz, a = sign*2*Pi*turns*t;
        const double x = 80*std::cos(a), y = 80*std::sin(a);
        events.push_back({uint64_t(std::llround(t*1e9)), x-px, y-py});
        px=x; py=y;
    }
    return events;
}

bool correction_regressions() {
    bool passed=true; unsigned cases=0;
    // Keep the host responsive during a quiet second. Neither samples nor
    // zero packets may make ancient velocity reverse a fresh small stroke.
    for(int axis : {0,1}) for(int sign : {-1,1}) for(int parts : {1,2,4}) {
        ape::MouseGadgetStick reducer,whole;
        const double px=axis==0?sign*1000.0:0,py=axis==1?sign*1000.0:0;
        bool ok=reducer.motion(0,0,px,py) && whole.motion(0,0,px,py);
        for(uint64_t t=10*Ms;t<1000*Ms;t+=10*Ms) {
            ok=reducer.motion(t,t,0,0) && ok;
            (void)reducer.sample(t);
        }
        for(int p=0;p<parts;++p)
            ok=reducer.motion(1000*Ms,1000*Ms,axis==0?sign*3.0/parts:0,axis==1?sign*3.0/parts:0) && ok;
        ok=whole.motion(1000*Ms,1000*Ms,axis==0?sign*3.0:0,axis==1?sign*3.0:0) && ok;
        const auto v=reducer.sample(1000*Ms);
        const double major=sign*(axis==0?v.x:v.y),minor=axis==0?v.y:v.x;
        uint8_t rx,ry,wx,wy;reducer.bytes(1000*Ms,rx,ry);whole.bytes(1000*Ms,wx,wy);
        ok=ok && major>0 && std::abs(minor)<1e-12 && magnitude(v)<=1 &&
           std::abs(int(rx)-int(wx))<=1 && std::abs(int(ry)-int(wy))<=1 &&
           magnitude(reducer.sample(1160*Ms))==0;
        if(!ok)std::printf("FAIL stale correction axis=%d sign=%d parts=%d major=%.9f minor=%.9f\n",axis,sign,parts,major,minor);
        passed=passed && ok;++cases;
    }
    // The old extrapolated X (or mirrored Y) almost cancels to zero here;
    // normalizing it must not magnify the tiny orthogonal input into a turn.
    for(int axis : {0,1}) for(int sign : {-1,1}) for(int noise_sign : {-1,1}) for(int parts : {1,2,4}) {
        ape::MouseGadgetStick reducer,whole;
        const double initial=sign*100.0;
        const double change=sign*(100.0/3-100*std::exp(-0.08));
        const double noise=noise_sign*0.001;
        bool ok=reducer.motion(0,0,axis==0?initial:0,axis==1?initial:0) &&
                whole.motion(0,0,axis==0?initial:0,axis==1?initial:0);
        for(int p=0;p<parts;++p)
            ok=reducer.motion(8*Ms,8*Ms,(axis==0?change:noise)/parts,(axis==1?change:noise)/parts) && ok;
        ok=whole.motion(8*Ms,8*Ms,axis==0?change:noise,axis==1?change:noise) && ok;
        const auto v=reducer.sample(8*Ms);
        const double major=sign*(axis==0?v.x:v.y),minor=axis==0?v.y:v.x;
        uint8_t rx,ry,wx,wy;reducer.bytes(8*Ms,rx,ry);whole.bytes(8*Ms,wx,wy);
        ok=ok && major>=0.9999*magnitude(v) && magnitude(v)>0.5 && magnitude(v)<=1 &&
           std::abs(minor)<=0.001*magnitude(v) &&
           std::abs(int(rx)-int(wx))<=1 && std::abs(int(ry)-int(wy))<=1;
        if(!ok)std::printf("FAIL near-zero correction axis=%d sign=%d noise=%d parts=%d major=%.9f minor=%.9f\n",axis,sign,noise_sign,parts,major,minor);
        passed=passed && ok;++cases;
    }
    // A nonzero extrapolation pointing opposite the remaining displacement
    // is unsafe too, even when normalization is numerically well defined.
    for(int axis : {0,1}) for(int sign : {-1,1}) for(int parts : {1,2,4}) for(double residual : {5.0,15.0}) {
        ape::MouseGadgetStick reducer,whole;
        const double initial=sign*100.0,change=sign*(residual-100*std::exp(-0.08));
        bool ok=reducer.motion(0,0,axis==0?initial:0,axis==1?initial:0) &&
                whole.motion(0,0,axis==0?initial:0,axis==1?initial:0);
        for(int p=0;p<parts;++p)
            ok=reducer.motion(8*Ms,8*Ms,axis==0?change/parts:0,axis==1?change/parts:0) && ok;
        ok=whole.motion(8*Ms,8*Ms,axis==0?change:0,axis==1?change:0) && ok;
        const auto v=reducer.sample(8*Ms);
        uint8_t rx,ry,wx,wy;reducer.bytes(8*Ms,rx,ry);whole.bytes(8*Ms,wx,wy);
        ok=ok && sign*(axis==0?v.x:v.y)>0 && std::abs(axis==0?v.y:v.x)<1e-12 &&
           std::abs(int(rx)-int(wx))<=1 && std::abs(int(ry)-int(wy))<=1;
        if(!ok)std::printf("FAIL inconsistent correction axis=%d sign=%d parts=%d residual=%.1f\n",axis,sign,parts,residual);
        passed=passed && ok;++cases;
    }
    std::printf("stale/near-zero/inconsistent correction regressions: %u cases, %s\n",cases,passed?"passed":"failed");
    return passed;
}

int main() {
    CHECK(correction_regressions());
    ape::MouseGadgetStick s;
    for (int d=0; d<8; ++d) {
        s.reset(); const double a=d*Pi/4;
        CHECK(s.motion(10*Ms,10*Ms,60*std::cos(a),60*std::sin(a)));
        const auto v=s.sample(10*Ms);
        CHECK(std::abs(magnitude(v)-1)<1e-12);
        CHECK(std::abs(angle_difference(std::atan2(v.y,v.x),a))<1e-12);
        uint8_t x,y; s.bytes(170*Ms,x,y); CHECK(x==128 && y==128);
        CHECK(magnitude(s.sample(90*Ms))>0.999);
        CHECK(std::abs(magnitude(s.sample(130*Ms))-0.5)<1e-12);
        CHECK(magnitude(s.sample(1000*Ms))==0);
    }
    s.reset(); s.motion(10*Ms,10*Ms,10,0); s.motion(10*Ms,10*Ms,-9,0);
    CHECK(magnitude(s.sample(10*Ms))==0); // cancellation must clear below gate
    s.motion(10*Ms,10*Ms,-1,0); CHECK(magnitude(s.sample(10*Ms))==0);
    ape::MouseGadgetStick a,b;
    a.motion(0,0,10,0); b.motion(0,0,10,0);
    a.motion(0,0,-10,0); b.motion(0,0,-4.5,0); b.motion(0,0,-5.5,0);
    CHECK(magnitude(a.sample(0))==0 && magnitude(b.sample(0))==0);
    // Direction correction is shared by equal-time splits and is frozen during
    // silence; it cannot bypass the original displacement cancellation gate.
    a.reset();b.reset();
    a.motion(0,0,20,0);b.motion(0,0,20,0);
    a.motion(8*Ms,8*Ms,0,20);
    b.motion(8*Ms,8*Ms,0,8);b.motion(8*Ms,8*Ms,0,12);
    CHECK(a.sample(8*Ms).x==b.sample(8*Ms).x && a.sample(8*Ms).y==b.sample(8*Ms).y);
    const auto directed=a.sample(8*Ms),quiet=a.sample(120*Ms);
    CHECK(std::abs(angle_difference(std::atan2(directed.y,directed.x),std::atan2(quiet.y,quiet.x)))<1e-12);
    const double remaining_x=20*std::exp(-8.0/100.0);
    a.motion(8*Ms,8*Ms,-remaining_x,-20);
    b.motion(8*Ms,8*Ms,-remaining_x/2,-10);b.motion(8*Ms,8*Ms,-remaining_x/2,-10);
    CHECK(magnitude(a.sample(8*Ms))==0 && magnitude(b.sample(8*Ms))==0);
    for(int i=0;i<30;++i) s.motion(20*Ms,20*Ms,0.1,0.1);
    CHECK(magnitude(s.sample(20*Ms))>0); // fractional packets cross accumulated gate
    s.hold(true); CHECK(magnitude(s.sample(20*Ms))==0);
    s.motion(21*Ms,21*Ms,30,10); const auto held=s.sample(21*Ms);
    CHECK(s.sample(9000*Ms).x==held.x);
    s.hold(false); CHECK(magnitude(s.sample(21*Ms))==0);
    s.motion(22*Ms,22*Ms,60,0);
    CHECK(s.motion(150*Ms,150*Ms,0,0)); CHECK(magnitude(s.sample(182*Ms))==0);
    s.reset(); s.motion(0,0,60,0); s.motion(30*Ms,30*Ms,-60,0);
    CHECK(s.sample(30*Ms).x<0); // measured lag remains displacement-dependent
    s.reset(); s.motion(0,0,30,10);
    CHECK(std::abs(std::atan2(s.sample(0).y,s.sample(0).x)-std::atan2(10.0,30.0))<1e-12);
    CHECK(!s.motion(31*Ms,30*Ms,10,0)); CHECK(magnitude(s.sample(40*Ms))==0);
    s.motion(50*Ms,50*Ms,10,0);
    CHECK(!s.motion(49*Ms,50*Ms,10,0));
    CHECK(!s.motion(0,300*Ms,10,0));
    CHECK(!s.motion(0,0,std::numeric_limits<double>::infinity(),0));
    s.motion(0,0,1e8,-1e8); CHECK(magnitude(s.sample(0))<=1.000000000001);
    s.configure({1.0,true,true}); s.motion(0,0,20,30);
    CHECK(s.sample(0).x<0 && s.sample(0).y<0);
    s.configure({std::numeric_limits<double>::quiet_NaN(),false,false});
    s.motion(0,0,48,0); CHECK(magnitude(s.sample(0))==1);

    // Same available timestamped history at common times, independently of
    // render/query cadence and event delivery batches. Queries never mutate.
    const auto trace=circle(1000,1,1);
    for (int fps : {30,60,120,144,240}) {
        ape::MouseGadgetStick reference, queried;
        size_t i=0; uint64_t next_query=0;
        for(uint64_t t=0;t<=3000*Ms;t+=10*Ms) {
            while(i<trace.size() && trace[i].t<=t) {
                const auto e=trace[i++];
                reference.motion(e.t,e.t,e.x,e.y); // immediate delivery vs batch
                queried.motion(e.t,t,e.x/4,e.y/4);
                queried.motion(e.t,t,e.x*3/4,e.y*3/4);
            }
            while(next_query<=t) {
                (void)queried.sample(next_query);
                next_query+=uint64_t(1e9/fps);
            }
            (void)queried.sample(t+100*Ms); (void)queried.sample(0);
            uint8_t ax,ay,bx,by; reference.bytes(t,ax,ay); queried.bytes(t,bx,by);
            CHECK(std::abs(int(ax)-int(bx))<=1 && std::abs(int(ay)-int(by))<=1);
            const auto v=queried.sample(t); CHECK(v.x==queried.sample(t).x && v.y==queried.sample(t).y);
        }
    }
    // Winding AND useful amplitude, rather than accepting a tiny rotating vector.
    for (double turns : {0.5,1.0,2.0}) for (int sign : {-1,1}) {
        ape::MouseGadgetStick reducer;
        double winding=0, last=0; bool started=false;
        auto events=circle(1000,turns,sign);
        for(const auto& e:events) {
            reducer.motion(e.t,e.t,e.x,e.y);
            if(e.t<1000*Ms || e.t%(10*Ms)!=0) continue;
            auto v=reducer.sample(e.t); CHECK(magnitude(v)>0.45);
            const double angle=std::atan2(v.y,v.x);
            if(started) winding+=angle_difference(angle,last);
            last=angle; started=true;
        }
        CHECK(sign*winding>2*Pi*turns*1.9);
        CHECK(magnitude(reducer.sample(events.back().t+160*Ms))==0);
    }
    // A figure-eight must turn in both senses and stop without extra turns.
    s.reset(); double px=0,py=0,previous_angle=0;
    bool positive_turn=false,negative_turn=false,have_angle=false;
    for(int i=1;i<=2000;++i) {
        double t=i/1000.0,x=80*std::sin(2*Pi*t),y=40*std::sin(4*Pi*t);
        CHECK(s.motion(uint64_t(i)*Ms,uint64_t(i)*Ms,x-px,y-py));
        CHECK(magnitude(s.sample(uint64_t(i)*Ms))<=1.000000000001);
        const auto v=s.sample(uint64_t(i)*Ms);
        if(i>1000 && magnitude(v)>0.1) {
            const double angle=std::atan2(v.y,v.x);
            if(have_angle) {
                const double delta=angle_difference(angle,previous_angle);
                positive_turn=positive_turn || delta>0.001;
                negative_turn=negative_turn || delta<-0.001;
            }
            previous_angle=angle;have_angle=true;
        }
        px=x;py=y;
    }
    CHECK(positive_turn && negative_turn);
    CHECK(magnitude(s.sample(2160*Ms))==0);
    // Physical polling approximations are distinct from exact-trace invariants.
    for(double turns : {0.5,1.0,2.0}) {
        std::array<int,7> rates{125,250,500,1000,2000,4000,8000};
        std::array<std::vector<Event>,7> paths;
        std::array<ape::MouseGadgetStick,7> reducers;
        std::array<size_t,7> index{};
        for(size_t r=0;r<rates.size();++r) paths[r]=circle(rates[r],turns,1);
        double max_gain=0,max_phase=0;
        for(uint64_t t=0;t<=3000*Ms;t+=Ms) {
            std::array<ape::MouseGadgetVector,7> vectors;
            for(size_t r=0;r<rates.size();++r) {
                while(index[r]<paths[r].size() && paths[r][index[r]].t<=t) {
                    auto e=paths[r][index[r]++]; reducers[r].motion(e.t,t,e.x,e.y);
                }
                vectors[r]=reducers[r].sample(t);
            }
            if(t<1000*Ms) continue;
            const auto ref=vectors.back();
            for(auto v:vectors) {
                max_gain=std::max(max_gain,std::abs(magnitude(v)/magnitude(ref)-1));
                max_phase=std::max(max_phase,std::abs(angle_difference(std::atan2(v.y,v.x),std::atan2(ref.y,ref.x)))*180/Pi);
            }
        }
        std::printf("circle %.1f Hz, 125..8000 polling: gain %.3f%% phase %.3f deg\n",turns,max_gain*100,max_phase);
        CHECK(max_gain<=0.05 && max_phase<=5.0);
    }
    std::puts("mouse gesture bounds, timing, cancellation, hold and geometry passed");
    return 0;
}
