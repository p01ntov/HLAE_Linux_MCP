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
  Require(f.commands.size()==1&&f.commands[0].method=="set_project"&&f.wire_commands[0].method=="guarded_mutation","Key retime bypassed atomic guarded project edit");
  Near(f.state["project"]["keys"][1]["time"],expected,"compound key drag");
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
    const double expected=cs2_live_ui::timeline_time::Snap(2.5+dx/((end.x-zero.x)/8),mode,64,120,bypass);
    Require(f.commands.size()==1&&f.commands[0].method=="set_project","Key timing emitted more than one project patch");Near(f.state["project"]["keys"][1]["time"],expected,"frame/off/Shift key timing");
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
void CtrlKey(Fixture& f,const char* id,const char* lane="Position"){
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);f.Step();
  f.Click((std::string("##composition_key_")+lane+"_"+id).c_str(),0,TimelineWindow);
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);f.Step();
}
void CheckGroupChange(const Json& before,const Json& after,const cs2_live_ui::timeline_time::Selection& ids){
  double delta=0;bool first=true;
  for(std::size_t i=0;i<before.size();++i){
    Json expected=before[i];if(ids.count(expected["id"].get<std::string>())){
      const double shift=after[i]["time"].get<double>()-before[i]["time"].get<double>();
      if(first){delta=shift;first=false;}else Require(std::abs(shift-delta)<1e-12,"Group keys did not keep their relative timing");
      expected["time"]=after[i]["time"];
    }
    Require(expected==after[i],"Group retime altered an unselected key or non-time camera property");
  }
}
void GroupMath(){
  using namespace cs2_live_ui::timeline_time;
  Json raw={{"duration",8.},{"keys",Json::array()}};
  for(int i=0;i<4;++i)raw["keys"].push_back({{"id",std::to_string(i)},{"time",i*2.5},{"position",{i,2,3}},{"angles",{0,10,0}},{"fov",80},{"fov_out",3}});
  const auto original=raw;const auto project=cs2_live::ReadProject(raw);
  for(const Selection& ids:{Selection{"1","2"},Selection{"0","2"},Selection{"1","3"},Selection{"0","1","2","3"}}){
    for(double request:{-86400.,-2.5,-.123,0.,.123,2.5,86400.}){
      const double delta=GroupDelta(project,ids,request);const auto patch=RetimeProject(raw,ids,delta);
      CheckGroupChange(raw["keys"],patch["keys"],ids);Json candidate=raw;candidate.update(patch);const auto result=cs2_live::ReadProject(candidate);
      for(std::size_t i=0;i<result.keys.size();++i)Require(result.keys[i].id==project.keys[i].id,"Group crossed an unselected key");
    }
  }
  Require(raw==original,"Group patch mutated its source");
  bool rejected=false;try{RetimeProject(raw,{"ghost"},0);}catch(const std::exception&){rejected=true;}Require(rejected,"Group patch accepted a ghost key");
  rejected=false;try{RetimeProject(raw,{"1"},4);}catch(const std::exception&){rejected=true;}Require(rejected,"Group patch crossed a stationary key");
  // Exercise representable boundaries and extremely tight adjacent selection.
  for(double offset:{0.,.1,1.,1000.,80000.}){
    Json tight=raw;tight["duration"]=86400.;tight["keys"]=Json::array();double time=offset;
    for(int i=0;i<3;++i){if(i){const double prior=time;time+=KeyEpsilon;while(time-prior<KeyEpsilon)time=std::nextafter(time,std::numeric_limits<double>::infinity());}
      tight["keys"].push_back({{"id",std::to_string(i)},{"time",time},{"position",{0,0,0}},{"angles",{0,0,0}}});}
    const auto parsed=cs2_live::ReadProject(tight);
    for(const Selection& ids:{Selection{"1"},Selection{"0","1"},Selection{"0","1","2"}})for(double requested:{-86400.,-.1,1e-5,.1,86400.}){
      const auto delta=GroupDelta(parsed,ids,requested);Require(GroupShiftFits(parsed,ids,delta),"Rounded group boundary violates native 1us key spacing");
      const auto patch=RetimeProject(tight,ids,delta);CheckGroupChange(tight["keys"],patch["keys"],ids);
    }
  }
}
void MultiSelectionBounds(float height){
  Fixture f(height);OpenTimeline(f);const auto original=f.state["project"]["keys"];
  f.Click("##composition_key_Position_1",0,TimelineWindow);CtrlKey(f,"2","Rotation");
  Require(f.commands.empty()&&f.DrawText().find("2 selected")!=std::string::npos,"Ctrl click did not select linked keys without an edit");
  DragTimeline(f,"##composition_key_FOV_1",1800*f.scale);
  Require(f.commands.size()==1&&f.commands[0].method=="set_project"&&f.wire_commands.back().method=="guarded_mutation","Group drag was not one guarded project edit");
  CheckGroupChange(original,f.state["project"]["keys"],{"1","2"});cs2_live::ReadProject(f.state["project"]);
  Require(f.state["project"]["keys"][2]["time"].get<double>()<7.5&&f.state["project"]["keys"][2]["time"].get<double>()>7.499,"Group did not stop at the nearest unselected key");
  f.commands.clear();f.wire_commands.clear();const auto positive=f.state["project"]["keys"];
  DragTimeline(f,"##composition_key_Position_1",-1800*f.scale);
  Require(f.commands.size()==1,"Left group clamp emitted multiple edits");CheckGroupChange(positive,f.state["project"]["keys"],{"1","2"});cs2_live::ReadProject(f.state["project"]);
  Require(f.state["project"]["keys"][1]["time"].get<double>()>=1e-6&&f.state["project"]["keys"][1]["time"].get<double>()<.001,"Group crossed the unselected first key");
  f.commands.clear();CtrlKey(f,"0");CtrlKey(f,"3");Require(f.DrawText().find("4 selected")!=std::string::npos,"All linked keys could not be selected");
  const auto before_all=f.state["project"]["keys"];DragTimeline(f,"##composition_key_Rotation_2",1800*f.scale);
  CheckGroupChange(before_all,f.state["project"]["keys"],{"0","1","2","3"});Near(f.state["project"]["keys"][3]["time"],8,"All-key duration boundary");
  f.commands.clear();const auto before_zero=f.state["project"]["keys"];DragTimeline(f,"##composition_key_Rotation_2",-1800*f.scale);
  CheckGroupChange(before_zero,f.state["project"]["keys"],{"0","1","2","3"});Near(f.state["project"]["keys"][0]["time"],0,"All-key zero boundary");
}
void MarqueeSelection(float height){
  Fixture f(height);f.state["editor_preferences"]["timeline_position"]=false;f.state["editor_preferences"]["timeline_fov"]=false;OpenTimeline(f);
  auto a=f.Find("##composition_key_Rotation_1",0,TimelineWindow).bounds.GetCenter(),b=f.Find("##composition_key_Rotation_2",0,TimelineWindow).bounds.GetCenter();
  const ImVec2 begin(a.x-18*f.scale,a.y-18*f.scale),end(b.x+18*f.scale,b.y+18*f.scale);
  f.Mouse(begin);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(end);f.Step();
  Require(f.commands.empty(),"Marquee preview changed the project");ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  Require(f.DrawText().find("2 selected")!=std::string::npos,"Visible-lane marquee did not select exactly two linked keys");
  const auto original=f.state["project"]["keys"];DragTimeline(f,"##composition_key_Rotation_1",30*f.scale);CheckGroupChange(original,f.state["project"]["keys"],{"1","2"});
  f.commands.clear();CtrlKey(f,"2","Rotation");Require(f.DrawText().find("1 selected")!=std::string::npos,"Ctrl click did not remove exactly one key");
  CtrlKey(f,"1","Rotation");Require(f.DrawText().find("0 selected")!=std::string::npos,"Removing the final selection left a ghost key");
  Require(f.commands.empty(),"Selection toggles edited the project");
  CtrlKey(f,"0","Rotation");ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);f.Step();
  a=f.Find("##composition_key_Rotation_1",0,TimelineWindow).bounds.GetCenter();b=f.Find("##composition_key_Rotation_2",0,TimelineWindow).bounds.GetCenter();
  f.Mouse(ImVec2(a.x-18*f.scale,a.y-18*f.scale));ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(b.x+18*f.scale,b.y+18*f.scale));ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);f.Step();Require(f.DrawText().find("3 selected")!=std::string::npos,"Ctrl marquee did not preserve the previous selection");
  f.ClickAt(ImVec2(begin.x,begin.y));Require(f.DrawText().find("0 selected")!=std::string::npos,"Empty lane click did not clear the selection");
  // A cancelled marquee must preserve the existing selection.
  f.Mouse(begin);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(end);f.Step();f.Key(ImGuiKey_Escape);ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  Require(f.commands.empty()&&f.DrawText().find("0 selected")!=std::string::npos,"Escape applied a marquee draft");
  f.state["editor_preferences"]["timeline_rotation"]=false;f.Step();
  for(const auto& item:f.submitted.items)Require(item.second.label.rfind("##composition_key_",0)!=0,"Hidden lanes kept submitting phantom key targets");
  f.Click("Fit selection",0,TimelineWindow);Require(f.commands.empty(),"Empty fit edited the project");
}
void GroupCancellationAndNoOp(){
  for(const std::string reason:{"revision","source","document","lanes","invalid","escape"}){
    Fixture f;OpenTimeline(f);f.Click("##composition_key_Position_1",0,TimelineWindow);CtrlKey(f,"2");const auto original=f.state["project"];
    auto point=f.Find("##composition_key_Rotation_1",0,TimelineWindow).bounds.GetCenter();f.Mouse(point);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(point.x+45,point.y));f.Step();
    if(reason=="revision")f.state["revision"]=f.state["revision"].get<int>()+1;
    else if(reason=="source")f.state["project"]["keys"][3]["fov"]=82;
    else if(reason=="document")f.state["project_document_id"]=73;
    else if(reason=="lanes")f.state["editor_preferences"]["timeline_rotation"]=false;
    else if(reason=="invalid")f.state["project"]["keys"]="invalid";
    else f.Key(ImGuiKey_Escape);
    f.Step();ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();Require(f.commands.empty(),reason+" change committed a stale group drag");
    if(reason=="invalid"){f.state["project"]=original;f.Step();Require(f.commands.empty(),"Restored valid project resumed an invalidated gesture");}
  }
  Fixture f;f.state["project"]["keys"][1]["time"]=2.503;OpenTimeline(f);f.Click("##composition_key_Position_1",0,TimelineWindow);CtrlKey(f,"2");
  DragTimeline(f,"##composition_key_FOV_1",2.5);Require(f.commands.empty(),"Subthreshold group click snapped a key");
  const auto point=f.Find("##composition_key_Rotation_1",0,TimelineWindow).bounds.GetCenter();f.Mouse(point);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(point.x+40,point.y));f.Step();f.Mouse(point);f.Step();ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  Require(f.commands.empty(),"Returning a group to its press point created an undo edit");
  f.state["project"]["keys"].erase(2);f.Step();Require(f.DrawText().find("1 selected")!=std::string::npos,"Removed key left a ghost multiselection");
  f.state["project_document_id"]=72;f.Step();Require(f.DrawText().find("1 selected")!=std::string::npos,"New document inherited an old group selection");
  CtrlKey(f,"3");f.state["project"]["keys"].erase(2);f.Step();Require(f.DrawText().find("1 selected")!=std::string::npos,"Deleting the primary key lost the surviving selected member");
  const auto before_release=f.state["project"]["keys"];const auto release_point=f.Find("##composition_key_Position_1",0,TimelineWindow).bounds.GetCenter();
  f.Mouse(release_point);ImGui::GetIO().AddMouseButtonEvent(0,true);f.Step();f.Mouse(ImVec2(release_point.x+60,release_point.y));ImGui::GetIO().AddMouseButtonEvent(0,false);f.Step();
  Require(f.commands.size()==1&&f.state["project"]["keys"]!=before_release,"Final-frame pointer movement was discarded on release");
}
void SelectionFitAndNavigation(float height){
  Fixture f(height);f.state["sync_demo"]=true;OpenTimeline(f);f.Click("##composition_key_Position_1",0,TimelineWindow);CtrlKey(f,"2");
  const auto* child=TestWindow(TimelineWindow);Require(child!=nullptr,"Timeline child missing");
  for(const char* label:{"Fit composition","Fit selection","Previous key","Next key"}){
    const auto control=f.Find(label,0,TimelineWindow).bounds;Require(control.Max.x<=child->InnerClipRect.Max.x+1&&control.Min.x>=child->InnerClipRect.Min.x,"Timeline navigation overflows the workspace at 720/1080");
  }
  const float before=f.Find("##composition_key_Position_2",0,TimelineWindow).bounds.GetCenter().x-f.Find("##composition_key_Position_1",0,TimelineWindow).bounds.GetCenter().x;
  f.Click("Fit selection",0,TimelineWindow);const auto canvas=f.Find("##camera_composition_canvas",0,TimelineWindow).bounds;
  const auto a=f.Find("##composition_key_Position_1",0,TimelineWindow).bounds.GetCenter(),b=f.Find("##composition_key_Position_2",0,TimelineWindow).bounds.GetCenter();
  Require(b.x-a.x>before*1.5&&canvas.Contains(a)&&canvas.Contains(b),"Fit selection did not frame the selected range");Require(f.commands.empty(),"Fit selection edited project");
  f.Click("Next key",0,TimelineWindow);Near(f.One("scrub","time"),2.5,"Next key from between keys");Require(f.commands[0].arguments["seek_demo"]==true,"Key navigation lost demo synchronization");
  f.commands.clear();f.state["cursor"]=2.5;f.Step();f.Click("Previous key",0,TimelineWindow);Near(f.One("scrub","time"),0,"Previous key from exact time");
  f.commands.clear();f.state["cursor"]=0.;f.Step();f.Click("Previous key",0,TimelineWindow);Require(f.commands.empty(),"Previous before first key emitted redundant scrub");
  f.state["cursor"]=7.5;f.Step();f.Click("Next key",0,TimelineWindow);Require(f.commands.empty(),"Next after final key emitted redundant scrub");
  f.Click("Fit composition",0,TimelineWindow);f.Step();const auto target=f.Find("##composition_key_FOV_2",0,TimelineWindow).bounds.GetCenter();
  f.ClickAt(target);f.ClickAt(target);Require(f.commands.size()==1&&f.commands[0].method=="scrub","Double click did not issue exactly one seek");Near(f.commands[0].arguments["time"],5.,"Double click exact key time");
}
void GuardedGroupUndo(){
  setenv("CS2_CAMERA_LIVE","1",1);Fixture f;const auto source=cs2_live::ToJson(cs2_live::ReadProject(f.state["project"]));cs2_live::Controller controller;
  float origin[3]={0,0,0},angles[3]={0,0,0},fov=90;controller.View(origin,angles,&fov,true);
  auto native=[&](const std::string& method,const Json& args){const auto command=Json{{"method",method},{"params",args}}.dump();controller.EnqueueText(command.c_str());controller.Frame(true,640,true,64,true,"",[](const std::string&){});};
  native("import_project",{{"project",source}});f.state=Json::parse(controller.StateJson());f.Step();OpenTimeline(f);
  std::vector<Command> queued;f.command_sink=[&](const std::string& method,const Json& args){queued.push_back({method,args});};
  f.Click("##composition_key_Position_1",0,TimelineWindow);CtrlKey(f,"2");DragTimeline(f,"##composition_key_Rotation_1",45);
  Require(queued.size()==1&&queued[0].method=="guarded_mutation","Native group edit was not one revision guarded command");native(queued[0].method,queued[0].arguments);
  auto edited=Json::parse(controller.StateJson());Require(edited["project"]!=source,"Native controller rejected a valid group edit");CheckGroupChange(source["keys"],edited["project"]["keys"],{"1","2"});
  native("undo",Json::object());Require(Json::parse(controller.StateJson())["project"]==source,"One undo did not restore the full group");
  native("redo",Json::object());Require(Json::parse(controller.StateJson())["project"]==edited["project"],"Redo did not restore the whole group");
  f.state=Json::parse(controller.StateJson());f.Step();f.commands.clear();queued.clear();DragTimeline(f,"##composition_key_Rotation_1",40);
  Require(queued.size()==1,"Second native group edit missing");native("update_key",{{"id","3"},{"fov",75}});const auto newer=Json::parse(controller.StateJson());
  native(queued[0].method,queued[0].arguments);const auto rejected=Json::parse(controller.StateJson());Require(rejected["project"]==newer["project"]&&rejected["revision"]==newer["revision"],"Delayed group command overwrote a concurrent native change");
}
}
int main(){try{TimelineMath();GroupMath();KeyTimingAndViewport(1080);KeyTimingAndViewport(720);DurationAndRegion(1080);DurationAndRegion(720);KeepScrubAndPreferences();SnapAndStale();FractionalRegionEdge();PendingDurationGrid();MultiSelectionBounds(1080);MultiSelectionBounds(720);MarqueeSelection(1080);MarqueeSelection(720);GroupCancellationAndNoOp();SelectionFitAndNavigation(1080);SelectionFitAndNavigation(720);GuardedGroupUndo();std::cout<<"PASS: composition timing, stable multiselection, marquee, atomic group retime/undo/CAS, boundaries, fit/navigation, 720/1080 layouts, stale/no-op/cancel\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
