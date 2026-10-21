#include "../overlay_platform.h"
#include "imgui.h"
#include <cassert>
#include <iostream>
using namespace cs2_camera_platform;
int main(){
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(2560,1440);io.DisplayFramebufferScale=ImVec2(1,1);io.DeltaTime=1.f/60.f;
    unsigned char* pixels=nullptr;int width=0,height=0;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);assert(pixels && width && height);
    unsigned clicked=0;ImVec2 center;
    const auto frame=[&]{ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(1700,500));ImGui::SetNextWindowSize(ImVec2(400,200));ImGui::Begin("Target window",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoTitleBar);
        if(ImGui::Button("Clickable tab",ImVec2(180,40)))++clicked;const auto lo=ImGui::GetItemRectMin(),hi=ImGui::GetItemRectMax();center=ImVec2((lo.x+hi.x)/2,(lo.y+hi.y)/2);ImGui::End();ImGui::Render();};
    frame();frame();
    // Window registration happens after its original focus-gained event. The
    // queued reset and actual main-thread focus snapshot must still allow the
    // first click, with no preceding mouse-motion event.
    io.AddFocusEvent(false);io.AddFocusEvent(true);
    const auto point=Pointer(7,7,1280,720,2560,1440,center.x/2,center.y/2);assert(point);
    const auto click=[&](bool down){PointerButton(point,down,[&](Point p){io.AddMousePosEvent(p.x,p.y);},[&](bool pressed){io.AddMouseButtonEvent(0,pressed);});};
    click(true);for(unsigned i=0;i<4;++i)frame();
    assert(!io.AppFocusLost && io.MouseDown[0] && io.MousePos.x==center.x && io.MousePos.y==center.y);
    click(false);for(unsigned i=0;i<4;++i)frame();assert(clicked==1 && !io.MouseDown[0]);
    // An auxiliary SDL window cannot inject focus loss or a stale click here.
    if(OwnEvent(7,8))io.AddFocusEvent(false);
    assert(!Pointer(7,8,1280,720,2560,1440,center.x/2,center.y/2));frame();assert(!io.AppFocusLost && clicked==1);
    ImGui::DestroyContext();std::cout<<"Real ImGui: initial focus synchronization and scaled first-click hit without motion passed\n";
}
