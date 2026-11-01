// Exercise the same ImGui tool bodies in standalone windows and editor children.
#include "../camera_map_ui.h"
#include "../camera_timeline_ui.h"
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
using Json=nlohmann::json;
struct Item{ImRect bounds;std::string label;};
std::map<ImGuiID,Item>* items=nullptr;
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext*,ImGuiID id,const ImRect& bounds,const ImGuiLastItemData*){
  if(items&&id)(*items)[id].bounds=bounds;
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*,ImGuiID id,const char* label,ImGuiItemStatusFlags){
  if(items&&id&&label)(*items)[id].label=label;
}
void ImGuiTestEngineHook_Log(ImGuiContext*,const char*,...){}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*,ImGuiID){return nullptr;}

namespace {
enum Tool{Map,Curves,Timeline};
constexpr const char* Names[]={"Camera map / XY","Camera curves / values","Camera timeline / composition"};
struct Command{std::string method;Json value;};
class Fixture{
 public:
  cs2_live_ui::CameraMapUi map;
  cs2_live_ui::CameraCurveUi curves;
  cs2_live_ui::CameraTimelineUi timeline;
  Json state;
  std::string selected="1",numeric_commit;
  double numeric_value=0;
  Tool tool=Map;
  bool embedded=true;
  float child_width=1160,child_height=740,child_bottom=0,child_right=0,tool_scale=1;
  int backdrop_calls=0,popup_backdrops=0;
  std::vector<Command> commands;
  std::map<ImGuiID,Item> submitted;
  Fixture(){
    ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.LogFilename=nullptr;io.DisplaySize=ImVec2(1280,900);io.DeltaTime=1.f/60;
    ImFontConfig font;font.SizePixels=18;io.Fonts->AddFontDefault(&font);
    unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    ImGui::GetCurrentContext()->TestEngineHookItems=true;items=&submitted;
    map.visible=curves.visible=timeline.visible=true;
    state={{"paused",true},{"preview_playing",false},{"tick_rate",64},{"cursor",1.},{"project_document_id",1},
      {"editor_preferences",{{"timeline_height",240},{"show_tooltips",false}}},
      {"camera",{{"position",{500,500,48}},{"angles",{0,90,0}},{"fov",90}}},
      {"project",{{"duration",5.},{"keys",Json::array()}}}};
    for(int i=0;i<3;++i)state["project"]["keys"].push_back({{"id",std::to_string(i)},{"time",i*2.},
      {"position",{i*100,0,48}},{"angles",{0,i*10,0}},{"fov",90}});
  }
  ~Fixture(){items=nullptr;ImGui::DestroyContext();}
  void Reset(){map.ResetGesture();curves.ResetGesture();timeline.ResetGesture();}
  void Step(){
    int frames=0;do{
      Require(++frames<16,"Input queue did not drain");submitted.clear();backdrop_calls=popup_backdrops=0;
      ImGui::NewFrame();ImGui::SetNextWindowPos(ImVec2(20,20));ImGui::SetNextWindowSize(ImVec2(1200,800));
      ImGui::Begin("Surface host",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
      ImGui::BeginChild("Embedded tool",ImVec2(child_width,child_height));ImGui::SetWindowFontScale(1.1f);
      auto* current=ImGui::GetCurrentWindow();const auto stack=ImGui::GetCurrentContext()->CurrentWindowStack.Size;
      child_bottom=current->InnerClipRect.Max.y;
      child_right=current->InnerClipRect.Max.x;
      const auto font_scale=current->FontWindowScale;
      auto send=[&](const std::string& method,const Json& value){
        commands.push_back({method,value});
        if(method=="set_project")state["project"].update(value);
        if(method=="update_key")for(auto& key:state["project"]["keys"])if(key["id"]==value["id"])key.update(value);
        if(method=="set_editor_preferences")state["editor_preferences"].update(value);
      };
      auto select=[&](const std::string& id){selected=id;};
      auto backdrop=[&]{++backdrop_calls;if(ImGui::GetCurrentWindow()->Flags&ImGuiWindowFlags_Popup)++popup_backdrops;};
      auto numeric=[&](const char* label,double* value,double,double,const char* format){
        ImGui::InputDouble(label,value,0,0,format);
        if(numeric_commit==label){*value=numeric_value;numeric_commit.clear();return true;}return false;
      };
      if(tool==Map){
        if(embedded)map.Draw(state,send,selected,select,tool_scale,backdrop,true);
        else map.Draw(state,send,selected,select,tool_scale,backdrop);
      }else if(tool==Curves){
        if(embedded)curves.Draw(state,send,selected,select,tool_scale,numeric,backdrop,true);
        else curves.Draw(state,send,selected,select,tool_scale,numeric,backdrop);
      }else{
        if(embedded)timeline.Draw(state,send,selected,select,tool_scale,backdrop,numeric,true);
        else timeline.Draw(state,send,selected,select,tool_scale,backdrop,numeric);
      }
      Require(ImGui::GetCurrentWindow()==current,"Tool changed the parent's current window");
      Require(ImGui::GetCurrentContext()->CurrentWindowStack.Size==stack,"Tool leaked or closed a window scope");
      Require(current->FontWindowScale==font_scale,"Tool changed its parent font scale");
      Require(ImGui::GetCurrentContext()->NextWindowData.HasFlags==0,"Tool left standalone window settings for the next child");
      ImGui::EndChild();ImGui::End();ImGui::Render();
    }while(ImGui::GetCurrentContext()->InputEventsQueue.Size);
  }
  ImVec2 Point(const char* label){
    for(const auto& entry:submitted)if(entry.second.label==label)return entry.second.bounds.GetCenter();
    throw std::runtime_error(std::string("Missing actual ImGui tool item: ")+label);
  }
  void Mouse(ImVec2 p){ImGui::GetIO().AddMousePosEvent(p.x,p.y);Step();}
  void Drag(const char* label,float dx,float dy,bool cancel=false){
    const auto start=Point(label);Mouse(start);ImGui::GetIO().AddMouseButtonEvent(0,true);Step();
    Mouse(ImVec2(start.x+dx,start.y+dy));Require(commands.empty(),"Tool committed a gesture before release");
    if(cancel)Reset();
    ImGui::GetIO().AddMouseButtonEvent(0,false);Step();
  }
};
void OwnershipAndErrors(){
  for(Tool tool:{Map,Curves,Timeline}){
    Fixture f;f.tool=tool;f.Step();f.Step();
    Require(f.backdrop_calls==0,"Embedded tool painted a standalone body backdrop");
    Require(!ImGui::FindWindowByName(Names[tool]),"Embedded tool created its standalone window");
    const auto valid=f.state["project"];f.state["project"]={{"keys","invalid"}};f.Step();
    Require(f.backdrop_calls==0,"Invalid embedded project painted standalone backdrop");
    f.state["project"]=valid;f.embedded=false;f.Step();f.Step();
    Require(f.backdrop_calls==1,"Standalone default lost its body backdrop");
    Require(ImGui::FindWindowByName(Names[tool])->Active,"Standalone default did not draw its window");
    f.state["project"]={{"keys","invalid"}};f.Step();
  }
}
void EmbeddedPopupAndNumbers(){
  Fixture f;f.tool=Curves;f.Step();f.curves.OpenChannelMenuForDiagnostics();f.Step();f.Step();
  Require(f.popup_backdrops==1&&f.backdrop_calls==1,"Embedded channel popup lost its own backdrop callback");
  ImGui::ClosePopupToLevel(0,true);f.Step();f.numeric_commit="##curve_value";f.numeric_value=125;f.Step();
  Require(f.commands.size()==1&&f.commands[0].method=="update_key"&&f.commands[0].value["position"][0]==125,
    "Embedded values bypassed the supplied numeric commit callback");
  f.commands.clear();f.tool=Timeline;f.Reset();f.numeric_commit="Duration##composition_duration";f.numeric_value=6;f.Step();
  Require(f.commands.size()==1&&f.commands[0].method=="set_project"&&f.commands[0].value["duration"]==6,
    "Embedded timeline bypassed the supplied numeric commit callback");
}
void EmbeddedGestures(){
  for(Tool tool:{Map,Curves,Timeline}){
    Fixture f;f.tool=tool;f.Step();f.Step();
    const char* label=tool==Map?"##map_handle_1":tool==Curves?"##curve_key_1":"##composition_key_Position_1";
    f.Drag(label,35,tool==Curves?-18.f:0.f);
    Require(f.commands.size()==1,"Embedded gesture did not commit exactly one edit");
    Require(f.commands[0].method==(tool==Curves?"set_project":"update_key"),"Embedded gesture changed its command contract");
    Require(f.selected=="1","Embedded gesture lost shared selection");
    f.commands.clear();f.Step();f.Drag(label,24,tool==Curves?-18.f:0.f,true);
    Require(f.commands.empty(),"ResetGesture committed a hidden tool's pending edit");
  }
}
void EmbeddedTimelineHeight(){
  Fixture f;f.tool=Timeline;f.child_width=800;f.child_height=350;f.state["editor_preferences"]["timeline_height"]=420;f.Step();f.Step();
  bool found=false;
  for(const auto& entry:f.submitted){
    const auto& label=entry.second.label;
    Require(label!="##composition_units"&&label!="FPS##composition_rate"&&label!="Height##composition_height",
      "Embedded timeline repeated setup controls from Settings");
    if(label=="Duration##composition_duration"||label=="##composition_range"||label=="##composition_resize")
      Require(entry.second.bounds.Max.x<=f.child_right,"Embedded timeline controls overflowed a narrow child");
    if(label=="##camera_composition_canvas"){
      found=true;Require(entry.second.bounds.GetHeight()<420,"Embedded timeline ignored host height");
      Require(entry.second.bounds.Max.y<=f.child_bottom,"Embedded timeline canvas extended beyond available child");
    }
  }
  Require(found,"Embedded timeline canvas was not submitted");
}
void FractionalTimelineEdge(){
  Fixture f;f.tool=Timeline;f.tool_scale=720.f/1080;
  f.state["project"]["render_range"]={{"mode","region"},{"in",0.},{"out",4.}};f.Step();f.Step();
  const auto keys=f.state["project"]["keys"];
  const auto start=f.Point("##composition_in");
  Require(start.x-std::floor(start.x)>.1f,"Edge regression did not exercise a fractional marker center");
  f.Drag("##composition_in",30*f.tool_scale,0);
  Require(f.commands.size()==1&&f.commands[0].method=="set_project"&&f.commands[0].value["render_range"]["in"]>0,
    "Floored pointer could not grab the visible In=0 marker");
  Require(f.state["project"]["keys"]==keys,"Left-edge region drag changed camera keys");
}
}
int main(){try{
  OwnershipAndErrors();EmbeddedPopupAndNumbers();EmbeddedGestures();EmbeddedTimelineHeight();FractionalTimelineEdge();
  std::cout<<"PASS: tool child ownership, standalone defaults, early returns, popup backdrops, numeric commits, gestures and cancellation\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
