#pragma once
#include "live_controller.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <string>

namespace cs2_live_ui {
namespace timeline_time {
using Json=nlohmann::json;
constexpr double KeyEpsilon=0.000001;
inline double Rate(const std::string& unit,double tick_rate,double frame_rate){
  return unit=="ticks"?tick_rate:unit=="frames"?frame_rate:1.;
}
inline double SecondsToDisplay(double seconds,const std::string& unit,double tick_rate,double frame_rate){return seconds*Rate(unit,tick_rate,frame_rate);}
inline double DisplayToSeconds(double value,const std::string& unit,double tick_rate,double frame_rate){return value/Rate(unit,tick_rate,frame_rate);}
inline double Snap(double seconds,const std::string& mode,double tick_rate,double frame_rate,bool bypass=false){
  if(bypass||mode=="off")return seconds;
  const double rate=mode=="frame"?frame_rate:tick_rate;return std::round(seconds*rate)/rate;
}
inline std::pair<double,double> KeyTimeBounds(const cs2_live::Project& project,std::size_t index){
  const auto& keys=project.keys;double low=0,high=project.duration;
  if(index){const double previous=keys[index-1].time;low=previous+KeyEpsilon;while(low-previous<KeyEpsilon)low=std::nextafter(low,std::numeric_limits<double>::infinity());}
  if(index+1<keys.size()){const double next=keys[index+1].time;high=next-KeyEpsilon;while(next-high<KeyEpsilon)high=std::nextafter(high,-std::numeric_limits<double>::infinity());}
  low=std::min(low,keys[index].time);high=std::max(high,keys[index].time);
  if(low>high)return {keys[index].time,keys[index].time};
  return {low,high};
}
inline double ScaledTime(double time,double before,double after){return time==before?after:std::clamp(time*(after/before),0.,after);}
inline bool ScaleFits(const cs2_live::Project& project,double duration){
  for(std::size_t i=1;i<project.keys.size();++i)
    if(ScaledTime(project.keys[i].time,project.duration,duration)-ScaledTime(project.keys[i-1].time,project.duration,duration)<KeyEpsilon)return false;
  return true;
}
inline double MinimumDuration(const cs2_live::Project& project,const std::string& mode){
  double result=.01;
  if(mode=="keep"){if(!project.keys.empty())result=std::max(result,project.keys.back().time);}
  else if(mode=="scale"){
    for(std::size_t i=1;i<project.keys.size();++i){
      const double a=project.keys[i-1].time,b=project.keys[i].time;
      // Multiplication and subtraction round separately. Keep a conservative
      // margin rather than publishing an analytical minimum that native rejects.
      const double gap=(b-a)-4*std::numeric_limits<double>::epsilon()*(std::abs(a)+std::abs(b));
      result=std::max(result,gap>0?project.duration*KeyEpsilon/gap:project.duration);
    }
    result=std::min(result,project.duration);
    if(result<project.duration)result=std::nextafter(result,std::numeric_limits<double>::infinity());
    if(!ScaleFits(project,result))result=project.duration;
  }else throw std::invalid_argument("resize mode must be keep or scale");
  return std::min(result,project.duration); // The unchanged valid project always fits.
}
inline double MinimumDuration(const Json& raw,const std::string& mode){return MinimumDuration(cs2_live::ReadProject(raw),mode);}
inline Json ResizeProject(const Json& raw,double duration,const std::string& mode){
  const auto project=cs2_live::ReadProject(raw);const double minimum=MinimumDuration(project,mode);
  if(duration<minimum&&minimum-duration<=8*std::numeric_limits<double>::epsilon()*std::max(1.,minimum))duration=minimum;
  cs2_live::Finite(duration,minimum,86400,"composition duration");
  Json keys=raw.value("keys",Json::array());auto range=cs2_live::StoredRenderRange(project);
  if(mode=="scale"){
    if(!ScaleFits(project,duration))throw std::invalid_argument("scaled keys need at least 0.000001 seconds between them; increase duration");
    for(auto& key:keys)key["time"]=ScaledTime(key.at("time").get<double>(),project.duration,duration);
    range.in=ScaledTime(range.in,project.duration,duration);range.out=ScaledTime(range.out,project.duration,duration);
    if(range.out<=range.in){
      if(range.out<duration)range.out=std::nextafter(range.in,std::numeric_limits<double>::infinity());
      else range.in=std::nextafter(range.out,0.);
    }
  }else{
    cs2_live::Project resized=project;resized.duration=duration;cs2_live::ResizeRenderRange(resized,project.duration);range=cs2_live::StoredRenderRange(resized);
  }
  Json patch={{"duration",duration},{"keys",keys},{"render_range",{{"mode",range.mode},{"in",range.in},{"out",range.out}}}};
  Json candidate=raw;candidate.update(patch);cs2_live::ReadProject(candidate);return patch;
}
inline bool Changed(const Json& source,const Json& patch){for(auto it=patch.begin();it!=patch.end();++it)if(!source.contains(it.key())||source[it.key()]!=it.value())return true;return false;}
inline double NiceStep(double desired){
  const double power=std::pow(10.,std::floor(std::log10(std::max(1e-12,desired))));
  for(double m:{1.,2.,5.,10.})if(m*power>=desired)return m*power;
  return 10*power;
}
inline void TestItem(const std::string& label,const ImRect& bounds){
#ifdef IMGUI_ENABLE_TEST_ENGINE
  auto& g=*ImGui::GetCurrentContext();const auto id=ImGui::GetID(label.c_str());
  IMGUI_TEST_ENGINE_ITEM_ADD(id,bounds,nullptr);IMGUI_TEST_ENGINE_ITEM_INFO(id,label.c_str(),0);
#else
  (void)label;(void)bounds;
#endif
}
} // namespace timeline_time

// Composition camera timing is independent of the full-demo event timeline.
// Position, rotation and lens markers deliberately share the same key time.
class CameraTimelineUi {
 public:
  using Json=nlohmann::json;
  using Send=std::function<void(const std::string&,const Json&)>;
  using Numeric=std::function<bool(const char*,double*,double,double,const char*)>;
  bool visible=false;
  void ResetGesture(){drag_={};duration_active_=false;fps_active_=false;height_active_=false;}
  void Draw(const Json& state,const Send& send,const std::string& selected,
            const std::function<void(const std::string&)>& select,float scale,const std::function<void()>& backdrop={},const Numeric& numeric={},bool embedded=false){
    if(!visible){ResetGesture();return;}
    const Json prefs=state.value("editor_preferences",Json::object());
    const int preferred_height=std::clamp(prefs.value("timeline_height",240),160,420);
    if(!embedded){
      const auto display=ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(std::min(display.x*.91f,1180*scale),420*scale),ImGuiCond_FirstUseEver);
      if(height_seen_!=preferred_height||scale_seen_!=scale){
        auto* window=ImGui::FindWindowByName("Camera timeline / composition");
        const float width=window?window->Size.x:std::min(display.x*.91f,1180*scale);
        ImGui::SetNextWindowSize(ImVec2(width,std::min(display.y-48*scale,(preferred_height+180)*scale)));
        height_seen_=preferred_height;scale_seen_=scale;
      }
      ImGui::SetNextWindowPos(ImVec2(24*scale,std::max(24*scale,display.y-620*scale)),ImGuiCond_FirstUseEver);
      if(!ImGui::Begin("Camera timeline / composition",&visible,ImGuiWindowFlags_NoCollapse|(backdrop?ImGuiWindowFlags_NoBackground:0))){ImGui::End();return;}
      if(backdrop)backdrop();
      ImGui::SetWindowFontScale(18*scale/ImGui::GetFont()->FontSize);
    }
    const Json raw=state.value("project",Json::object());cs2_live::Project project;
    try{project=cs2_live::ReadProject(raw);}catch(const std::exception&){ImGui::TextUnformatted("No valid camera composition");if(!embedded)ImGui::End();return;}
    const auto document=state.value("project_document_id",std::uint64_t(0));
    if(document!=document_){ResetGesture();document_=document;fitted_=false;}
    if(drag_.kind!=None&&raw!=drag_.source)ResetGesture();
    const auto unit=prefs.value("time_unit",std::string("seconds")),snap=prefs.value("snap_mode",std::string("tick")),resize=prefs.value("resize_mode",std::string("keep"));
    const double tick_rate=std::max(1,state.value("tick_rate",64)),fps=std::clamp(prefs.value("frame_rate",60),1,1000);
    const bool labels=prefs.value("show_key_labels",true),tips=prefs.value("show_tooltips",true);
    auto tooltip=[&](const char* text){ImGui::BeginTooltip();if(backdrop)backdrop();ImGui::PushTextWrapPos(ImGui::GetFontSize()*27);ImGui::TextUnformatted(text);ImGui::PopTextWrapPos();ImGui::EndTooltip();};
    auto preference=[&](const char* field,const Json& value){if(send)send("set_editor_preferences",{{field,value}});};
    auto number=[&](const char* label,double* value,double minimum,double maximum,const char* format){
      if(numeric)return numeric(label,value,minimum,maximum,format);
      ImGui::InputDouble(label,value,0,0,format,ImGuiInputTextFlags_AutoSelectAll);
      return ImGui::IsItemDeactivatedAfterEdit();
    };
    auto combo=[&](const char* label,const char* field,const std::string& current,const std::vector<std::string>& values,float width){
      ImGui::SetNextItemWidth(width*scale);if(ImGui::BeginCombo(label,current.c_str())){if(backdrop)backdrop();for(const auto& value:values)if(ImGui::Selectable(value.c_str(),current==value))preference(field,value);ImGui::EndCombo();}
    };
    // Shared-workspace setup lives in Settings. Standalone tools keep their
    // original setup controls for diagnostics and independent use.
    if(!embedded){
      combo("##composition_units","time_unit",unit,{"seconds","ticks","frames"},95);ImGui::SameLine();
      ImGui::TextDisabled("relative");ImGui::SameLine();if(!fps_active_)fps_draft_=fps;ImGui::SetNextItemWidth(68*scale);
      const bool fps_commit=number("FPS##composition_rate",&fps_draft_,1,1000,"%.0f");
      fps_active_=ImGui::IsItemActive()||(ImGui::GetIO().MouseDown[ImGuiMouseButton_Middle]&&ImGui::IsAnyItemActive());if(fps_commit&&std::llround(fps_draft_)!=int(fps))preference("frame_rate",std::clamp(int(std::llround(fps_draft_)),1,1000));
      ImGui::SameLine();
    }
    ImGui::TextDisabled("Snap");ImGui::SameLine();combo("##composition_snap","snap_mode",snap,{"tick","frame","off"},80);
    ImGui::SameLine();if(ImGui::Button("Fit composition"))fitted_=false;
    if(tips&&ImGui::IsItemHovered())tooltip("Wheel: zoom at pointer. Middle drag: pan. Shift bypasses tick/frame snapping.");
    int height=std::clamp(prefs.value("timeline_height",240),160,420);
    if(!embedded){
      ImGui::SameLine();ImGui::SetNextItemWidth(100*scale);
      const bool height_changed=ImGui::SliderInt("Height##composition_height",&height,160,420);
      // Slider values are previews until release; preserve the local active value.
      if(ImGui::IsItemActive()||height_changed){height_draft_=height;height_active_=true;}else if(height_active_){if(height_draft_!=prefs.value("timeline_height",240))preference("timeline_height",height_draft_);height_active_=false;}
    }
    auto next_control=[&](float width){
      if(!embedded||ImGui::GetCurrentWindow()->WorkRect.Max.x-ImGui::GetItemRectMax().x>=width*scale+ImGui::GetStyle().ItemSpacing.x)ImGui::SameLine();
    };
    const char* lane_fields[]={"timeline_position","timeline_rotation","timeline_fov"};const char* lane_names[]={"Position","Rotation","FOV"};
    bool lanes[3];for(int i=0;i<3;++i){lanes[i]=prefs.value(lane_fields[i],true);if(i)ImGui::SameLine();if(ImGui::Checkbox(lane_names[i],&lanes[i]))preference(lane_fields[i],lanes[i]);}
    next_control(80);combo("##composition_resize","resize_mode",resize,{"keep","scale"},80);
    if(!duration_active_){duration_unit_=unit;duration_resize_=resize;duration_tick_rate_=tick_rate;duration_fps_=fps;duration_draft_=timeline_time::SecondsToDisplay(project.duration,duration_unit_,duration_tick_rate_,duration_fps_);}
    next_control(120+ImGui::CalcTextSize("Duration").x/scale+ImGui::GetStyle().ItemInnerSpacing.x/scale);
    ImGui::SetNextItemWidth(120*scale);const bool duration_commit=number("Duration##composition_duration",&duration_draft_,timeline_time::SecondsToDisplay(timeline_time::MinimumDuration(project,duration_resize_),duration_unit_,duration_tick_rate_,duration_fps_),timeline_time::SecondsToDisplay(86400,duration_unit_,duration_tick_rate_,duration_fps_),"%.4f");
    duration_active_=ImGui::IsItemActive()||(ImGui::GetIO().MouseDown[ImGuiMouseButton_Middle]&&ImGui::IsAnyItemActive());
    if(duration_commit){try{const double seconds=timeline_time::DisplayToSeconds(duration_draft_,duration_unit_,duration_tick_rate_,duration_fps_);const auto patch=timeline_time::ResizeProject(raw,seconds,duration_resize_);if(timeline_time::Changed(raw,patch)&&send)send("set_project",patch);error_.clear();}catch(const std::exception& e){error_=e.what();}}
    if(tips&&ImGui::IsItemHovered())tooltip("Keep preserves every key time and cannot end before the last key. Scale moves all key times and saved In/Out proportionally; camera shape controls stay unchanged.");
    next_control(110);const auto stored=cs2_live::StoredRenderRange(project);ImGui::SetNextItemWidth(110*scale);
    if(ImGui::BeginCombo("##composition_range",stored.mode.c_str())){if(backdrop)backdrop();for(const char* mode:{"composition","region"})if(ImGui::Selectable(mode,stored.mode==mode)&&stored.mode!=mode&&send)send("set_project",{{"render_range",{{"mode",mode}}}});ImGui::EndCombo();}
    if(!error_.empty())ImGui::TextColored(ImVec4(1,.55f,.35f,1),"%s",error_.c_str());
    const float lane_label=86*scale,ruler=28*scale,footer=20*scale;
    const float desired_height=float(height_active_?height_draft_:height)*scale;
    // The shared workspace owns its height and applies the height preference.
    // Small children scroll a minimum usable canvas within their own clip.
    const float canvas_height=embedded?std::max(120*scale,ImGui::GetContentRegionAvail().y-ImGui::GetTextLineHeightWithSpacing()):desired_height;
    const ImVec2 origin=ImGui::GetCursorScreenPos(),size(std::max(240.f*scale,ImGui::GetContentRegionAvail().x),canvas_height);
    if(!fitted_){view_start_=0;view_span_=std::max(.012,project.duration*1.2);fitted_=true;}
    ImGui::InvisibleButton("##camera_composition_canvas",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonMiddle);
    const bool hover=ImGui::IsItemHovered();const auto mouse=ImGui::GetIO().MousePos;
    const double left=origin.x+lane_label,width=std::max(1.,double(size.x-lane_label-12*scale));
    auto time_at=[&](float x){return view_start_+(x-left)/width*view_span_;};
    if(hover&&drag_.kind==None&&ImGui::GetIO().MouseWheel!=0){const double fraction=std::clamp((mouse.x-left)/width,0.,1.),anchor=view_start_+fraction*view_span_;view_span_=std::clamp(view_span_*std::pow(1.25,-ImGui::GetIO().MouseWheel),.00001,172800.);view_start_=std::clamp(anchor-fraction*view_span_,0.,172800.-view_span_);}
    if(drag_.kind==None&&ImGui::IsItemActive()&&ImGui::IsMouseDragging(ImGuiMouseButton_Middle))view_start_=std::clamp(view_start_-ImGui::GetIO().MouseDelta.x/width*view_span_,0.,172800.-view_span_);
    auto x_at=[&](double time){return float(left+(time-view_start_)/view_span_*width);};
    auto* draw=ImGui::GetWindowDrawList();draw->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(22,22,22,235));draw->PushClipRect(origin,ImVec2(origin.x+size.x,origin.y+size.y),true);
    const int count=int(lanes[0])+int(lanes[1])+int(lanes[2]);const float lane_height=(size.y-ruler-footer)/std::max(1,count);
    const double unit_rate=timeline_time::Rate(unit,tick_rate,fps),step=timeline_time::NiceStep(view_span_*unit_rate/std::max(1.,width/(82*scale)));
    for(double d=std::ceil(view_start_*unit_rate/step)*step;d<=(view_start_+view_span_)*unit_rate;d+=step){const float x=x_at(d/unit_rate);char label[64];std::snprintf(label,sizeof(label),"%.5g",d);draw->AddLine(ImVec2(x,origin.y+21*scale),ImVec2(x,origin.y+size.y-footer),IM_COL32(145,145,145,45));draw->AddText(ImVec2(x+3*scale,origin.y+3*scale),IM_COL32(190,190,190,255),label);}
    draw->PushClipRect(ImVec2(float(left),origin.y),ImVec2(float(left+width),origin.y+size.y),true);
    const double cursor=drag_.kind==Scrub?drag_.time:state.value("cursor",0.);
    double end=drag_.kind==End?drag_.time:project.duration,region_in=stored.in,region_out=stored.out;
    if(drag_.kind==End&&drag_.resize=="scale"){region_in*=end/project.duration;region_out*=end/project.duration;}
    if(drag_.kind==In)region_in=drag_.time;
    if(drag_.kind==Out)region_out=drag_.time;
    draw->AddRectFilled(ImVec2(float(std::clamp(double(x_at(end)),left,left+width)),origin.y+ruler),ImVec2(float(left+width),origin.y+size.y-footer),IM_COL32(0,0,0,115));
    if(stored.mode=="region")draw->AddRectFilled(ImVec2(x_at(region_in),origin.y+size.y-footer),ImVec2(x_at(region_out),origin.y+size.y),IM_COL32(165,165,165,90));
    std::string hit;int hit_kind=None;std::size_t hit_index=0;double best=10*scale;
    auto hitpoint=[&](const std::string& label,float x,float y,int kind,const std::string& id={},std::size_t index=0){
      timeline_time::TestItem(label,ImRect(ImVec2(x-7*scale,y-7*scale),ImVec2(x+7*scale,y+7*scale)));
      const double distance=std::hypot(mouse.x-x,mouse.y-y);if(distance<best){best=distance;hit=id;hit_kind=kind;hit_index=index;}
    };
    int row=0;for(int lane=0;lane<3;++lane){if(!lanes[lane])continue;const float y=origin.y+ruler+lane_height*(row+.5f);++row;
      draw->AddLine(ImVec2(float(left),y),ImVec2(float(left+width),y),IM_COL32(125,125,125,55));
      for(std::size_t i=0;i<project.keys.size();++i){const auto& key=project.keys[i];double time=drag_.kind==Key&&drag_.id==key.id?drag_.time:key.time;if(drag_.kind==End&&drag_.resize=="scale")time*=end/project.duration;const float x=x_at(time);if(x<left-10*scale||x>left+width+10*scale)continue;
        const bool chosen=key.id==selected;const auto color=chosen?IM_COL32(255,220,135,255):IM_COL32(220,220,220,255);const float r=(chosen?6.f:4.f)*scale;
        draw->AddQuadFilled(ImVec2(x,y-r),ImVec2(x+r,y),ImVec2(x,y+r),ImVec2(x-r,y),color);
        hitpoint(std::string("##composition_key_")+lane_names[lane]+"_"+key.id,x,y,Key,key.id,i);
        if(labels&&(chosen||project.keys.size()<24)){char label[32];std::snprintf(label,sizeof(label),"%02zu",i+1);draw->AddText(ImVec2(x+8*scale,y-7*scale),color,label);}
      }
    }
    const float end_x=x_at(end);draw->AddLine(ImVec2(end_x,origin.y),ImVec2(end_x,origin.y+size.y),IM_COL32(220,190,110,220),2*scale);draw->AddTriangleFilled(ImVec2(end_x-7*scale,origin.y+2*scale),ImVec2(end_x+7*scale,origin.y+2*scale),ImVec2(end_x,origin.y+14*scale),IM_COL32(220,190,110,255));hitpoint("##composition_end",end_x,origin.y+8*scale,End);
    if(stored.mode=="region")for(int kind:{In,Out}){const float x=x_at(kind==In?region_in:region_out),y=origin.y+size.y-8*scale;draw->AddTriangleFilled(ImVec2(x,y-6*scale),ImVec2(x-6*scale,y+6*scale),ImVec2(x+6*scale,y+6*scale),IM_COL32(235,235,235,255));hitpoint(kind==In?"##composition_in":"##composition_out",x,y,kind);}
    draw->AddLine(ImVec2(x_at(cursor),origin.y+ruler),ImVec2(x_at(cursor),origin.y+size.y-footer),IM_COL32(120,205,225,255),1.5f*scale);
    draw->PopClipRect();row=0;for(int lane=0;lane<3;++lane)if(lanes[lane]){const float y=origin.y+ruler+lane_height*(row+.5f);++row;draw->AddText(ImVec2(origin.x+8*scale,y-7*scale),IM_COL32(195,195,195,255),lane_names[lane]);}
    if(!count)draw->AddText(ImVec2(float(left)+12*scale,origin.y+ruler+18*scale),IM_COL32(175,175,175,255),"Enable a channel lane to show linked keys");
    // A visible marker may straddle the ruler edge. ImGui floors MousePos to
    // pixels, so a fractional-scale In=0 center can land just left of `left`.
    if(hover&&(mouse.x>=left||(hit_kind!=None&&mouse.x>=std::floor(left)))&&ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
      drag_.kind=hit_kind==None?Scrub:hit_kind;drag_.id=hit;drag_.index=hit_index;drag_.source=raw;drag_.mouse=mouse;drag_.original=drag_.time=drag_.kind==Key?project.keys[hit_index].time:drag_.kind==In?stored.in:drag_.kind==Out?stored.out:drag_.kind==End?project.duration:std::clamp(time_at(mouse.x),0.,project.duration);drag_.snap=snap;drag_.resize=resize;drag_.tick_rate=tick_rate;drag_.frame_rate=fps;
      if(drag_.kind==Key&&select)select(hit);
    }
    if(drag_.kind!=None){
      if(ImGui::IsKeyPressed(ImGuiKey_Escape,false)){ResetGesture();ImGui::ClearActiveID();}
      else if(ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        const double dx=mouse.x-drag_.mouse.x;drag_.moved=std::abs(dx)>3*scale;
        if(!drag_.moved&&drag_.kind!=Scrub)drag_.time=drag_.original;
        if(drag_.moved||drag_.kind==Scrub){double time=drag_.kind==Scrub?time_at(mouse.x):drag_.original+dx/width*view_span_;time=timeline_time::Snap(time,drag_.snap,drag_.tick_rate,drag_.frame_rate,ImGui::GetIO().KeyShift);
          if(drag_.kind==Key){const auto bounds=timeline_time::KeyTimeBounds(project,drag_.index);time=std::clamp(time,bounds.first,bounds.second);}
          else if(drag_.kind==End)time=std::clamp(time,timeline_time::MinimumDuration(raw,drag_.resize),86400.);
          else if(drag_.kind==In)time=std::clamp(time,0.,std::max(0.,stored.out-1e-6));
          else if(drag_.kind==Out)time=std::clamp(time,std::min(project.duration,stored.in+1e-6),project.duration);
          else time=std::clamp(time,0.,project.duration);
          drag_.time=time;
        }
      }else{
        if(send&&((drag_.moved&&std::abs(drag_.time-drag_.original)>1e-10)||drag_.kind==Scrub)){
          if(drag_.kind==Key)send("update_key",{{"id",drag_.id},{"time",drag_.time}});
          else if(drag_.kind==In||drag_.kind==Out)send("set_project",{{"render_range",{{drag_.kind==In?"in":"out",drag_.time}}}});
          else if(drag_.kind==End){try{send("set_project",timeline_time::ResizeProject(drag_.source,drag_.time,drag_.resize));error_.clear();}catch(const std::exception& e){error_=e.what();}}
          else if(std::abs(drag_.time-state.value("cursor",0.))>1e-10)send("scrub",{{"time",drag_.time},{"seek_demo",state.value("sync_demo",false)}});
        }ResetGesture();
      }
    }
    if(tips&&hover&&hit_kind!=None&&drag_.kind==None){char text[512];std::snprintf(text,sizeof(text),"%s | %s | Shift: no snap",hit_kind==Key?hit.c_str():hit_kind==End?"Composition end":hit_kind==In?"Region In":"Region Out",hit_kind==Key?"All three channels move together":hit_kind==End?"Keep protects key times; Scale retimes all keys":"Temporal render bound");tooltip(text);}
    draw->PopClipRect();ImGui::TextDisabled("Shared key times | Wheel zoom | MMB pan | Shift no snap | Esc cancel");
    if(!embedded)ImGui::End();
    if(!visible)ResetGesture();
  }
 private:
  enum Kind{None,Key,Scrub,In,Out,End};
  struct Drag{int kind=None;std::string id,snap,resize;std::size_t index=0;Json source;ImVec2 mouse{};double original=0,time=0,tick_rate=64,frame_rate=60;bool moved=false;}drag_;
  std::uint64_t document_=std::numeric_limits<std::uint64_t>::max();bool fitted_=false,duration_active_=false,height_active_=false,fps_active_=false;
  double view_start_=0,view_span_=1,duration_draft_=0,fps_draft_=60,duration_tick_rate_=64,duration_fps_=60;int height_draft_=240,height_seen_=-1;float scale_seen_=0;std::string error_,duration_unit_="seconds",duration_resize_="keep";
};
} // namespace cs2_live_ui
