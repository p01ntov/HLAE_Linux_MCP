#pragma once
#include "live_controller.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <string>

namespace cs2_live_ui {
namespace camera_curve {
using Json=nlohmann::json;
using Vector=std::array<double,3>;
inline const char* Prefix(int channel){return channel==0?"position":channel==1?"rotation":"fov";}
inline const char* ModeField(int channel){return channel==0?"interpolation":channel==1?"rotation_interpolation":"fov_interpolation";}
inline std::string Field(int channel,const char* suffix){return std::string(Prefix(channel))+suffix;}
inline Json* Find(Json& project,const std::string& id){for(auto& key:project["keys"])if(key.value("id",std::string())==id)return &key;return nullptr;}
inline std::size_t Index(const cs2_live::Project& project,const std::string& id){for(std::size_t i=0;i<project.keys.size();++i)if(project.keys[i].id==id)return i;return project.keys.size();}
inline std::array<Vector,4> Controls(const cs2_live::Project& project,std::size_t left,int channel){
  if(channel==0)return cs2_live::PositionControls(project,left);
  if(channel==1)return cs2_live::RotationControls(project,left);
  const auto lens=cs2_live::FovControls(project,left);return {{{lens[0],0,0},{lens[1],0,0},{lens[2],0,0},{lens[3],0,0}}};
}
inline Vector Value(const cs2_live::Project& project,std::size_t key,int channel){
  if(channel==0)return project.keys[key].pose.position;
  if(channel==2)return {project.keys[key].pose.fov,0,0};
  if(project.keys.size()<2)return project.keys[key].pose.angles;
  return key?Controls(project,key-1,channel)[3]:Controls(project,0,channel)[0];
}
inline double SampleValue(const cs2_live::Project& project,double time,int channel,int axis){
  const auto pose=cs2_live::Sample(project,time);
  if(channel==0)return pose.position[axis];
  if(channel==2)return pose.fov;
  double value=pose.angles[axis];
  if(axis && project.rotationInterpolation!="bezier"){
    // Display equivalent yaw/roll on one continuous branch. This is still the
    // native SLERP sample, not an Euler cubic approximation of the rotation.
    const auto& values=cs2_live::UnwrappedRotations(project);double reference=values.front()[axis];
    if(time>=project.keys.back().time)reference=values.back()[axis];
    else if(time>project.keys.front().time){const auto next=std::upper_bound(project.keys.begin(),project.keys.end(),time,[](double t,const auto& key){return t<key.time;});const auto right=std::size_t(next-project.keys.begin()),left=right-1;const double u=(time-project.keys[left].time)/(project.keys[right].time-project.keys[left].time);reference=values[left][axis]+u*(values[right][axis]-values[left][axis]);}
    value=reference+cs2_live::AngleDelta(reference,value);
  }
  return value;
}
inline Vector Offset(const cs2_live::Project& project,std::size_t key,int channel,bool incoming){
  Vector result{};if((incoming&&key==0)||(!incoming&&key+1>=project.keys.size()))return result;
  const auto controls=Controls(project,incoming?key-1:key,channel);
  for(int axis=0;axis<3;++axis)result[axis]=incoming?controls[2][axis]-controls[3][axis]:controls[1][axis]-controls[0][axis];
  return result;
}
inline void Store(Json& key,int channel,bool incoming,const Vector& offset){
  const auto field=Field(channel,incoming?"_in":"_out");if(channel==2)key[field]=offset[0];else key[field]=offset;
}
inline Json Patch(const Json& project,int channel){return {{"keys",project.at("keys")},{ModeField(channel),"bezier"}};}
inline bool Changed(const Json& source,const Json& patch){for(auto it=patch.begin();it!=patch.end();++it)if(!source.contains(it.key())||source[it.key()]!=it.value())return true;return false;}
inline Json Automatic(Json project,const std::string& id,int channel){
  if(auto* key=Find(project,id)){(*key)[Field(channel,"_in")]=nullptr;(*key)[Field(channel,"_out")]=nullptr;}
  return Patch(project,channel);
}
// Explicit whole-pose repair: auto spatial/value tangents and linear timing on
// both sides of the selected key. Adjacent spans may have different durations.
inline Json SmoothThrough(Json project,const std::string& id){
  const auto parsed=cs2_live::ReadProject(project);const auto i=Index(parsed,id);
  if(i==parsed.keys.size())return Json::object();
  auto* key=Find(project,id);
  for(int channel=0;channel<3;++channel){
    (*key)[Field(channel,"_in")]=nullptr;(*key)[Field(channel,"_out")]=nullptr;
    (*key)[Field(channel,"_easing")]=Json::array({0,0,1,1});
    if(i)if(auto* previous=Find(project,parsed.keys[i-1].id))(*previous)[Field(channel,"_easing")]=Json::array({0,0,1,1});
  }
  return {{"keys",project["keys"]},{"interpolation","bezier"},{"rotation_interpolation","bezier"},{"fov_interpolation","bezier"}};
}
// 0 Auto, 1 Aligned (equal endpoint velocities), 2 Free. This is a UI state,
// derived from values, never a new portable-project property.
inline int TangentMode(const Json& raw,const cs2_live::Project& project,std::size_t i,int channel){
  const Json* key=nullptr;for(const auto& candidate:raw.at("keys"))if(candidate.at("id")==project.keys[i].id)key=&candidate;
  if(!key)return 0;
  const auto in=Field(channel,"_in"),out=Field(channel,"_out");
  if((!key->contains(in)||(*key)[in].is_null())&&(!key->contains(out)||(*key)[out].is_null()))return 0;
  if(!i||i+1>=project.keys.size())return 2;
  const auto a=Offset(project,i,channel,true),b=Offset(project,i,channel,false);
  const double before=project.keys[i].time-project.keys[i-1].time,after=project.keys[i+1].time-project.keys[i].time;
  for(int axis=0;axis<(channel==2?1:3);++axis)if(std::abs(a[axis]/before+b[axis]/after)>1e-5*std::max({1.,std::abs(a[axis]/before),std::abs(b[axis]/after)}))return 2;
  return 1;
}
inline std::pair<double,double> Bounds(int channel,int axis){return channel==2?std::make_pair(1.,179.):channel==1&&axis==0?std::make_pair(-90.,90.):std::make_pair(-1000000.,1000000.);}
inline std::pair<double,double> TimeBounds(const cs2_live::Project& project,std::size_t index){
  constexpr double minimum_span=.000001; // Native Normalize accepts >= 1 us.
  const double current=project.keys[index].time;
  double low=0,high=project.duration;
  if(index){const double previous=project.keys[index-1].time;low=previous+minimum_span;
    if(low-previous<minimum_span)low=std::nextafter(low,std::numeric_limits<double>::infinity());
    low=std::min(low,current);
  }
  if(index+1<project.keys.size()){const double next=project.keys[index+1].time;high=next-minimum_span;
    if(next-high<minimum_span)high=std::nextafter(high,-std::numeric_limits<double>::infinity());
    high=std::max(high,current);
  }
  // A valid key always belongs to its interval, including a zero-width range.
  // Tight neighboring keys lock horizontal dragging while value edits work.
  return low<=high?std::make_pair(low,high):std::make_pair(current,current);
}
inline Json EditHandle(const Json& source,const std::string& id,int channel,bool incoming,Vector offset,int mode,int first_axis=0,int axis_count=3){
  Json result=source;const auto original=cs2_live::ReadProject(source);const auto i=Index(original,id);if(i==original.keys.size())return Json::object();
  auto* key=Find(result,id);
  // A dormant handle can be present while its track uses smooth/SLERP/linear.
  // Editing one component must retain its authored, currently hidden siblings.
  const auto preserve=[&](Vector& value,bool side){const auto field=Field(channel,side?"_in":"_out");if(channel!=2&&key->contains(field)&&(*key)[field].is_array())for(int axis=0;axis<3;++axis)if(axis<first_axis||axis>=first_axis+axis_count)value[axis]=(*key)[field][axis].get<double>();};
  preserve(offset,incoming);
  const bool aligned=mode!=2&&i&&i+1<original.keys.size();
  const double ratio=aligned?(incoming?(original.keys[i+1].time-original.keys[i].time)/(original.keys[i].time-original.keys[i-1].time):(original.keys[i].time-original.keys[i-1].time)/(original.keys[i+1].time-original.keys[i].time)):1;
  const auto base=Value(original,i,channel);
  for(int axis=first_axis;axis<first_axis+axis_count;++axis){const auto bounds=Bounds(channel,axis);double low=std::max(-1000000.,bounds.first-base[axis]),high=std::min(1000000.,bounds.second-base[axis]);
    if(aligned){const double opposite_low=low,opposite_high=high;low=std::max(low,-opposite_high/ratio);high=std::min(high,-opposite_low/ratio);}
    offset[axis]=low<=high?std::clamp(offset[axis],low,high):0;
  }
  Store(*key,channel,incoming,offset);
  if(aligned){
    auto opposite=Offset(original,i,channel,!incoming);
    preserve(opposite,!incoming);
    for(int axis=first_axis;axis<first_axis+axis_count;++axis)opposite[axis]=-offset[axis]*ratio;
    Store(*key,channel,!incoming,opposite);
  }
  return Patch(result,channel);
}
inline void TestHandle(const std::string& label,ImVec2 point,float radius){
#ifdef IMGUI_ENABLE_TEST_ENGINE
  auto& g=*ImGui::GetCurrentContext();const auto id=ImGui::GetID(label.c_str());
  IMGUI_TEST_ENGINE_ITEM_ADD(id,ImRect(ImVec2(point.x-radius,point.y-radius),ImVec2(point.x+radius,point.y+radius)),nullptr);
  IMGUI_TEST_ENGINE_ITEM_INFO(id,label.c_str(),0);
#else
  (void)label;(void)point;(void)radius;
#endif
}
inline void Tooltip(const char* text,const std::function<void()>& backdrop={}){
  ImGui::BeginTooltip();if(backdrop)backdrop();
  ImGui::PushTextWrapPos(ImGui::GetFontSize()*27);ImGui::TextUnformatted(text);ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}
inline void Hint(const char* text,const std::function<void()>& backdrop={}){if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))Tooltip(text,backdrop);}
} // namespace camera_curve

