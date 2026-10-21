#include "../overlay_platform.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
using namespace cs2_camera_platform;
static unsigned services=0,polls=0,pumps=0;
static void Service(){++services;}
static void Pump(){EventDispatch dispatch(Service);++pumps;}
static bool Poll(void* event){EventDispatch dispatch(Service);++polls;Pump();return event!=nullptr;}
static void CursorRoundtrip(){
    CursorCapture cursor;bool relative=true,grab=true,visible=false,text=false;unsigned reads=0,writes=0;
    CursorCapture::Functions f;
    f.relative=[&]{++reads;return relative;};f.grab=[&]{++reads;return grab;};f.visible=[&]{++reads;return visible;};f.text=[&]{++reads;return text;};
    f.setRelative=[&](bool v){++writes;relative=v;};f.setGrab=[&](bool v){++writes;grab=v;};f.setVisible=[&](bool v){++writes;visible=v;};f.setText=[&](bool v){++writes;text=v;};
    cursor.Update(false,0x1000,true,-1,-1,-1,f);assert(!reads && !writes);
    cursor.Update(true,0x1000,true,-1,-1,-1,f);assert(!relative && !grab && visible && text && reads==4 && writes==4);
    cursor.Update(true,0x1000,true,-1,-1,-1,f);assert(reads==4 && writes==4);
    cursor.Update(true,0x1000,false,-1,-1,-1,f);assert(relative && grab && !visible && !text);
    cursor.Update(true,0x1000,true,-1,-1,-1,f);assert(!relative && !grab && visible && text);
    // Explicit false requests while captured supersede the old true baseline.
    cursor.Update(true,0x1000,false,0,0,1,f);assert(!relative && !grab && visible && !text);
    cursor.Update(true,0x1000,true,-1,-1,-1,f);const auto before=writes;
    cursor.Update(true,0,false,-1,-1,-1,f);assert(writes==before); // A destroyed window is never touched.
    relative=false;grab=false;visible=true;text=true;
    cursor.Update(true,0x2000,true,-1,-1,-1,f);assert(!relative && !grab && visible && text);
    cursor.Update(true,0x2000,false,-1,-1,-1,f);assert(!relative && !grab && visible && text);
}
int main(){
    // Stable mirrored keyboard state tracks native changes without events.
    bool physical[8]={},forwarded[8]={},native[8]={};const auto* cached=forwarded;
    native[2]=true;KeyboardSnapshot(physical,forwarded,native,8,false,true);assert(cached[2]);
    KeyboardSnapshot(physical,forwarded,native,8,true,true);assert(!cached[2]);
    KeyboardSnapshot(physical,forwarded,native,8,false,true);assert(cached[2]);
    native[2]=false;KeyboardSnapshot(physical,forwarded,native,8,false,true);assert(!cached[2]);
    native[3]=true;KeyboardSnapshot(physical,forwarded,native,8,false,false);assert(!cached[3]);
    native[3]=false;KeyboardSnapshot(physical,forwarded,native,8,false,true);assert(!cached[3]);
    CursorRoundtrip();
    assert(OwnEvent(7,7) && !OwnEvent(7,8) && !OwnEvent(0,0));
    auto p=Pointer(7,7,2560,1440,2560,1440,2000,600);assert(p && p->x==2000 && p->y==600);
    p=Pointer(7,7,1280,720,2560,1440,1000,300);assert(p && p->x==2000 && p->y==600);
    assert(!Pointer(7,8,1280,720,2560,1440,1000,300));
    assert(!Pointer(7,7,0,720,2560,1440,1000,300));
    assert(!Pointer(7,7,1280,720,0,1440,1000,300));
    assert(!Pointer(7,7,1280,720,2560,1440,std::numeric_limits<float>::quiet_NaN(),300));
    p=Pointer(7,7,1280,720,2560,1440,-10,730);assert(p && p->x==-20 && p->y==1460);
    std::vector<std::string> events;Point mouse{-1000,-1000};bool held=false;
    // No preceding motion: the click itself supplies the scaled hit position.
    p=Pointer(7,7,1280,720,2560,1440,1000,300);
    assert(PointerButton(p,true,[&](Point v){mouse=v;events.push_back("position");},[&](bool down){assert(mouse.x==2000 && mouse.y==600);held=down;events.push_back("down");}));
    assert(held && (events==std::vector<std::string>{"position","down"}));
    assert(!PointerButton(std::nullopt,true,[](Point){assert(false);},[](bool){assert(false);}));
    assert(PointerButton(std::nullopt,false,[](Point){assert(false);},[&](bool down){held=down;}));assert(!held);
    assert(!Poll(nullptr));assert(polls==1 && pumps==1 && services==2);
    assert(Poll(&mouse));assert(polls==2 && pumps==2 && services==4);
    DesktopFullscreen fullscreen;fullscreen.Requested(true);const auto now=DesktopFullscreen::Clock::time_point{};
    std::vector<std::string> calls;const auto mode=[&]{calls.push_back("desktop_null");return true;};const auto set=[&]{calls.push_back("fullscreen_true");return true;};
    fullscreen.Update(false,0x1000,7,true,false,now,mode,set);assert(calls.empty());
    fullscreen.Update(true,0x1000,7,false,false,now,mode,set);assert(calls.empty());
    fullscreen.Update(true,0,0,true,false,now,mode,set);assert(calls.empty());
    fullscreen.Update(true,0x1000,7,true,false,now,mode,set);assert((calls==std::vector<std::string>{"desktop_null","fullscreen_true"}));
    fullscreen.Update(true,0x1000,7,true,false,now+std::chrono::milliseconds(500),mode,set);assert(calls.size()==2);
    fullscreen.Update(true,0x1000,7,true,true,now+std::chrono::milliseconds(700),mode,set);assert(calls.size()==2 && fullscreen.State()==DesktopFullscreen::Status::Confirmed);
    // Replacement (including a recycled ID with a new pointer) resets budget.
    fullscreen.Update(true,0x2000,7,true,false,now+std::chrono::seconds(1),mode,set);assert(calls.size()==4 && fullscreen.Attempts()==1);
    for(int second=2;second<=10;++second)fullscreen.Update(true,0x2000,7,true,false,now+std::chrono::seconds(second),mode,set);
    assert(fullscreen.Attempts()==3 && calls.size()==8 && fullscreen.State()==DesktopFullscreen::Status::Failed);
    fullscreen.Update(true,0x2000,7,true,true,now+std::chrono::seconds(11),mode,set);assert(fullscreen.State()==DesktopFullscreen::Status::Confirmed && calls.size()==8);
    fullscreen.Update(true,0x3000,8,true,false,now+std::chrono::seconds(12),[]{return false;},[&]{assert(false);return false;});assert(fullscreen.Attempts()==1);
    fullscreen.Requested(false);fullscreen.Update(true,0x3000,8,true,false,now+std::chrono::seconds(13),mode,set);assert(fullscreen.State()==DesktopFullscreen::Status::Disabled && calls.size()==8);
    std::cout<<"Overlay platform: own-window coordinates/click ordering, F8 cursor roundtrip, nested poll/pump and bounded main-thread desktop fullscreen passed\n";
}
