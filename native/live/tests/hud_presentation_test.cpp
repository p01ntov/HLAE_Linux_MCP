#include "../hud_presentation.h"
#include <cassert>
#include <iostream>

int main() {
    using cs2_live::HudPresentationEvidence;
    HudPresentationEvidence state;
    assert(!state.Ready());
    bool refused=false;try{state.Request(true);}catch(const std::runtime_error&){refused=true;}
    assert(refused);
    state.Context(true,2);state.Request(true);assert(state.Override());
    for(unsigned i=0;i<5;++i)state.Record(i,true,true,false,false);
    assert(state.Ready());
    auto caps=state.Capabilities(true,"");assert(caps["available"]==true && caps["gameplay_active"]==true);
    assert(caps["render_verified"]==false);
    state.Record(0,false,true,true,true);assert(!state.Ready());
    state.Record(0,true,true,false,false);assert(state.Ready());
    // A real observer-mode change disables the presentation and invalidates
    // old evidence; prior first-person callbacks cannot prove a free view.
    state.Context(true,4);assert(!state.Override() && !state.Ready());
    refused=false;try{state.Request(true);}catch(const std::runtime_error&){refused=true;}
    assert(refused);
    state.Request(false);
    for(unsigned i=0;i<5;++i)state.Record(i,true,i%2,i%2,bool(i%2));
    assert(state.Ready() && state.Capabilities(true,"")["gameplay_active"]==false);
    // Correctly forwarding the requested value still requires engine readback.
    state.Record(2,true,true,true,std::nullopt);assert(!state.Ready());
    assert(state.Capabilities(false,"unavailable")["available"]==false);
    cs2_live::LinuxHudPresentation noGame;
    assert(noGame.Install()!=nullptr);assert(!noGame.Installed());assert(noGame.Remove());
    std::cout<<"hud presentation evidence and unavailable-build tests passed\n";
}