class CameraCurveUi {
 public:
  using Json=nlohmann::json;
  using Send=std::function<void(const std::string&,const Json&)>;
  using Numeric=std::function<bool(const char*,double*,double,double,const char*)>;
  bool visible=false;
  void OpenChannelMenuForDiagnostics(){visible=true;diagnostic_open_channel_=true;}
  void ResetGesture(){drag_={};numeric_target_.clear();}
  void Draw(const Json& state,const Send& send,const std::string& selected,
            const std::function<void(const std::string&)>& select,float scale,const Numeric& numeric,const std::function<void()>& backdrop={},bool embedded=false){
    if(!visible){ResetGesture();return;}
    if(!embedded){
      const auto display=ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(std::min(930.f*scale,display.x*.78f),std::min(620.f*scale,display.y*.8f)),ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowPos(ImVec2(80*scale,60*scale),ImGuiCond_FirstUseEver);
      if(!ImGui::Begin("Camera curves / values",&visible,ImGuiWindowFlags_NoCollapse|(backdrop?ImGuiWindowFlags_NoBackground:0))){ImGui::End();return;}
      if(backdrop)backdrop();
      ImGui::SetWindowFontScale(18*scale/ImGui::GetFont()->FontSize);
    }
    const auto prefs=state.value("editor_preferences",Json::object());
    const bool tips=prefs.is_object()?prefs.value("show_tooltips",true):true,labels=prefs.is_object()?prefs.value("show_key_labels",true):true;
    const std::string unit=prefs.is_object()?prefs.value("time_unit",std::string("seconds")):"seconds";
    const std::string snap_mode=prefs.is_object()?prefs.value("snap_mode",std::string("tick")):"tick";
    const double fps=prefs.is_object()?std::clamp(prefs.value("frame_rate",60.),1.,1000.):60.,tick_rate=std::max(1.,state.value("tick_rate",64.));
    const double time_factor=unit=="ticks"?tick_rate:unit=="frames"?fps:1.;
    auto hint=[&](const char* text){if(tips)camera_curve::Hint(text,backdrop);};
    Json raw=state.value("project",Json::object());cs2_live::Project project;
    try{project=cs2_live::ReadProject(raw);}catch(const std::exception&){ImGui::TextUnformatted("No valid camera project");if(!embedded)ImGui::End();return;}
    const auto document=state.value("project_document_id",std::uint64_t(0));
    if(document!=document_){ResetGesture();document_=document;}
    if(!drag_.id.empty()&&raw!=drag_.source)ResetGesture();
    static const char* names[]={"Position X","Position Y","Position Z","Rotation pitch","Rotation yaw","Rotation roll","Field of view"};
    ImGui::SetNextItemWidth(220*scale);
    if(diagnostic_open_channel_){ImGui::OpenPopupEx(ImHashStr("##ComboPopup",0,ImGui::GetID("##value_channel")),ImGuiPopupFlags_None);diagnostic_open_channel_=false;}
    if(ImGui::BeginCombo("##value_channel",names[component_])){if(backdrop)backdrop();for(int i=0;i<7;++i)if(ImGui::Selectable(names[i],i==component_)){component_=i;ResetGesture();}ImGui::EndCombo();}
    const int channel=component_<3?0:component_<6?1:2,axis=component_<6?component_%3:0;
    const auto index=camera_curve::Index(project,selected);
    ImGui::SameLine();ImGui::TextDisabled("%s",channel==0?"Source units":channel==1?"Degrees (unwrapped)":"Degrees FOV");
    ImGui::SameLine();if(ImGui::Button("Fit values"))fit_=true;
    ImGui::SameLine();ImGui::TextDisabled("%s",raw.value(camera_curve::ModeField(channel),std::string(channel==0?"smooth":channel==1?"slerp":"linear")).c_str());
    const std::string target=selected+"/"+std::to_string(component_);
    if(mode_target_!=target){mode_target_=target;mode_=index<project.keys.size()?camera_curve::TangentMode(raw,project,index,channel):0;}
    ImGui::BeginDisabled(index==project.keys.size());
    if(ImGui::Button("Auto")){const auto patch=camera_curve::Automatic(raw,selected,channel);if(camera_curve::Changed(raw,patch)&&send)send("set_project",patch);mode_=0;numeric_target_.clear();}
    ImGui::SameLine();ImGui::BeginDisabled(index==0||index+1>=project.keys.size());
    if(ImGui::RadioButton("Aligned",&mode_,1)){auto patch=camera_curve::EditHandle(raw,selected,channel,false,camera_curve::Offset(project,index,channel,false),1,0,channel==2?1:3);if(camera_curve::Changed(raw,patch)&&send)send("set_project",patch);numeric_target_.clear();}
    ImGui::EndDisabled();ImGui::SameLine();ImGui::RadioButton("Free",&mode_,2);
    hint("Aligned links shape handles using adjacent key durations. Timing can still change speed. Ctrl-drag changes one handle only.");
    ImGui::SameLine();if(ImGui::Button("Smooth through key")){const auto patch=camera_curve::SmoothThrough(raw,selected);if(camera_curve::Changed(raw,patch)&&send)send("set_project",patch);mode_=0;numeric_target_.clear();}
    hint("Auto handles for position, rotation and FOV; linear timing before and after this key. Enables Bezier for all three tracks, including other segments. One undo restores the previous curves.");
    ImGui::EndDisabled();
    if(index<project.keys.size()){
      const auto base=camera_curve::Value(project,index,channel);const auto in=camera_curve::Offset(project,index,channel,true),out=camera_curve::Offset(project,index,channel,false);
      if(numeric_target_!=target||(!numeric_active_&&drag_.id.empty())){numeric_target_=target;time_=project.keys[index].time;value_=base[axis];in_=base[axis]+in[axis];out_=base[axis]+out[axis];}
      const auto bounds=camera_curve::Bounds(channel,axis);
      const auto time_bounds=camera_curve::TimeBounds(project,index);
      ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,ImVec2(4*scale,2*scale));
      if(ImGui::BeginTable("##curve_values",4,ImGuiTableFlags_SizingStretchSame)){
        const char* labels[]={"Key time","Key value","Incoming value","Outgoing value"};
        for(int col=0;col<4;++col){ImGui::TableNextColumn();ImGui::TextDisabled("%s",labels[col]);ImGui::SetNextItemWidth(-1);
          if(col==0){if(numeric("##curve_time",&time_,time_bounds.first,time_bounds.second,"%.6f s")){if(send)send("update_key",{{"id",selected},{"time",time_}});}}
          else if(col==1){if(numeric("##curve_value",&value_,bounds.first,bounds.second,"%.3f")){
            Json key=*camera_curve::Find(raw,selected);if(channel==2)key["fov"]=value_;else key[channel==0?"position":"angles"][axis]=value_;
            if(send)send("update_key",{{"id",selected},{channel==0?"position":channel==1?"angles":"fov",key[channel==0?"position":channel==1?"angles":"fov"]}});
          }}else{const bool incoming=col==2;ImGui::BeginDisabled(incoming?index==0:index+1>=project.keys.size());double* edit=incoming?&in_:&out_;
            if(numeric(incoming?"##curve_in":"##curve_out",edit,bounds.first,bounds.second,"%.3f")){
              auto offset=incoming?in:out;offset[axis]=*edit-base[axis];auto patch=camera_curve::EditHandle(raw,selected,channel,incoming,offset,mode_,axis,1);if(camera_curve::Changed(raw,patch)&&send)send("set_project",patch);
            }ImGui::EndDisabled();}
        }ImGui::EndTable();
      }ImGui::PopStyleVar();numeric_active_=ImGui::IsAnyItemActive();
    }else ImGui::TextDisabled("Select a key in the graph or timeline");
    const ImVec2 origin=ImGui::GetCursorScreenPos();const ImVec2 size(std::max(240.f*scale,ImGui::GetContentRegionAvail().x),std::max(120.f*scale,ImGui::GetContentRegionAvail().y-32*scale));
    ImGui::InvisibleButton("##value_curve_canvas",size);const bool hover=ImGui::IsItemHovered();const auto mouse=ImGui::GetIO().MousePos;
    if(!drag_.id.empty()&&!drag_.patch.empty()){raw.update(drag_.patch);try{project=cs2_live::ReadProject(raw);}catch(const std::exception&){ResetGesture();}}
    auto sample=[&](double t){return camera_curve::SampleValue(project,t,channel,axis);};
    if(drag_.id.empty()||fit_){double lo=0,hi=1;bool first=true;auto bound=[&](double v){if(first){lo=hi=v;first=false;}else{lo=std::min(lo,v);hi=std::max(hi,v);}};
      if(!project.keys.empty())for(int n=0;n<=384;++n)bound(sample(project.duration*n/384.));
      if(index<project.keys.size()){const auto base=camera_curve::Value(project,index,channel);bound(base[axis]);if(index)bound(base[axis]+camera_curve::Offset(project,index,channel,true)[axis]);if(index+1<project.keys.size())bound(base[axis]+camera_curve::Offset(project,index,channel,false)[axis]);}
      const double pad=std::max(1.,hi-lo)*.18;low_=lo-pad;high_=hi+pad;fit_=false;
    }
    const ImVec2 a(origin.x+68*scale,origin.y+14*scale),b(origin.x+size.x-14*scale,origin.y+size.y-30*scale);
    auto point=[&](double t,double v){return ImVec2(float(a.x+(b.x-a.x)*t/project.duration),float(b.y-(b.y-a.y)*(v-low_)/(high_-low_)));};
    auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(22,22,22,235));draw->PushClipRect(origin,ImVec2(origin.x+size.x,origin.y+size.y),true);
    for(int n=0;n<=4;++n){const double v=low_+(high_-low_)*n/4.,t=project.duration*n/4.;char text[48];std::snprintf(text,sizeof(text),"%.2f",v);draw->AddText(ImVec2(origin.x+4*scale,point(0,v).y-7*scale),IM_COL32(155,155,155,255),text);draw->AddLine(point(0,v),point(project.duration,v),IM_COL32(100,100,100,50));std::snprintf(text,sizeof(text),unit=="seconds"?"%.2fs":unit=="ticks"?"%.0ftk":"%.0ffr",t*time_factor);draw->AddText(ImVec2(point(t,low_).x-12*scale,b.y+7*scale),IM_COL32(155,155,155,255),text);draw->AddLine(point(t,low_),point(t,high_),IM_COL32(100,100,100,50));}
    if(!project.keys.empty()){ImVec2 previous{};for(int n=0;n<=512;++n){const auto next=point(project.duration*n/512.,sample(project.duration*n/512.));if(n)draw->AddLine(previous,next,IM_COL32(225,225,225,255),2*scale);previous=next;}}
    std::string hit;int kind=0;double nearest=12*scale;auto candidate=[&](const std::string& id,int type,ImVec2 p){double d=std::hypot(mouse.x-p.x,mouse.y-p.y);if(d<nearest){nearest=d;hit=id;kind=type;}};
    if(index<project.keys.size()){
      const auto base=camera_curve::Value(project,index,channel);const auto key_point=point(project.keys[index].time,base[axis]);
      for(bool incoming:{true,false}){if((incoming&&index==0)||(!incoming&&index+1==project.keys.size()))continue;
        const double span=incoming?project.keys[index].time-project.keys[index-1].time:project.keys[index+1].time-project.keys[index].time;
        const auto offset=camera_curve::Offset(project,index,channel,incoming);const auto p=point(project.keys[index].time+(incoming?-1:1)*span/3,base[axis]+offset[axis]);
        draw->AddLine(key_point,p,IM_COL32(180,180,180,180),scale);draw->AddCircleFilled(p,5*scale,IM_COL32(48,48,48,255));draw->AddCircle(p,5*scale,IM_COL32(240,240,240,255),16,1.5f*scale);
        camera_curve::TestHandle(std::string("##curve_handle_")+(incoming?"in_":"out_")+selected,p,8*scale);candidate(selected,incoming?1:2,p);
      }
    }
    for(std::size_t i=0;i<project.keys.size();++i){const auto v=camera_curve::Value(project,i,channel);const auto p=point(project.keys[i].time,v[axis]);draw->AddCircleFilled(p,(project.keys[i].id==selected?6.f:4.f)*scale,IM_COL32(235,235,235,255));camera_curve::TestHandle("##curve_key_"+project.keys[i].id,p,8*scale);candidate(project.keys[i].id,0,p);if(labels){char label[24];std::snprintf(label,sizeof(label),"%02d",int(i)+1);draw->AddText(ImVec2(p.x+8*scale,p.y-18*scale),IM_COL32(180,180,180,255),label);}}
    if(hover&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)&&!hit.empty()){
      drag_.id=hit;drag_.kind=kind;drag_.source=state.at("project");drag_.mouse=mouse;drag_.mode=ImGui::GetIO().KeyCtrl?2:mode_;drag_.index=camera_curve::Index(project,hit);drag_.time=project.keys[drag_.index].time;drag_.value=camera_curve::Value(project,drag_.index,channel)[axis];drag_.offset=camera_curve::Offset(project,drag_.index,channel,kind==1);drag_.low=low_;drag_.high=high_;if(select)select(hit);
    }
    if(!drag_.id.empty()){
      if(ImGui::IsKeyPressed(ImGuiKey_Escape,false)){ResetGesture();ImGui::ClearActiveID();}
      else if(ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        const double dx=mouse.x-drag_.mouse.x,dy=mouse.y-drag_.mouse.y;
        drag_.patch=Json::object();
        if((drag_.kind?std::abs(dy):std::hypot(dx,dy))>3*scale){const auto bounds=camera_curve::Bounds(channel,axis);const double delta=-dy/(b.y-a.y)*(drag_.high-drag_.low);
          if(drag_.kind){auto offset=drag_.offset;offset[axis]=std::clamp(drag_.value+drag_.offset[axis]+delta,bounds.first,bounds.second)-drag_.value;if(offset!=drag_.offset)drag_.patch=camera_curve::EditHandle(drag_.source,drag_.id,channel,drag_.kind==1,offset,drag_.mode,axis,1);}
          else{Json updated=drag_.source;auto* key=camera_curve::Find(updated,drag_.id);const auto original=cs2_live::ReadProject(drag_.source);const auto time_bounds=camera_curve::TimeBounds(original,drag_.index);double time=drag_.time;if(std::abs(dx)>3*scale){time+=dx/(b.x-a.x)*original.duration;const double snap_rate=snap_mode=="frame"?fps:snap_mode=="tick"?tick_rate:0;if(snap_rate>0&&!ImGui::GetIO().KeyShift)time=std::round(time*snap_rate)/snap_rate;}(*key)["time"]=std::clamp(time,time_bounds.first,time_bounds.second);const double value=std::clamp(drag_.value+delta,bounds.first,bounds.second);if(value!=drag_.value){if(channel==2)(*key)["fov"]=value;else (*key)[channel==0?"position":"angles"][axis]=value;}drag_.patch={{"keys",updated["keys"]}};}
        }
      }else{if(!drag_.patch.empty()&&camera_curve::Changed(drag_.source,drag_.patch)&&send)send("set_project",drag_.patch);ResetGesture();}
    }
    draw->PopClipRect();ImGui::TextDisabled("Shape handles; timing is edited separately. Ctrl: free handle | Esc: cancel");
    hint("The line uses the same native camera sampler as preview. Value handles sit at segment thirds; the separate Timing graph remaps elapsed time. Editing a handle enables Bezier for that channel.");
    if(!embedded)ImGui::End();
    if(!visible)ResetGesture();
  }
 private:
  struct Drag{std::string id;int kind=0,mode=0;std::size_t index=0;Json source,patch;ImVec2 mouse{};double time=0,value=0,low=0,high=1;camera_curve::Vector offset{};}drag_;
  int component_=0,mode_=0;bool fit_=true,numeric_active_=false,diagnostic_open_channel_=false;double low_=0,high_=1,time_=0,value_=0,in_=0,out_=0;
  std::string numeric_target_,mode_target_;std::uint64_t document_=std::numeric_limits<std::uint64_t>::max();
};
} // namespace cs2_live_ui
