// Reuse the real editor's public ImGui fixture and item discovery hooks. The
// original main is not run; this suite exercises the embedded composition UI.
#define main ExistingEditorRegressionMain
#include "editor_ui_test.cpp"
#undef main

namespace {
constexpr const char* TimelineWindow="Camera timeline / composition";
void TimelineMath(){
  using namespace cs2_live_ui::timeline_time;
  Near(SecondsToDisplay(.125,"ticks",128,60),16,"actual tick rate");
  Near(DisplayToSeconds(125,"frames",64,1000),.125,"1000 FPS units");
  Near(Snap(.13,"tick",64,60),.125,"tick snap");
  Near(Snap(.13,"frame",64,60),8./60,"frame snap");
  Near(Snap(.13,"tick",64,60,true),.13,"Shift bypass");
  Json raw={{"duration",8.},{"interpolation","bezier"},{"rotation_interpolation","bezier"},{"fov_interpolation","bezier"},
    {"render_range",{{"mode","region"},{"in",1.},{"out",7.}}},{"keys",Json::array()}};
  for(int i=0;i<3;++i)raw["keys"].push_back({{"id",std::to_string(i)},{"time",i*3.5},{"position",{i*100,20,30}},{"angles",{0,i*30,0}},{"fov",74},
    {"position_out",{10,20,30}},{"rotation_in",nullptr},{"fov_out",12},{"position_easing",{.1,.2,.8,.9}}});
  const auto original=raw;const auto keep=ResizeProject(raw,12,"keep");Require(keep["keys"]==raw["keys"],"Keep moved camera keys");Near(keep["render_range"]["in"],1,"keep In");Near(keep["render_range"]["out"],7,"keep Out");
  bool rejected=false;try{ResizeProject(raw,6,"keep");}catch(const std::exception&){rejected=true;}Require(rejected,"Keep shrank past last camera key");
  const auto scaled=ResizeProject(raw,16,"scale");
  for(std::size_t i=0;i<3;++i){Json expected=raw["keys"][i];expected["time"]=i*7.;Require(scaled["keys"][i]==expected,"Scale changed curve shape/lens/angle controls");}
  Near(scaled["render_range"]["in"],2,"scaled In");Near(scaled["render_range"]["out"],14,"scaled Out");Require(raw==original,"Resize mutated source project");
  Json tight=raw;tight["duration"]=.01;tight["render_range"]={{"mode","composition"},{"in",0},{"out",.01}};
  for(int i=0;i<3;++i)tight["keys"][i]["time"]=i*1e-6;
  const auto parsed=cs2_live::ReadProject(tight);const auto bounds=KeyTimeBounds(parsed,1);
  Require(bounds.first<=1e-6&&bounds.second>=1e-6,"Tight valid key excluded by time bounds");
  Near(MinimumDuration(tight,"scale"),.01,"tight scale minimum");
  Json boundary={{"duration",10},{"keys",Json::array()}};
  for(int i=0;i<2;++i)boundary["keys"].push_back({{"id",std::to_string(i)},{"time",1+i*.0001},{"position",{0,0,0}},{"angles",{0,0,0}}});
  const double minimum=MinimumDuration(boundary,"scale");const auto minimum_patch=ResizeProject(boundary,minimum,"scale");
  Require(minimum_patch["render_range"]["out"]==minimum_patch["duration"],"Scaled full range Out rounded beyond composition end");
  Require(minimum_patch["keys"][1]["time"].get<double>()-minimum_patch["keys"][0]["time"].get<double>()>=KeyEpsilon,"Published scale minimum violates native key spacing");
  boundary["keys"][1]["time"]=10;const auto endpoint=ResizeProject(boundary,.1234567,"scale");
  Require(endpoint["keys"][1]["time"]==endpoint["duration"],"Scaled endpoint key exceeds composition due to rounding");
}
void OpenTimeline(Fixture& f){
  f.state["project"]=cs2_live::ToJson(cs2_live::ReadProject(f.state["project"]));
  f.Tool("Camera");f.Tool("Timeline");f.Step();f.Step();f.commands.clear();f.wire_commands.clear();
}
void DragTimeline(Fixture& f,const char* label,float dx,bool shift=false,bool escape=false){
  const auto origin=f.Find(label,0,TimelineWindow).bounds.GetCenter();
  if(shift){ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift,true);f.Step();}
  f.Mouse(origin);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(origin.x+dx,origin.y));f.Step();
  Require(f.commands.empty(),"Timeline emitted an edit before mouse release");
  if(escape)f.Key(ImGuiKey_Escape);
  ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  if(shift){ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift,false);f.Step();}
}
void KeyTimingAndViewport(float height){
  Fixture f(height);OpenTimeline(f);const auto original=f.state["project"]["keys"];
  const auto start=f.Find("##composition_key_Position_0",0,TimelineWindow).bounds.GetCenter();
  const auto end=f.Find("##composition_end",0,TimelineWindow).bounds.GetCenter();
  const double pixels=(end.x-start.x)/8.;
  const auto key_start=f.Find("##composition_key_Position_1",0,TimelineWindow).bounds.GetCenter();
  DragTimeline(f,"##composition_key_Position_1",float(38*f.scale));
  const double pointer_delta=std::floor(key_start.x+38*f.scale)-std::floor(key_start.x);
  const double expected=cs2_live_ui::timeline_time::Snap(2.5+pointer_delta/pixels,"tick",64,60);
  Near(f.One("update_key","time"),expected,"compound key drag");
  for(int i=0;i<4;++i){Json actual=f.state["project"]["keys"][i];actual["time"]=original[i]["time"];Require(actual==original[i],"Key drag changed another channel property");}
  f.commands.clear();DragTimeline(f,"##composition_key_Rotation_1",0);Require(f.commands.empty(),"Select/no-op created a history edit");
  DragTimeline(f,"##composition_key_FOV_1",35*f.scale,false,true);Require(f.commands.empty(),"Escape committed a key drag");
  const auto anchor=f.Find("##composition_key_Position_1",0,TimelineWindow).bounds.GetCenter();f.Mouse(anchor);ImGui::GetIO().AddMouseWheelEvent(0,1);f.Step();
  Require(std::abs(f.Find("##composition_key_Position_1",0,TimelineWindow).bounds.GetCenter().x-anchor.x)<1,"Wheel zoom moved its pointer anchor by over one pixel");Require(f.commands.empty(),"Zoom edited project");
  const auto canvas=f.Find("##camera_composition_canvas",0,TimelineWindow).bounds;f.Mouse(canvas.GetCenter());ImGui::GetIO().AddMouseButtonEvent(2,true);f.Step();f.Mouse(ImVec2(canvas.GetCenter().x-50*f.scale,canvas.GetCenter().y));f.Step();ImGui::GetIO().AddMouseButtonEvent(2,false);f.Step();
  Require(f.Find("##composition_key_Position_1",0,TimelineWindow).bounds.GetCenter().x<anchor.x-25*f.scale,"MMB did not pan ruler");Require(f.commands.empty(),"Pan edited project");
  f.Click("Fit composition",0,TimelineWindow);Require(f.commands.empty(),"Fit edited project");
}
void DurationAndRegion(float height){
  Fixture f(height);f.state["editor_preferences"]["resize_mode"]="scale";f.state["project"]["render_range"]={{"mode","region"},{"in",1.},{"out",6.}};OpenTimeline(f);
  const auto original=f.state["project"];DragTimeline(f,"##composition_end",60*f.scale);
  Require(f.commands.size()==1&&f.commands[0].method=="set_project","End resize was not one atomic project edit");
  Require(f.wire_commands.back().method=="guarded_mutation","Full timeline resize bypassed native revision CAS");
  const double ratio=f.state["project"]["duration"].get<double>()/8.;Require(ratio>1,"End handle did not extend composition");
  for(int i=0;i<4;++i){Json expected=original["keys"][i];expected["time"]=expected["time"].get<double>()*ratio;Require(f.state["project"]["keys"][i]==expected,"Scale changed a key control or failed to retime");}
  Near(f.state["project"]["render_range"]["in"],ratio,"end drag scaled In");Near(f.state["project"]["render_range"]["out"],6*ratio,"end drag scaled Out");
  f.commands.clear();f.wire_commands.clear();DragTimeline(f,"##composition_in",25*f.scale);Require(f.commands.size()==1,"Region trim emitted multiple history edits");Require(f.commands[0].arguments.contains("render_range")&&!f.commands[0].arguments.contains("keys"),"Region handle changed camera keys");
}
void KeepScrubAndPreferences(){
  Fixture f;OpenTimeline(f);const auto original=f.state["project"]["keys"];
  DragTimeline(f,"##composition_end",-120*f.scale);Near(f.One("set_project","duration"),7.5,"keep end lower bound");Require(f.state["project"]["keys"]==original,"Keep resize retimed keys");
  f.commands.clear();const auto canvas=f.Find("##camera_composition_canvas",0,TimelineWindow).bounds;
  const auto zero=f.Find("##composition_key_Position_0",0,TimelineWindow).bounds.GetCenter();const auto end=f.Find("##composition_end",0,TimelineWindow).bounds.GetCenter();
  f.ClickAt(ImVec2(zero.x+(end.x-zero.x)*.4f,canvas.Min.y+20*f.scale));Near(f.One("scrub","time"),3.,"ruler scrub");
  f.commands.clear();f.Click("Rotation",0,TimelineWindow);Require(f.commands.size()==1&&f.commands[0].method=="set_editor_preferences"&&f.commands[0].arguments["timeline_rotation"]==false,"Track visibility was not an editor preference");Require(f.state["project"]["keys"]==original,"Lane preference mutated project");
}
void SnapAndStale(){
  for(const char* mode:{"frame","off"})for(bool bypass:{false,true}){
    Fixture f;f.state["editor_preferences"]["snap_mode"]=mode;f.state["editor_preferences"]["frame_rate"]=120;OpenTimeline(f);
    const auto zero=f.Find("##composition_key_Position_0",0,TimelineWindow).bounds.GetCenter(),end=f.Find("##composition_end",0,TimelineWindow).bounds.GetCenter();
    const auto point=f.Find("##composition_key_Position_1",0,TimelineWindow).bounds.GetCenter();const double dx=std::floor(point.x+29.5)-std::floor(point.x);
    DragTimeline(f,"##composition_key_Position_1",29.5,bypass);
    const double expected=cs2_live_ui::timeline_time::Snap(2.5+dx/((end.x-zero.x)/8),mode,64,120,bypass);Near(f.One("update_key","time"),expected,"frame/off/Shift key timing");
  }
  Fixture f;OpenTimeline(f);auto point=f.Find("##composition_key_Position_1",0,TimelineWindow).bounds.GetCenter();
  f.Mouse(point);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(point.x+35,point.y));f.Step();
  f.state["project"]["keys"][2]["fov"]=81;f.state["revision"]=f.state["revision"].get<int>()+1;f.Step();ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  Require(f.commands.empty(),"Stale timeline gesture overwrote a newer external project edit");Near(f.state["project"]["keys"][2]["fov"],81,"external key edit preserved");
  const float previous_height=f.Find("##camera_composition_canvas",0,TimelineWindow).bounds.GetHeight();
  f.state["editor_preferences"]["timeline_height"]=420;f.Step();f.Step();
  const auto canvas=f.Find("##camera_composition_canvas",0,TimelineWindow).bounds;
  auto* workspace=TestWindow("##CS2CameraWorkspace");auto* child=TestWindow(TimelineWindow);
  Require(workspace&&child&&(child->Flags&ImGuiWindowFlags_ChildWindow),"Composition is not hosted in the shared workspace");
  Require(canvas.GetHeight()>=previous_height,"Larger workspace preference shrank the composition canvas");
  Require(canvas.Min.y>=child->InnerClipRect.Min.y&&canvas.Max.y<=child->InnerClipRect.Max.y+1,
    "Embedded canvas exceeds the available child height");
  Require(child->Pos.y+child->Size.y<=workspace->Pos.y+workspace->Size.y+1,"Composition child exceeds its workspace");
}
void FractionalRegionEdge(){
  Fixture f(720);f.state["project"]["render_range"]={{"mode","region"},{"in",0.},{"out",6.}};OpenTimeline(f);
  const auto keys=f.state["project"]["keys"];
  const auto start=f.Find("##composition_in",0,TimelineWindow).bounds.GetCenter();
  Require(start.x-std::floor(start.x)>.1,"720p region-edge regression did not exercise a fractional marker");
  DragTimeline(f,"##composition_in",30*f.scale);
  Require(f.commands.size()==1&&f.commands[0].method=="set_project"&&f.commands[0].arguments["render_range"]["in"]>0,
    "Floored pointer missed the visible In=0 marker at the left ruler edge");
  Require(f.state["project"]["keys"]==keys,"Region edge changed camera keys");
}
void PendingDurationGrid(){
  Fixture f;OpenTimeline(f);const auto keys=f.state["project"]["keys"];
  f.Click("Duration##composition_duration",0,TimelineWindow);ImGui::GetIO().AddInputCharactersUTF8("10");f.Step();Require(f.commands.empty(),"Duration typing committed before Enter");
  f.state["editor_preferences"]["time_unit"]="frames";f.state["editor_preferences"]["frame_rate"]=120;f.state["editor_preferences"]["resize_mode"]="scale";f.Step();f.Key(ImGuiKey_Enter);
  Near(f.One("set_project","duration"),10,"typed duration retained original seconds");Require(f.state["project"]["keys"]==keys,"Remote resize preference changed a pending Keep edit into Scale");
  f.state["editor_preferences"]["frame_rate"]=60;f.state["editor_preferences"]["resize_mode"]="keep";f.Step();f.commands.clear();f.wire_commands.clear();
  const auto origin=f.Find("Duration##composition_duration",0,TimelineWindow).bounds.GetCenter();f.Mouse(origin);ImGui::GetIO().AddMouseButtonEvent(2,true);f.Step();f.Mouse(ImVec2(origin.x+8,origin.y));f.Step();Require(f.commands.empty(),"Middle duration scrub committed early");
  f.state["editor_preferences"]["frame_rate"]=120;f.state["editor_preferences"]["resize_mode"]="scale";f.Step();ImGui::GetIO().AddMouseButtonEvent(2,false);f.Step();
  Near(f.One("set_project","duration"),10+2./60,"middle duration retained its 60 FPS grid");Require(f.state["project"]["keys"]==keys,"Remote Scale mode changed a pending middle Keep edit");
}
}
int main(){try{TimelineMath();KeyTimingAndViewport(1080);KeyTimingAndViewport(720);DurationAndRegion(1080);DurationAndRegion(720);KeepScrubAndPreferences();SnapAndStale();FractionalRegionEdge();PendingDurationGrid();std::cout<<"PASS: embedded composition timeline units, timing, anchored zoom/pan, keep/scale, range/edge hits, bounded height, snapping, stale/no-op/cancel\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
